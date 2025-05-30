/****************************************************************************
 *
 *       Copyright (c) 2015 Samsung Electronics Co., Ltd
 *
 ****************************************************************************/

/* MX BT shared memory interface */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/firmware.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/kthread.h>
#include <asm/io.h>
#include <linux/version.h>
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0))
#include <scsc/scsc_wakelock.h>
#else
#include <linux/wakelock.h>
#endif
#include <scsc/scsc_mx.h>
#include <scsc/scsc_mifram.h>
#include <scsc/api/bsmhcp.h>
#include <scsc/scsc_logring.h>

#ifdef CONFIG_SCSC_LOG_COLLECTION
#include <scsc/scsc_log_collector.h>
#endif

#include "scsc_bt_priv.h"
#include "scsc_shm.h"
#include "scsc_bt_hci.h"

#define NUMBER_OF_HCI_EVT       (BSMHCP_USED_ENTRIES(bt_service.mailbox_hci_evt_write, bt_service.mailbox_hci_evt_read, BSMHCP_TRANSFER_RING_EVT_SIZE))
#define NUMBER_OF_ACL_RX        (BSMHCP_USED_ENTRIES(bt_service.mailbox_acl_rx_write, bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE))

struct hci_credit_entry {
	u16 hci_connection_handle;
	u16 credits;
};

static u8   h4_write_buffer[DATA_PKTS_MAX_LEN];
static u8   h4_read_data_header_len;
static u8   h4_read_data_header[H4DMUX_HEADER_MAX_SIZE + ISO_DATA_LOAD_HEADER_SIZE]; /* Worst case */
static u8   h4_hci_event_ncp_header[HCI_EVENT_NCP_HEADER_LEN + MAX_NCP_ENTRIES * sizeof(struct hci_credit_entry)];
static u32  h4_hci_event_ncp_header_len = HCI_EVENT_NCP_HEADER_LEN + sizeof(struct hci_credit_entry);

static struct hci_credit_entry *h4_hci_credit_entries =
		(struct hci_credit_entry *)&h4_hci_event_ncp_header[HCI_EVENT_NCP_HEADER_LEN];

static u8   h4_hci_event_hardware_error[4] = { HCI_EVENT_PKT, HCI_EVENT_HARDWARE_ERROR_EVENT, 1, 0 };
static u8   h4_iq_report_evt[HCI_IQ_REPORT_MAX_LEN];
static u32  h4_iq_report_evt_len;
static u16  h4_irq_mask;

static bool scsc_read_data_available(void)
{
	if (bt_service.bsmhcp_protocol->header.mailbox_hci_evt_write !=
	    bt_service.bsmhcp_protocol->header.mailbox_hci_evt_read ||
	    bt_service.bsmhcp_protocol->header.mailbox_acl_rx_write !=
	    bt_service.bsmhcp_protocol->header.mailbox_acl_rx_read ||
	    bt_service.bsmhcp_protocol->header.mailbox_acl_free_write !=
	    bt_service.bsmhcp_protocol->header.mailbox_acl_free_read ||
	    bt_service.bsmhcp_protocol->header.mailbox_iq_report_write !=
	    bt_service.bsmhcp_protocol->header.mailbox_iq_report_read ||
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_write !=
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_read ||
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_write !=
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_read)
		return true;

	return false;
}

static void scsc_bt_shm_irq_handler(int irqbit, void *data)
{
	/* Clear interrupt */
	scsc_service_mifintrbit_bit_clear(bt_service.service, irqbit);

	/* Ensure irq bit is cleared before reading the mailbox indexes */
	mb();

	bt_service.interrupt_count++;

	/* Wake the reader operation */
	if (scsc_read_data_available() ||
	    atomic_read(&bt_service.error_count) != 0 ||
	    bt_service.bsmhcp_protocol->header.panic_deathbed_confession) {
		bt_service.interrupt_read_count++;
		wake_lock_timeout(&bt_service.read_wake_lock, HZ);
		wake_up(&bt_service.read_wait);
	}

	if (bt_service.bsmhcp_protocol->header.mailbox_hci_cmd_write ==
	    bt_service.bsmhcp_protocol->header.mailbox_hci_cmd_read &&
	    bt_service.bsmhcp_protocol->header.mailbox_acl_tx_write ==
	    bt_service.bsmhcp_protocol->header.mailbox_acl_tx_read &&
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_tx_write ==
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_tx_read) {
		bt_service.interrupt_write_count++;

		if (wake_lock_active(&bt_service.write_wake_lock)) {
			bt_service.write_wake_unlock_count++;
			wake_unlock(&bt_service.write_wake_lock);
		}
	}
}

static void scsc_bt_clear_paused_acl_rx(u16 conn_hdl)
{
	/* Adjust the index for reverse searching of acl_rx_transfer_ring */
	u32 search = 0;
	u32 dst = BSMHCP_PREV_INDEX(bt_service.mailbox_acl_rx_write, BSMHCP_TRANSFER_RING_ACL_SIZE);
	u32 stop = BSMHCP_PREV_INDEX(bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE);

	/* Clear all ACL data having data_paused_conn_hdl from acl_rx_transfer_ring */
	for (search = dst; search != stop;
	     search = BSMHCP_PREV_INDEX(search, BSMHCP_TRANSFER_RING_ACL_SIZE)) {
		if (conn_hdl != bt_service.bsmhcp_protocol->acl_rx_transfer_ring[search].header.hci_connection_handle) {
			if (search != dst)
				memcpy(&bt_service.bsmhcp_protocol->acl_rx_transfer_ring[dst],
				       &bt_service.bsmhcp_protocol->acl_rx_transfer_ring[search],
				       sizeof(struct BSMHCP_TD_ACL_RX));

			dst = BSMHCP_PREV_INDEX(dst, BSMHCP_TRANSFER_RING_ACL_SIZE);
		} else
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE);
	}
}

/* Assign firmware/host interrupts */
static int scsc_bt_shm_init_interrupt(void)
{
	int irq_ret = 0;
	u16 irq_num = 0;

	/* To-host f/w IRQ allocations and ISR registrations */
	irq_ret = scsc_service_mifintrbit_register_tohost(
	    bt_service.service, scsc_bt_shm_irq_handler, NULL, SCSC_MIFINTR_TARGET_WPAN);

	if (irq_ret < 0)
		return irq_ret;

	bt_service.bsmhcp_protocol->header.bg_to_ap_int_src = irq_ret;
	h4_irq_mask |= 1 << irq_num++;

	/* From-host f/w IRQ allocations */
	irq_ret = scsc_service_mifintrbit_alloc_fromhost(
	    bt_service.service, SCSC_MIFINTR_TARGET_WPAN);
	if (irq_ret < 0)
		return irq_ret;

	bt_service.bsmhcp_protocol->header.ap_to_bg_int_src = irq_ret;
	h4_irq_mask |= 1 << irq_num++;

	irq_ret = scsc_service_mifintrbit_alloc_fromhost(
	    bt_service.service, SCSC_MIFINTR_TARGET_WPAN);
	if (irq_ret < 0)
		return irq_ret;

	bt_service.bsmhcp_protocol->header.ap_to_fg_int_src = irq_ret;
	h4_irq_mask |= 1 << irq_num++;

	SCSC_TAG_DEBUG(BT_COMMON, "Registered to-host IRQ bits %d, from-host IRQ bits %d:%d\n",
		       bt_service.bsmhcp_protocol->header.bg_to_ap_int_src,
		       bt_service.bsmhcp_protocol->header.ap_to_bg_int_src,
		       bt_service.bsmhcp_protocol->header.ap_to_fg_int_src);

	return 0;
}

static void scsc_bt_big_handle_list_init(void)
{
	int i;

	for (i = 0; i < SCSC_BT_BIG_INFO_MAX; i++) {
		bt_service.big_handle_list[i].big_handle = BSMHCP_INVALID_BIG_HANDLE;
		bt_service.big_handle_list[i].num_of_connections = 0;
	}
}

static void scsc_bt_big_reset_connection_handles(u16 big_handle)
{
	int i;

	for (i = 0; i < SCSC_BT_CONNECTION_INFO_MAX; i++) {
		if (bt_service.connection_handle_list[i].state != CONNECTION_NONE &&
		    bt_service.connection_handle_list[i].big_handle == big_handle) {
			bt_service.connection_handle_list[i].big_handle = BSMHCP_INVALID_BIG_HANDLE;
			bt_service.connection_handle_list[i].state = CONNECTION_NONE;
		}
	}
}

bool scsc_bt_shm_h4_avdtp_detect_write(uint32_t flags, uint16_t l2cap_cid, uint16_t hci_connection_handle)
{
	uint32_t tr_read;
	uint32_t tr_write;
	struct BSMHCP_TD_AVDTP *td;

	spin_lock(&bt_service.avdtp_detect.fw_write_lock);

	/* Store the read/write pointer on the stack since both are placed in unbuffered/uncached memory */
	tr_read = bt_service.bsmhcp_protocol->header.mailbox_avdtp_read;
	tr_write = bt_service.bsmhcp_protocol->header.mailbox_avdtp_write;

	td = &bt_service.bsmhcp_protocol->avdtp_transfer_ring[tr_write];

	SCSC_TAG_DEBUG(BT_H4,
		       "AVDTP_DETECT_PKT (flags: 0x%08X, cid: 0x%04X, handle: 0x%04X, read=%u, write=%u)\n",
		       flags, l2cap_cid, hci_connection_handle, tr_read, tr_write);

	/* Index out of bounds check */
	if (tr_read >= BSMHCP_TRANSFER_RING_AVDTP_SIZE || tr_write >= BSMHCP_TRANSFER_RING_AVDTP_SIZE) {
		spin_unlock(&bt_service.avdtp_detect.fw_write_lock);
		SCSC_TAG_ERR(BT_H4,
			     "AVDTP_DETECT_PKT - Index out of bounds (tr_read=%u, tr_write=%u)\n",
			     tr_read, tr_write);
		atomic_inc(&bt_service.error_count);
		return false;
	}

	/* Does the transfer ring have room for an entry */
	if (BSMHCP_HAS_ROOM(tr_write, tr_read, BSMHCP_TRANSFER_RING_AVDTP_SIZE)) {
		/* Fill the transfer descriptor with the AVDTP data */
		td->flags = flags;
		td->l2cap_cid = l2cap_cid;
		td->hci_connection_handle = hci_connection_handle;

		/* Ensure the wake lock is acquired */
		if (!wake_lock_active(&bt_service.write_wake_lock)) {
			bt_service.write_wake_lock_count++;
			wake_lock(&bt_service.write_wake_lock);
		}

		/* Increate the write pointer */
		BSMHCP_INCREASE_INDEX(tr_write, BSMHCP_TRANSFER_RING_AVDTP_SIZE);
		bt_service.bsmhcp_protocol->header.mailbox_avdtp_write = tr_write;

		spin_unlock(&bt_service.avdtp_detect.fw_write_lock);

		/* Memory barrier to ensure out-of-order execution is completed */
		wmb();

		/* Trigger the interrupt in the mailbox */
		scsc_service_mifintrbit_bit_set(
			bt_service.service,
			bt_service.bsmhcp_protocol->header.ap_to_bg_int_src,
			SCSC_MIFINTR_TARGET_WPAN);
	} else {
		/* Transfer ring full */
		spin_unlock(&bt_service.avdtp_detect.fw_write_lock);
		SCSC_TAG_ERR(BT_H4,
			     "AVDTP_DETECT_PKT - No more room for messages (tr_read=%u, tr_write=%u)\n",
			     tr_read, tr_write);

		scsc_service_force_panic(bt_service.service);
		return false;
	}
	return true;
}

static ssize_t scsc_bt_shm_h4_hci_cmd_write(const unsigned char *data, size_t count)
{
	/* Store the read/write pointer on the stack since both are placed in unbuffered/uncached memory */
	uint32_t tr_read = bt_service.bsmhcp_protocol->header.mailbox_hci_cmd_read;
	uint32_t tr_write = bt_service.bsmhcp_protocol->header.mailbox_hci_cmd_write;
#ifdef CONFIG_SCSC_PRINTK
	uint16_t op_code = *(uint16_t *)data;
#endif

	/* Temp vars */
	struct BSMHCP_TD_CONTROL *td = &bt_service.bsmhcp_protocol->hci_cmd_transfer_ring[tr_write];

	SCSC_TAG_DEBUG(BT_H4,
		       "HCI_COMMAND_PKT (op_code=0x%04x, len=%zu, read=%u, write=%u)\n",
		       op_code, count, tr_read, tr_write);

	/* Index out of bounds check */
	if (tr_read >= BSMHCP_TRANSFER_RING_CMD_SIZE || tr_write >= BSMHCP_TRANSFER_RING_CMD_SIZE) {
		SCSC_TAG_ERR(BT_H4,
			     "HCI_COMMAND_PKT - Index out of bounds (tr_read=%u, tr_write=%u)\n",
			     tr_read, tr_write);

		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* Does the transfer ring have room for an entry */
	if (BSMHCP_HAS_ROOM(tr_write, tr_read, BSMHCP_TRANSFER_RING_CMD_SIZE)) {
		/* Fill the transfer descriptor with the HCI command data */
		memcpy(td->data, data, count);
		td->length = (u16)count;

		/* Ensure the wake lock is acquired */
		if (!wake_lock_active(&bt_service.write_wake_lock)) {
			bt_service.write_wake_lock_count++;
			wake_lock(&bt_service.write_wake_lock);
		}

		/* Increate the write pointer */
		BSMHCP_INCREASE_INDEX(tr_write, BSMHCP_TRANSFER_RING_CMD_SIZE);
		bt_service.bsmhcp_protocol->header.mailbox_hci_cmd_write = tr_write;

		/* Memory barrier to ensure out-of-order execution is completed */
		wmb();

		/* Trigger the interrupt in the mailbox */
		scsc_service_mifintrbit_bit_set(bt_service.service,
						bt_service.bsmhcp_protocol->header.ap_to_bg_int_src,
						SCSC_MIFINTR_TARGET_WPAN);
	} else
		/* Transfer ring full. Only happens if the user attempt to send more HCI command packets than
		 * available credits
		 */
		count = 0;

	return count;
}

static ssize_t scsc_bt_shm_h4_acl_write(const unsigned char *data, size_t count)
{
	/* Store the read/write pointer on the stack since both are placed in unbuffered/uncached memory */
	uint32_t tr_read = bt_service.bsmhcp_protocol->header.mailbox_acl_tx_read;
	uint32_t tr_write = bt_service.bsmhcp_protocol->header.mailbox_acl_tx_write;

	/* Temp vars */
	struct BSMHCP_TD_ACL_TX_DATA *td = &bt_service.bsmhcp_protocol->acl_tx_data_transfer_ring[tr_write];
	int acldata_buf_index = -1;
	u16 l2cap_length;
	u32 i;
	size_t payload_len = count - ACLDATA_HEADER_SIZE;

	/* Index out of bounds check */
	if (tr_read >= BSMHCP_TRANSFER_RING_ACL_SIZE || tr_write >= BSMHCP_TRANSFER_RING_ACL_SIZE) {
		SCSC_TAG_ERR(BT_H4,
			     "ACL_DATA_PKT - Index out of bounds (tr_read=%u, tr_write=%u)\n",
			     tr_read, tr_write);
		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* Allocate a data slot */
	for (i = 0; i < BSMHCP_DATA_BUFFER_TX_ACL_SIZE; i++) {
		/* Wrap the offset index around the buffer max */
		if (++bt_service.last_alloc == BSMHCP_DATA_BUFFER_TX_ACL_SIZE)
			bt_service.last_alloc = 0;
		/* Claim a free slot */
		if (bt_service.allocated[bt_service.last_alloc] == 0) {
			bt_service.allocated[bt_service.last_alloc] = 1;
			acldata_buf_index = bt_service.last_alloc;
			bt_service.allocated_count++;
			break;
		}
	}

	/* Is a buffer available to hold the data */
	if (acldata_buf_index < 0) {
		SCSC_TAG_ERR(BT_H4, "ACL_DATA_PKT - No buffers available\n");
		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* Does the transfer ring have room for an entry */
	if (BSMHCP_HAS_ROOM(tr_write, tr_read, BSMHCP_TRANSFER_RING_ACL_SIZE)) {
		/* Extract the ACL data header and L2CAP header and fill it into the transfer descriptor */
		uint8_t pb_flag;
		uint16_t conn_hdl;

		td->header.buffer_index = (uint8_t)acldata_buf_index;
		td->header.flags = HCI_ACL_DATA_GET_FLAGS(data);
		td->header.hci_connection_handle = HCI_ACL_DATA_GET_CON_HDL(data);
		td->header.length = (uint16_t)payload_len;
		pb_flag = td->header.flags & BSMHCP_ACL_PB_FLAG_MASK;
		conn_hdl = td->header.hci_connection_handle;

		if ((td->header.flags & BSMHCP_ACL_BC_FLAG_BCAST_ACTIVE) ||
		    (td->header.flags & BSMHCP_ACL_BC_FLAG_BCAST_ALL)) {
			SCSC_TAG_DEBUG(BT_H4, "Setting broadcast handle (hci_connection_handle=0x%03x)\n", conn_hdl);

			bt_service.connection_handle_list[conn_hdl].state = CONNECTION_ACTIVE;
			bt_service.connection_handle_list[conn_hdl].big_handle = BSMHCP_INVALID_BIG_HANDLE;
		}

		/* Is this a packet marked with the start flag */
		if (pb_flag == BSMHCP_ACL_PB_FLAG_START_NONFLUSH || pb_flag == BSMHCP_ACL_PB_FLAG_START_FLUSH) {
			/* Extract the L2CAP payload length and connection identifier */
			td->header.l2cap_cid = HCI_L2CAP_GET_CID(data);

			/* data + 4 to skip the HCI header, to align offsets with the rx detection. The "true"
			 * argument is to tell the detection that this is TX
			 */
			scsc_avdtp_detect_rxtx(conn_hdl, data + 4, td->header.length, true);

			l2cap_length = HCI_L2CAP_GET_LENGTH(data);

			SCSC_TAG_DEBUG(BT_TX,
				    "ACL[START] (len=%u, buffer=%u, credits=%u, l2cap_cid=0x%04x, l2cap_length=%u)\n",
				    td->header.length, acldata_buf_index,
				    GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ACL_SIZE,
							     bt_service.allocated_count,
							     bt_service.freed_count),
				    td->header.l2cap_cid, l2cap_length);

			if (l2cap_length == payload_len - L2CAP_HEADER_SIZE)
				/* Mark it with the END flag if packet length matches the L2CAP payload length */
				td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;
			else if (l2cap_length < payload_len - L2CAP_HEADER_SIZE) {
				/* Mark it with the END flag if packet length is greater than the L2CAP payload length
				 * and generate a warning notifying that this is incorrect according to the
				 * specification. This is allowed to support the BITE tester.
				 */
				SCSC_TAG_WARNING(BT_H4,
					"ACL_DATA_PKT - H4 ACL payload length %zu > L2CAP Length %u\n",
					payload_len - L2CAP_HEADER_SIZE, l2cap_length);

				td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;
			} else if (l2cap_length > (payload_len - L2CAP_HEADER_SIZE)) {
				/* This is only a fragment of the packet. Save the remaining number of octets required
				 * to complete the packet
				 */
				bt_service.connection_handle_list[conn_hdl].remaining_length =
					(u16)(l2cap_length - payload_len + L2CAP_HEADER_SIZE);

				bt_service.connection_handle_list[conn_hdl].l2cap_cid = HCI_L2CAP_GET_CID(data);
			} else {
				/* The packet is larger than the L2CAP payload length - protocol error */
				SCSC_TAG_ERR(BT_H4,
					     "ACL_DATA_PKT - L2CAP Length Error (l2cap_length=%u, payload_len=%zu)\n",
					     l2cap_length, payload_len - L2CAP_HEADER_SIZE);

				atomic_inc(&bt_service.error_count);
				return -EIO;
			}
		} else if (pb_flag == BSMHCP_ACL_PB_FLAG_CONT) {
			/* Set the L2CAP connection identifer set by the start packet */
			u16 remaining_length = bt_service.connection_handle_list[conn_hdl].remaining_length;

			td->header.l2cap_cid = bt_service.connection_handle_list[conn_hdl].l2cap_cid;

			SCSC_TAG_DEBUG(BT_TX,
				       "ACL[CONT] (len=%u, buffer=%u, credits=%u, l2cap_cid=0x%04x, length=%u)\n",
				       td->header.length, acldata_buf_index,
				       GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ACL_SIZE,
								bt_service.allocated_count,
								bt_service.freed_count),
				       bt_service.connection_handle_list[conn_hdl].l2cap_cid,
				       remaining_length);

			/* Does this packet complete the L2CAP frame */
			if (remaining_length == payload_len) {
				/* The L2CAP frame is complete. mark it with the END flag */
				td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;

				/* Set the remaining length to zero */
				bt_service.connection_handle_list[conn_hdl].remaining_length = 0;
			} else if (remaining_length < payload_len) {
				/* Mark it with the END flag if packet length is greater than the L2CAP missing
				 * payload length and generate a warning notifying that this is incorrect according
				 * to the specification. This is allowed to support the BITE tester.
				 */
				SCSC_TAG_WARNING(BT_H4,
					"ACL_DATA_PKT - H4 ACL payload length %zu > L2CAP Missing Length %u\n",
					payload_len,
					remaining_length);

				td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;
				/* Set the remaining length to zero */
				bt_service.connection_handle_list[conn_hdl].remaining_length = 0;
			} else if (remaining_length > payload_len)
				/* This is another fragment of the packet. Save the remaining number of octets required
				 * to complete the packet
				 */
				bt_service.connection_handle_list[conn_hdl].remaining_length -= (u16)payload_len;
			else if (remaining_length < payload_len) {
				/* The packet is larger than the L2CAP payload length - protocol error */
				SCSC_TAG_ERR(BT_H4,
					     "ACL_DATA_PKT - L2CAP Length Error (missing=%u, payload_len=%zu)\n",
					     remaining_length, payload_len);

				atomic_inc(&bt_service.error_count);
				return -EIO;
			}
		} else {
			/* Reserved flags set - report it as an error */
			SCSC_TAG_ERR(BT_H4, "ACL_DATA_PKT - Flag set to reserved\n");
			atomic_inc(&bt_service.error_count);
			return -EIO;
		}

		SCSC_TAG_DEBUG(BT_H4,
			"ACL_DATA_PKT (len=%zu, read=%u, write=%u, slot=%u, flags=0x%04x, handle=0x%03x, l2cap_cid=0x%04x, missing=%u)\n",
			payload_len, tr_read, tr_write, acldata_buf_index, td->header.flags >> 4,
			conn_hdl, td->header.l2cap_cid,
			bt_service.connection_handle_list[conn_hdl].remaining_length);

		/* Ensure the wake lock is acquired */
		if (!wake_lock_active(&bt_service.write_wake_lock)) {
			bt_service.write_wake_lock_count++;
			wake_lock(&bt_service.write_wake_lock);
		}

		/* Copy the ACL packet into the targer buffer */
		memcpy(&bt_service.bsmhcp_protocol->acl_tx_buffer[acldata_buf_index][0],
		       &data[ACLDATA_HEADER_SIZE],
		       payload_len);

		/* Increate the write pointer */
		BSMHCP_INCREASE_INDEX(tr_write, BSMHCP_TRANSFER_RING_ACL_SIZE);
		bt_service.bsmhcp_protocol->header.mailbox_acl_tx_write = tr_write;

		/* Memory barrier to ensure out-of-order execution is completed */
		wmb();

		/* Trigger the interrupt in the mailbox */
		scsc_service_mifintrbit_bit_set(bt_service.service,
						bt_service.bsmhcp_protocol->header.ap_to_fg_int_src,
						SCSC_MIFINTR_TARGET_WPAN);
	} else {
		/* Transfer ring full. Only happens if the user attempt to send more ACL data packets than
		 * available credits
		 */
		SCSC_TAG_ERR(BT_H4,
			     "ACL_DATA_PKT - No room in transfer ring (tr_write=%u, tr_read=%u)\n",
			     tr_write, tr_read);

		atomic_inc(&bt_service.error_count);
		count = -EIO;
	}

	return count;
}

static ssize_t scsc_bt_shm_h4_iso_write(const unsigned char *data, size_t count)
{
	uint8_t pb_flag;
	uint16_t conn_hdl;
	/* Store the read/write pointer on the stack since both are placed in
	 * unbuffered/uncached memory
	 */
	uint32_t tr_read = bt_service.bsmhcp_protocol->header_2.mailbox_iso_tx_read;
	uint32_t tr_write = bt_service.bsmhcp_protocol->header_2.mailbox_iso_tx_write;

	struct BSMHCP_TD_ISO_TX_DATA *td = NULL;
	int isodata_buf_index = -1;
	uint16_t data_load_header_len = 0;
	int i;

	/* Index out of bounds check */
	if (tr_read >= BSMHCP_TRANSFER_RING_ISO_TX_SIZE || tr_write >= BSMHCP_TRANSFER_RING_ISO_TX_SIZE) {
		SCSC_TAG_ERR(BT_H4,
			     "ISO_DATA_PKT - Index out of bounds (tr_read=%u, tr_write=%u)\n",
			     tr_read, tr_write);

		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* Allocate a data slot */
	for (i = 0; i < BSMHCP_DATA_BUFFER_TX_ISO_SIZE; i++) {
		/* Wrap the offset index around the buffer max */
		if (++bt_service.iso_last_alloc == BSMHCP_DATA_BUFFER_TX_ISO_SIZE)
			bt_service.iso_last_alloc = 0;
		/* Claim a free slot */
		if (bt_service.iso_allocated[bt_service.iso_last_alloc] == 0) {
			bt_service.iso_allocated[bt_service.iso_last_alloc] = 1;
			isodata_buf_index = bt_service.iso_last_alloc;
			bt_service.iso_allocated_count++;
			break;
		}
	}

	/* Is a buffer available to hold the data */
	if (isodata_buf_index < 0) {
		SCSC_TAG_ERR(BT_H4, "ISO_DATA_PKT - No buffers available\n");
		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* Does the transfer ring have room for an entry */
	if (!BSMHCP_HAS_ROOM(tr_write, tr_read, BSMHCP_TRANSFER_RING_ISO_TX_SIZE)) {
		/* Transfer ring full. Only happens if the user attempt to send more
		 * ISO data packets than available credits
		 */
		SCSC_TAG_ERR(BT_H4,
			     "ISO_DATA_PKT - No room in transfer ring (tr_write=%u, tr_read=%u)\n",
			     tr_write, tr_read);
		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	/* The transfer ring has room for a entry. Extract the ISO data header and
	 * fill it into the transfer descriptor
	 */
	td = &bt_service.bsmhcp_protocol->iso_tx_data_transfer_ring[tr_write];

	td->header.length = (uint16_t)(count - ISODATA_HEADER_SIZE);
	td->header.buffer_index = (uint8_t)isodata_buf_index;
	td->header.flags = HCI_ISO_DATA_GET_FLAGS(data);
	td->header.hci_connection_handle = HCI_ISO_DATA_GET_CON_HDL(data);
	conn_hdl = td->header.hci_connection_handle;
	pb_flag = td->header.flags & BSMHCP_ISO_PB_FLAG_MASK;


	/* The Time_Stamp, Packet_Sequence_Number and ISO_SDU_Length fields
	 * are only included in the HCI ISO data packet when the PB_Flag equals
	 * BSMHCP_ISO_PB_FLAG_FIRST (0b00) or BSMHCP_ISO_PB_FLAG_COMPLETE (0b10)
	 */
	if (pb_flag == BSMHCP_ISO_PB_FLAG_FIRST || pb_flag == BSMHCP_ISO_PB_FLAG_COMPLETE) {
		u8 offset = 0;

		data_load_header_len = ISO_PKT_SEQ_NUM_SIZE + ISO_PKT_SDU_LENGTH_SIZE;

		/* Is the Time stamp include */
		if ((td->header.flags & BSMHCP_ISO_TS_FLAG_MASK) == BSMHCP_ISO_TS_FLAG_TS_INCL) {
			data_load_header_len += ISO_TIMESTAMP_SIZE;
			offset = ISO_TIMESTAMP_SIZE;
		}

		if (td->header.length >= data_load_header_len) {
			/* Subtract the length of the additional headers in the ISO Data load field
			 * to get the 'real' payload_len
			 */
			td->header.length -= data_load_header_len;

			if ((td->header.flags & BSMHCP_ISO_TS_FLAG_MASK) == BSMHCP_ISO_TS_FLAG_TS_INCL)
				td->time_stamp = HCI_ISO_DATA_GET_TIMESTAMP(data);
		} else {
			/* The ISO_Data_Load field shall contain a header - protocol error */
			SCSC_TAG_ERR(BT_H4,
				"ISO[FIRST] - Length Error (data_load_header_len=%u, packet_len=%u)\n",
				data_load_header_len, td->header.length);

			atomic_inc(&bt_service.error_count);
			return -EIO;
		}

		/* Extract Packet Sequence Number and SDU length */
		td->packet_sequence_number = HCI_ISO_DATA_GET_PKT_SEQ_NUM(data, offset);
		td->sdu_length = HCI_ISO_DATA_GET_SDU_LENGTH(data, offset);

		if (pb_flag == BSMHCP_ISO_PB_FLAG_FIRST) {
			/* The ISO Data Load field contains the first fragment of a fragmented SDU */
			SCSC_TAG_DEBUG(BT_TX,
				"ISO[FIRST] (payload_len=%u, buffer=%u, credits=%u, sdu_length=%u, header_length=%u)\n",
				td->header.length, td->header.buffer_index,
				GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ISO_SIZE,
							 bt_service.iso_allocated_count,
							 bt_service.iso_freed_count),
				td->sdu_length, data_load_header_len);

			if (td->sdu_length > td->header.length) {
				bt_service.connection_handle_list[conn_hdl].remaining_length =
					td->sdu_length - td->header.length;
			} else {
				/* The SDU length field shall be greater than the payload len - protocol error */
				SCSC_TAG_ERR(BT_H4,
					     "ISO[FIRST] - SDU Length Error (sdu_length=%u, payload_len=%u)\n",
					     td->sdu_length, td->header.length);

				atomic_inc(&bt_service.error_count);
				return -EIO;
			}
		} else {
			/* The ISO Data Load field contains a complete SDU */
			SCSC_TAG_DEBUG(BT_TX,
				"ISO[COMP] (payload_len=%u, buffer=%u, credits=%u, sdu_length=%u, header_length=%u)\n",
				td->header.length, td->header.buffer_index,
				GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ISO_SIZE,
							 bt_service.iso_allocated_count,
							 bt_service.iso_freed_count),
				td->sdu_length, data_load_header_len);

			if (td->sdu_length == td->header.length) {
				/* Mark it with the END flag as the payload length matches the ISO SDU length */
				td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;
				bt_service.connection_handle_list[conn_hdl].remaining_length = 0;
			} else {
				/* The SDU length field not equals the payload length - protocol error */
				SCSC_TAG_ERR(BT_H4,
					     "ISO[COMP] - SDU Length Error (sdu_length=%u, payload_len=%u)\n",
					     td->sdu_length, td->header.length);

				atomic_inc(&bt_service.error_count);
				return -EIO;
			}
		}
	} else if (pb_flag == BSMHCP_ISO_PB_FLAG_CONTINUE) {
		/* The ISO_Data_Load field contains a continuation fragment of an SDU */
		SCSC_TAG_DEBUG(BT_TX,
			       "ISO[CONT] (payload_len=%u, buffer=%u, credits=%u, length=%u)\n",
			       td->header.length, td->header.buffer_index,
			       GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ISO_SIZE,
							bt_service.iso_allocated_count,
							bt_service.iso_freed_count),
			       bt_service.connection_handle_list[conn_hdl].remaining_length);

		if (bt_service.connection_handle_list[conn_hdl].remaining_length >= td->header.length) {
			/* This is another fragment of the packet. Save the remaining number of octets required
			 * to complete the packet.
			 */
			bt_service.connection_handle_list[conn_hdl].remaining_length -= td->header.length;
		} else {
			SCSC_TAG_ERR(BT_H4,
				"ISO[CONT] - SDU Length Error (missing=%u, payload_len=%u)\n",
				bt_service.connection_handle_list[conn_hdl].remaining_length,
				td->header.length);

			atomic_inc(&bt_service.error_count);
			return -EIO;
		}
	} else if (pb_flag == BSMHCP_ISO_PB_FLAG_LAST) {
		/* The ISO_Data_Load field contains the last fragment of an SDU */
		SCSC_TAG_DEBUG(BT_TX,
			       "ISO[LAST] (payload_len=%u, buffer=%u, credits=%u, length=%u)\n",
			       td->header.length, td->header.buffer_index,
			       GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ISO_SIZE,
							bt_service.iso_allocated_count,
							bt_service.iso_freed_count),
			       bt_service.connection_handle_list[conn_hdl].remaining_length);

		if (bt_service.connection_handle_list[conn_hdl].remaining_length == td->header.length) {
			/* Mark it with the END flag as the length matches the ISO SDU length */
			td->header.flags |= BSMHCP_ACL_L2CAP_FLAG_END;
			bt_service.connection_handle_list[conn_hdl].remaining_length = 0;
		} else {
			SCSC_TAG_ERR(BT_H4,
				"ISO[LAST] - SDU Length Error (missing=%u, payload_len=%u)\n",
				bt_service.connection_handle_list[conn_hdl].remaining_length,
				td->header.length);

			atomic_inc(&bt_service.error_count);
			return -EIO;
		}
	} else {
		/* Reserved flags set - report it as an error */
		SCSC_TAG_ERR(BT_H4, "ISO_DATA_PKT - PB Flag set to reserved value\n");

		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	SCSC_TAG_DEBUG(BT_H4,
		"ISO_DATA_PKT (payload_len=%u, read=%u, write=%u, slot=%u, flags=0x%04x, handle=0x%03x, missing=%u)\n",
		td->header.length, tr_read, tr_write, td->header.buffer_index, td->header.flags >> 4,
		conn_hdl, bt_service.connection_handle_list[conn_hdl].remaining_length);

	/* Ensure the wake lock is acquired */
	if (!wake_lock_active(&bt_service.write_wake_lock)) {
		bt_service.write_wake_lock_count++;
		wake_lock(&bt_service.write_wake_lock);
	}

	/* Copy the ISO packet into the target buffer */
	memcpy(&bt_service.bsmhcp_protocol->iso_tx_buffer[td->header.buffer_index][0],
	       &data[ISODATA_HEADER_SIZE + data_load_header_len],
	       td->header.length);

	/* Increate the write pointer */
	BSMHCP_INCREASE_INDEX(tr_write, BSMHCP_TRANSFER_RING_ISO_TX_SIZE);
	bt_service.bsmhcp_protocol->header_2.mailbox_iso_tx_write = tr_write;

	/* Memory barrier to ensure out-of-order execution is completed */
	wmb();

	/* Trigger the interrupt in the mailbox */
	scsc_service_mifintrbit_bit_set(bt_service.service,
					bt_service.bsmhcp_protocol->header.ap_to_fg_int_src,
					SCSC_MIFINTR_TARGET_WPAN);

	return count;
}

#ifdef CONFIG_SCSC_PRINTK
static const char *scsc_hci_evt_decode_event_code(u8 hci_event_code, u8 hci_ulp_sub_code)
{
	const char *ret = "NA";

	switch (hci_event_code) {
	HCI_EV_DECODE(HCI_EV_INQUIRY_COMPLETE);
	HCI_EV_DECODE(HCI_EV_INQUIRY_RESULT);
	HCI_EV_DECODE(HCI_EV_CONN_COMPLETE);
	HCI_EV_DECODE(HCI_EV_CONN_REQUEST);
	HCI_EV_DECODE(HCI_EV_DISCONNECT_COMPLETE);
	HCI_EV_DECODE(HCI_EV_AUTH_COMPLETE);
	HCI_EV_DECODE(HCI_EV_REMOTE_NAME_REQ_COMPLETE);
	HCI_EV_DECODE(HCI_EV_ENCRYPTION_CHANGE);
	HCI_EV_DECODE(HCI_EV_CHANGE_CONN_LINK_KEY_COMPLETE);
	HCI_EV_DECODE(HCI_EV_MASTER_LINK_KEY_COMPLETE);
	HCI_EV_DECODE(HCI_EV_READ_REM_SUPP_FEATURES_COMPLETE);
	HCI_EV_DECODE(HCI_EV_READ_REMOTE_VER_INFO_COMPLETE);
	HCI_EV_DECODE(HCI_EV_QOS_SETUP_COMPLETE);
	HCI_EV_DECODE(HCI_EV_COMMAND_COMPLETE);
	HCI_EV_DECODE(HCI_EV_COMMAND_STATUS);
	HCI_EV_DECODE(HCI_EV_HARDWARE_ERROR);
	HCI_EV_DECODE(HCI_EV_FLUSH_OCCURRED);
	HCI_EV_DECODE(HCI_EV_ROLE_CHANGE);
	HCI_EV_DECODE(HCI_EV_NUMBER_COMPLETED_PKTS);
	HCI_EV_DECODE(HCI_EV_MODE_CHANGE);
	HCI_EV_DECODE(HCI_EV_RETURN_LINK_KEYS);
	HCI_EV_DECODE(HCI_EV_PIN_CODE_REQ);
	HCI_EV_DECODE(HCI_EV_LINK_KEY_REQ);
	HCI_EV_DECODE(HCI_EV_LINK_KEY_NOTIFICATION);
	HCI_EV_DECODE(HCI_EV_LOOPBACK_COMMAND);
	HCI_EV_DECODE(HCI_EV_DATA_BUFFER_OVERFLOW);
	HCI_EV_DECODE(HCI_EV_MAX_SLOTS_CHANGE);
	HCI_EV_DECODE(HCI_EV_READ_CLOCK_OFFSET_COMPLETE);
	HCI_EV_DECODE(HCI_EV_CONN_PACKET_TYPE_CHANGED);
	HCI_EV_DECODE(HCI_EV_QOS_VIOLATION);
	HCI_EV_DECODE(HCI_EV_PAGE_SCAN_MODE_CHANGE);
	HCI_EV_DECODE(HCI_EV_PAGE_SCAN_REP_MODE_CHANGE);
	HCI_EV_DECODE(HCI_EV_FLOW_SPEC_COMPLETE);
	HCI_EV_DECODE(HCI_EV_INQUIRY_RESULT_WITH_RSSI);
	HCI_EV_DECODE(HCI_EV_READ_REM_EXT_FEATURES_COMPLETE);
	HCI_EV_DECODE(HCI_EV_FIXED_ADDRESS);
	HCI_EV_DECODE(HCI_EV_ALIAS_ADDRESS);
	HCI_EV_DECODE(HCI_EV_GENERATE_ALIAS_REQ);
	HCI_EV_DECODE(HCI_EV_ACTIVE_ADDRESS);
	HCI_EV_DECODE(HCI_EV_ALLOW_PRIVATE_PAIRING);
	HCI_EV_DECODE(HCI_EV_ALIAS_ADDRESS_REQ);
	HCI_EV_DECODE(HCI_EV_ALIAS_NOT_RECOGNISED);
	HCI_EV_DECODE(HCI_EV_FIXED_ADDRESS_ATTEMPT);
	HCI_EV_DECODE(HCI_EV_SYNC_CONN_COMPLETE);
	HCI_EV_DECODE(HCI_EV_SYNC_CONN_CHANGED);
	HCI_EV_DECODE(HCI_EV_SNIFF_SUB_RATE);
	HCI_EV_DECODE(HCI_EV_EXTENDED_INQUIRY_RESULT);
	HCI_EV_DECODE(HCI_EV_ENCRYPTION_KEY_REFRESH_COMPLETE);
	HCI_EV_DECODE(HCI_EV_IO_CAPABILITY_REQUEST);
	HCI_EV_DECODE(HCI_EV_IO_CAPABILITY_RESPONSE);
	HCI_EV_DECODE(HCI_EV_USER_CONFIRMATION_REQUEST);
	HCI_EV_DECODE(HCI_EV_USER_PASSKEY_REQUEST);
	HCI_EV_DECODE(HCI_EV_REMOTE_OOB_DATA_REQUEST);
	HCI_EV_DECODE(HCI_EV_SIMPLE_PAIRING_COMPLETE);
	HCI_EV_DECODE(HCI_EV_LST_CHANGE);
	HCI_EV_DECODE(HCI_EV_ENHANCED_FLUSH_COMPLETE);
	HCI_EV_DECODE(HCI_EV_USER_PASSKEY_NOTIFICATION);
	HCI_EV_DECODE(HCI_EV_KEYPRESS_NOTIFICATION);
	HCI_EV_DECODE(HCI_EV_REM_HOST_SUPPORTED_FEATURES);
	HCI_EV_DECODE(HCI_EV_TRIGGERED_CLOCK_CAPTURE);
	HCI_EV_DECODE(HCI_EV_SYNCHRONIZATION_TRAIN_COMPLETE);
	HCI_EV_DECODE(HCI_EV_SYNCHRONIZATION_TRAIN_RECEIVED);
	HCI_EV_DECODE(HCI_EV_CSB_RECEIVE);
	HCI_EV_DECODE(HCI_EV_CSB_TIMEOUT);
	HCI_EV_DECODE(HCI_EV_TRUNCATED_PAGE_COMPLETE);
	HCI_EV_DECODE(HCI_EV_SLAVE_PAGE_RESPONSE_TIMEOUT);
	HCI_EV_DECODE(HCI_EV_CSB_CHANNEL_MAP_CHANGE);
	HCI_EV_DECODE(HCI_EV_INQUIRY_RESPONSE_NOTIFICATION);
	HCI_EV_DECODE(HCI_EV_AUTHENTICATED_PAYLOAD_TIMEOUT_EXPIRED);
	case HCI_EV_ULP:
	{
		switch (hci_ulp_sub_code) {
		HCI_EV_DECODE(HCI_EV_ULP_CONNECTION_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_ADVERTISING_REPORT);
		HCI_EV_DECODE(HCI_EV_ULP_CONNECTION_UPDATE_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_READ_REMOTE_USED_FEATURES_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_LONG_TERM_KEY_REQUEST);
		HCI_EV_DECODE(HCI_EV_ULP_REMOTE_CONNECTION_PARAMETER_REQUEST);
		HCI_EV_DECODE(HCI_EV_ULP_DATA_LENGTH_CHANGE);
		HCI_EV_DECODE(HCI_EV_ULP_READ_LOCAL_P256_PUB_KEY_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_GENERATE_DHKEY_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_ENHANCED_CONNECTION_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_DIRECT_ADVERTISING_REPORT);
		HCI_EV_DECODE(HCI_EV_ULP_PHY_UPDATE_COMPLETE);
		HCI_EV_DECODE(HCI_EV_ULP_USED_CHANNEL_SELECTION);
		}
		break;
	}
	}

	return ret;
}
#endif

static ssize_t scsc_iq_report_evt_read(char __user *buf, size_t len)
{
	ssize_t consumed = 0;
	ssize_t ret = 0;

	/* Calculate the amount of data that can be transferred */
	len = min(h4_iq_report_evt_len - bt_service.read_offset, len);

	SCSC_TAG_DEBUG(BT_H4,
		       "SCSC_IQ_REPORT_EVT_READ: td(h4_iq_len=%u offset=%u)\n",
		       h4_iq_report_evt_len, bt_service.read_offset);

	/* Copy the data to the user buffer */
	ret = copy_to_user(buf, &h4_iq_report_evt[bt_service.read_offset], len);
	if (ret == 0) {
		/* All good - Update our consumed information */
		bt_service.read_offset += len;
		consumed = len;

		SCSC_TAG_DEBUG(BT_H4,
			       "SCSC_IQ_REPORT_EVT_READ: (offset=%u consumed: %u)\n",
			       bt_service.read_offset, consumed);

		/* Have all data been copied to the userspace buffer */
		if (bt_service.read_offset == h4_iq_report_evt_len) {
			/* All good - read operation is completed */
			bt_service.read_offset = 0;
			bt_service.read_operation = BT_READ_OP_NONE;
		}
	} else {
		SCSC_TAG_ERR(BT_H4, "copy_to_user returned: %zu\n", ret);
		ret = -EACCES;
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_hci_evt_read(char __user *buf, size_t len)
{
	struct BSMHCP_TD_HCI_EVT *td = &bt_service.bsmhcp_protocol->hci_evt_transfer_ring[bt_service.read_index];
	u8                       h4_hci_event_header = HCI_EVENT_PKT;
	ssize_t                  consumed = 0;
	ssize_t                  ret = 0;

	SCSC_TAG_DEBUG(BT_H4,
		       "td (length=%u, hci_connection_handle=0x%03x, event_type=%u), len=%zu, read_offset=%zu\n",
		       td->length, td->hci_connection_handle, BSMHCP_GET_EVENT_TYPE(td->event_type),
		       len, bt_service.read_offset);

	/* Is this the start of the copy operation */
	if (bt_service.read_offset == 0) {
		SCSC_TAG_DEBUG(BT_RX,
			       "HCI Event [type=%s (0x%02x), length=%u]\n",
			       scsc_hci_evt_decode_event_code(td->data[0], td->data[2]), td->data[0], td->data[1]);

		if (td->data[1] + HCI_EVENT_HEADER_LENGTH != td->length) {
			SCSC_TAG_ERR(BT_H4, "Firmware sent invalid HCI event\n");
			atomic_inc(&bt_service.error_count);
			ret = -EFAULT;
		}

		/* Store the H4 header in the user buffer */
		ret = copy_to_user(buf, &h4_hci_event_header, sizeof(h4_hci_event_header));
		if (ret == 0) {
			/* All good - Update our consumed information */
			consumed = sizeof(h4_hci_event_header);
			bt_service.read_offset = sizeof(h4_hci_event_header);
		} else {
			SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
			ret = -EACCES;
		}
	}

	/* Can more data be put into the userspace buffer */
	if (ret == 0 && (len - consumed)) {
		/* Calculate the amount of data that can be transferred */
		len = min((td->length - (bt_service.read_offset - sizeof(h4_hci_event_header))), (len - consumed));

		/* Copy the data to the user buffer */
		ret = copy_to_user(&buf[consumed],
				   &td->data[bt_service.read_offset - sizeof(h4_hci_event_header)],
				   len);

		if (ret == 0) {
			/* All good - Update our consumed information */
			bt_service.read_offset += len;
			consumed += len;

			/* Have all data been copied to the userspace buffer */
			if (bt_service.read_offset == (sizeof(h4_hci_event_header) + td->length)) {
				/* All good - read operation is completed */
				bt_service.read_offset = 0;
				bt_service.read_operation = BT_READ_OP_NONE;
			}
		} else {
			SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
			ret = -EACCES;
		}
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_hci_evt_error_read(char __user *buf, size_t len)
{
	ssize_t ret;
	ssize_t consumed = 0;

	/* Send HCI_EVENT_VENDOR_SPECIFIC_SYSERR_SUBCODE and HCI_EVENT_HARDWARE_ERROR_EVENT */
	if (bt_service.system_error_info != NULL) {
		/* Calculate the amount of data that can be transferred */
		len = min(HCI_VSE_SYSTEM_ERROR_INFO_LEN - bt_service.read_offset, len);
		/* Copy the data to the user buffer */
		ret = copy_to_user(buf, &bt_service.system_error_info[bt_service.read_offset], len);
	} else {
		/* Calculate the amount of data that can be transferred */
		len = min(sizeof(h4_hci_event_hardware_error) - bt_service.read_offset, len);
		/* Copy the data to the user buffer */
		ret = copy_to_user(buf, &h4_hci_event_hardware_error[bt_service.read_offset], len);
	}

	if (ret == 0) {
		/* All good - Update our consumed information */
		bt_service.read_offset += len;
		consumed = len;

		/* Have all data been copied to the userspace buffer */
		if ((bt_service.system_error_info != NULL) && (bt_service.read_offset == HCI_VSE_SYSTEM_ERROR_INFO_LEN)) {
			/* All good - h4_hci_vse_system_error_info read operation is completed */
			bt_service.read_offset = 0;
			kfree(bt_service.system_error_info);
			bt_service.system_error_info = NULL;
		} else if (bt_service.read_offset == sizeof(h4_hci_event_hardware_error)){
			/* All good - h4_hci_event_hardware_error read operation is completed */
			bt_service.read_offset = 0;
			bt_service.read_operation = BT_READ_OP_NONE;
#ifdef CONFIG_SCSC_LOG_COLLECTION
			if (bt_service.recovery_level == 0)
				scsc_log_collector_schedule_collection(
					SCSC_LOG_HOST_BT,
					SCSC_LOG_HOST_BT_REASON_HCI_ERROR);
#endif
		}
	} else {
		SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
		ret = -EACCES;
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_rx_read_data(char __user *buf, size_t len, uint16_t data_length, uint8_t *data)
{
	ssize_t ret = 0;
	ssize_t consumed = 0;
	size_t copy_len = 0;

	/* Has the header been copied to userspace */
	if (bt_service.read_offset < h4_read_data_header_len) {
		copy_len = min(h4_read_data_header_len - bt_service.read_offset, len);

		/* Copy the header to the userspace buffer */
		ret = copy_to_user(buf, &h4_read_data_header[bt_service.read_offset], copy_len);

		if (ret == 0) {
			/* All good - Update our consumed information */
			consumed = copy_len;
			bt_service.read_offset += copy_len;
		} else {
			SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
			ret = -EACCES;
		}
	}

	/* Can more data be put into the userspace buffer */
	if (ret == 0 && bt_service.read_offset >= h4_read_data_header_len && (len - consumed)) {
		/* Calculate the amount of data that can be transferred */
		copy_len = min((data_length - (bt_service.read_offset - h4_read_data_header_len)),
			       (len - consumed));

		/* Copy the data to the user buffer */
		ret = copy_to_user(&buf[consumed],
				   &data[bt_service.read_offset - h4_read_data_header_len],
				   copy_len);

		if (ret == 0) {
			/* All good - Update our consumed information */
			bt_service.read_offset += copy_len;
			consumed += copy_len;

			/* Have all data been copied to the userspace buffer */
			if (bt_service.read_offset == (h4_read_data_header_len + data_length)) {
				/* All good - read operation is completed */
				bt_service.read_offset = 0;
				bt_service.read_operation = BT_READ_OP_NONE;
			}
		} else {
			SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
			ret = -EACCES;
		}
	}

	SCSC_TAG_DEBUG(BT_H4,
		       "read_offset=%zu, consumed=%zu, ret=%zd, len=%zu, copy_len=%zu\n",
		       bt_service.read_offset, consumed, ret, len, copy_len);

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_rx_read_iso_data(char __user *buf, size_t len)
{
	struct BSMHCP_TD_ISO_RX *td = &bt_service.bsmhcp_protocol->iso_rx_transfer_ring[bt_service.read_index];

	SCSC_TAG_DEBUG(BT_H4,
		"td (length=%u, hci_connection_handle=0x%03x, pb_flag=%u, ts_flag=%u), len=%zu, read_offset=%zu\n",
		td->header.length, td->header.hci_connection_handle, td->header.pb_flag,
		td->header.flag, len, bt_service.read_offset);

	return scsc_rx_read_data(buf, len, td->header.length, td->data);
}

static ssize_t scsc_rx_read_acl_data(char __user *buf, size_t len)
{
	ssize_t ret;
	struct BSMHCP_TD_ACL_RX *td = &bt_service.bsmhcp_protocol->acl_rx_transfer_ring[bt_service.read_index];

	SCSC_TAG_DEBUG(BT_H4,
		"td (length=%u, hci_connection_handle=0x%03x, pb_flag=%u, bc_flag=%u), len=%zu, read_offset=%zu\n",
		td->header.length, td->header.hci_connection_handle, td->header.pb_flag,
		td->header.flag, len, bt_service.read_offset);

	ret = scsc_rx_read_data(buf, len, td->header.length, td->data);

	if (bt_service.read_operation == BT_READ_OP_NONE && td->header.pb_flag == HCI_ACL_PACKET_BOUNDARY_START_FLUSH)
		/* The "false" argument is to tell the detection that this is RX */
		scsc_avdtp_detect_rxtx(td->header.hci_connection_handle, td->data, td->header.length, false);

	return ret;
}

static ssize_t scsc_read_credit(char __user *buf, size_t len)
{
	ssize_t                      consumed = 0;
	ssize_t                      ret = 0;

	SCSC_TAG_DEBUG(BT_H4, "len=%zu, read_offset=%zu\n", len, bt_service.read_offset);

	/* Calculate the amount of data that can be transferred */
	len = min(h4_hci_event_ncp_header_len - bt_service.read_offset, len);

	/* Copy the data to the user buffer */
	ret = copy_to_user(buf, &h4_hci_event_ncp_header[bt_service.read_offset], len);
	if (ret == 0) {
		/* All good - Update our consumed information */
		bt_service.read_offset += len;
		consumed = len;

		/* Have all data been copied to the userspace buffer */
		if (bt_service.read_offset == h4_hci_event_ncp_header_len) {
			/* All good - read operation is completed */
			bt_service.read_offset = 0;
			bt_service.read_operation = BT_READ_OP_NONE;
		}
	} else {
		SCSC_TAG_WARNING(BT_H4, "copy_to_user returned: %zu\n", ret);
		ret = -EACCES;
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_bt_shm_h4_read_continue(char __user *buf, size_t len)
{
	ssize_t ret = 0;

	/* Is a HCI event read operation ongoing */
	if (bt_service.read_operation == BT_READ_OP_HCI_EVT) {
		SCSC_TAG_DEBUG(BT_H4, "BT_READ_OP_HCI_EVT\n");

		/* Copy data into the userspace buffer */
		ret = scsc_hci_evt_read(buf, len);
		if (bt_service.read_operation == BT_READ_OP_NONE)
			/* All done - increase the read pointer and continue
			 * unless this was an out-of-order read for the queue
			 * sync helper
			 */
			if (bt_service.read_index == bt_service.mailbox_hci_evt_read)
				BSMHCP_INCREASE_INDEX(bt_service.mailbox_hci_evt_read, BSMHCP_TRANSFER_RING_EVT_SIZE);
		/* Is a ACL data read operation ongoing */
	} else if (bt_service.read_operation == BT_READ_OP_ACL_DATA) {
		SCSC_TAG_DEBUG(BT_H4, "BT_READ_OP_ACL_DATA\n");

		/* Copy data into the userspace buffer */
		ret = scsc_rx_read_acl_data(buf, len);
		if (bt_service.read_operation == BT_READ_OP_NONE)
			/* All done - increase the read pointer and continue */
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE);
		/* Is a ISO data read operation ongoing */
	} else if (bt_service.read_operation == BT_READ_OP_ISO_DATA) {
		SCSC_TAG_DEBUG(BT_H4, "BT_READ_OP_ISO_DATA\n");
		/* Copy data into the userspace buffer */
		ret = scsc_rx_read_iso_data(buf, len);
		if (bt_service.read_operation == BT_READ_OP_NONE)
			/* All done - increase the read pointer and continue */
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_iso_rx_read, BSMHCP_TRANSFER_RING_ISO_RX_SIZE);
		/* Is a ACL credit update operation ongoing */
	} else if (bt_service.read_operation == BT_READ_OP_CREDIT) {
		SCSC_TAG_DEBUG(BT_H4, "BT_READ_OP_CREDIT\n");

		/* Copy data into the userspace buffer */
		ret = scsc_read_credit(buf, len);
	} else if (bt_service.read_operation == BT_READ_OP_IQ_REPORT) {
		SCSC_TAG_DEBUG(BT_H4, "BT_READ_OP_IQ_REPORT\n");

		/* Copy data into the userspace buffer */
		ret = scsc_iq_report_evt_read(buf, len);
		if (bt_service.read_operation == BT_READ_OP_NONE)
			/* All done - increase the read pointer and continue */
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_iq_report_read, BSMHCP_TRANSFER_RING_IQ_REPORT_SIZE);
	} else if (bt_service.read_operation == BT_READ_OP_HCI_EVT_ERROR) {
		SCSC_TAG_ERR(BT_H4, "BT_READ_OP_HCI_EVT_ERROR\n");

		/* Copy data into the userspace buffer */
		ret = scsc_hci_evt_error_read(buf, len);
		if (bt_service.read_operation == BT_READ_OP_NONE)
			/* All done - set the stop condition */
			bt_service.read_operation = BT_READ_OP_STOP;
	} else if (bt_service.read_operation == BT_READ_OP_STOP) {
		ret = -EIO;
	}

	return ret;
}

static ssize_t scsc_bt_shm_h4_read_iq_report_evt(char __user *buf, size_t len)
{
	ssize_t ret = 0;
	ssize_t consumed = 0;

	if (bt_service.read_operation == BT_READ_OP_NONE &&
	    bt_service.mailbox_iq_report_read != bt_service.mailbox_iq_report_write) {
		struct BSMHCP_TD_IQ_REPORTING_EVT *td =
			&bt_service.bsmhcp_protocol->iq_reporting_transfer_ring[bt_service.mailbox_iq_report_read];
		u32 index = 0;
		u32 j = 0;
		u32 i;

		if (!bt_service.iq_reports_enabled) {
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_iq_report_read, BSMHCP_TRANSFER_RING_IQ_REPORT_SIZE);
		} else {
			memset(h4_iq_report_evt, 0, sizeof(h4_iq_report_evt));
			h4_iq_report_evt_len = 0;

			h4_iq_report_evt[index++] = HCI_EVENT_PKT;
			h4_iq_report_evt[index++] = HCI_EV_ULP;
			index++; /* Leaving room for total length of params  */
			h4_iq_report_evt[index++] = td->subevent_code;

			if (td->subevent_code == HCI_LE_CONNECTIONLESS_IQ_REPORT_EVENT_SUB_CODE) {
				/* LE Connectionless IQ Report Event*/
				h4_iq_report_evt[index++] = td->sync_handle & 0xFF;
				h4_iq_report_evt[index++] = (td->sync_handle >> 8) & 0xFF;
			} else if (td->subevent_code == HCI_LE_CONNECTION_IQ_REPORT_EVENT_SUB_CODE) {
				/* LE connection IQ Report Event */
				h4_iq_report_evt[index++] = td->connection_handle & 0xFF;
				h4_iq_report_evt[index++] = (td->connection_handle >> 8) & 0xFF;
				h4_iq_report_evt[index++] = td->rx_phy;
			}
			h4_iq_report_evt[index++] = td->channel_index;
			h4_iq_report_evt[index++] = td->rssi & 0xFF;
			h4_iq_report_evt[index++] = (td->rssi >> 8) & 0xFF;
			h4_iq_report_evt[index++] = td->rssi_antenna_id;
			h4_iq_report_evt[index++] = td->cte_type;
			h4_iq_report_evt[index++] = td->slot_durations;
			h4_iq_report_evt[index++] = td->packet_status;
			h4_iq_report_evt[index++] = td->event_count & 0xFF;
			h4_iq_report_evt[index++] = (td->event_count >> 8) & 0xFF;
			h4_iq_report_evt[index++] = td->sample_count;

			/* Total length of hci event */
			h4_iq_report_evt_len = index + (2 * td->sample_count);

			/* Total length of hci event parameters */
			h4_iq_report_evt[2] = h4_iq_report_evt_len - 3;

			for (i = 0; i < td->sample_count; i++) {
				h4_iq_report_evt[index + i] = td->data[j++];
				h4_iq_report_evt[(index + td->sample_count) + i] = td->data[j++];
			}

			bt_service.read_operation = BT_READ_OP_IQ_REPORT;
			bt_service.read_index = bt_service.mailbox_iq_report_read;

			ret = scsc_iq_report_evt_read(&buf[consumed], len - consumed);
			if (ret > 0) {
				/* All good - Update our consumed information */
				consumed += ret;
				ret = 0;

				/**
				 * Update the index if all the data could be copied to the userspace
				 * buffer otherwise stop processing the HCI events
				 */
				if (bt_service.read_operation == BT_READ_OP_NONE)
					BSMHCP_INCREASE_INDEX(bt_service.mailbox_iq_report_read,
							      BSMHCP_TRANSFER_RING_IQ_REPORT_SIZE);
			}
		}
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_bt_shm_h4_read_hci_evt(char __user *buf, size_t len)
{
	ssize_t ret = 0;
	ssize_t consumed = 0;

	while (bt_service.read_operation == BT_READ_OP_NONE &&
	       ret == 0 &&
	       !bt_service.hci_event_paused &&
	       bt_service.mailbox_hci_evt_read != bt_service.mailbox_hci_evt_write) {
		struct BSMHCP_TD_HCI_EVT *td =
				&bt_service.bsmhcp_protocol->hci_evt_transfer_ring[bt_service.mailbox_hci_evt_read];

		u8 event_type = BSMHCP_GET_EVENT_TYPE(td->event_type);
		u16 big_handle = BSMHCP_GET_BIG_HANDLE(td->event_type);

		/* This event has already been processed - skip it */
		if (bt_service.processed[bt_service.mailbox_hci_evt_read]) {
			bt_service.processed[bt_service.mailbox_hci_evt_read] = false;
			BSMHCP_INCREASE_INDEX(bt_service.mailbox_hci_evt_read, BSMHCP_TRANSFER_RING_EVT_SIZE);
			continue;
		}

		/* A connection event has been detected by the firmware */
		if (event_type == BSMHCP_EVENT_TYPE_CONNECTED ||
		    event_type == BSMHCP_EVENT_TYPE_BIG_CONNECTED) {
			/* Sanity check of the HCI connection handle */
			bool update_connection_handle_list = true;

			if (td->hci_connection_handle >= SCSC_BT_CONNECTION_INFO_MAX) {
				SCSC_TAG_ERR(BT_H4,
					     "connection handle is beyond max (hci_connection_handle=0x%03x)\n",
					     td->hci_connection_handle);

				atomic_inc(&bt_service.error_count);
				break;
			}

			if (event_type == BSMHCP_EVENT_TYPE_BIG_CONNECTED) {
				/* Ensure that the connections only are register to a big handle once */
				if (bt_service.connection_handle_list[td->hci_connection_handle].state == CONNECTION_NONE) {
					bt_service.big_handle_list[big_handle].big_handle = big_handle;
					bt_service.big_handle_list[big_handle].num_of_connections++;
				} else
					update_connection_handle_list = false;
			}

			if (update_connection_handle_list) {
				SCSC_TAG_DEBUG(BT_H4,
					     "connected (hci_connection_handle=0x%03x, state=%u, big_handle=0x%04x)\n",
					     td->hci_connection_handle,
					     bt_service.connection_handle_list[td->hci_connection_handle].state,
					     big_handle);

				/* Update the connection table to mark it as active */
				bt_service.connection_handle_list[td->hci_connection_handle].state = CONNECTION_ACTIVE;
				bt_service.connection_handle_list[td->hci_connection_handle].remaining_length = 0;
				bt_service.connection_handle_list[td->hci_connection_handle].big_handle = big_handle;
				/* ACL and ISO data processing can now continue */
				bt_service.data_paused = false;
			}

			/* A disconnection event has been detected by the firmware */
		} else if (event_type == BSMHCP_EVENT_TYPE_DISCONNECTED) {
			SCSC_TAG_DEBUG(BT_H4,
				       "disconnected (hci_connection_handle=0x%03x, state=%u)\n",
				       td->hci_connection_handle,
				       bt_service.connection_handle_list[td->hci_connection_handle].state);

			/* If this ACL connection had an avdtp stream, mark it gone and interrupt the bg */
			if (scsc_avdtp_detect_reset_connection_handle(td->hci_connection_handle))
				wmb();

			/* If the connection is marked as active the ACL disconnect packet hasn't yet arrived */
			if (bt_service.connection_handle_list[td->hci_connection_handle].state == CONNECTION_ACTIVE) {
				/* Pause the HCI event procssing until the ACL or ISO disconnect packet arrives */
				bt_service.hci_event_paused = true;
				break;
			}

			/* Firmware does not have more ACL data - Mark the connection as inactive */
			bt_service.connection_handle_list[td->hci_connection_handle].state = CONNECTION_NONE;

			/* Clear the ACL and ISO data processing to allow for the ACL and ISO
			 * disconnect event to be transferred to userspace
			 */
			if (bt_service.data_paused &&
			    bt_service.data_paused_conn_hdl == td->hci_connection_handle) {
				/* ACL and ISO data processing can now continue */
				bt_service.data_paused = false;

				/* Clear all ACL data having data_paused_conn_hdl from acl_rx_transfer_ring */
				scsc_bt_clear_paused_acl_rx(bt_service.data_paused_conn_hdl);

				/* Initialize the data_paused_conn_hdl for next data_paused */
				bt_service.data_paused_conn_hdl = 0;
			}

		/* A group disconnection event has been detected by the firmware */
		} else if (event_type == BSMHCP_EVENT_TYPE_BIG_DISCONNECTED) {
			SCSC_TAG_DEBUG(BT_H4, "Isochronous Broadcaster destroyed (big_handle=0x%04x)\n", big_handle);

			if (bt_service.big_handle_list[big_handle].num_of_connections > 0) {
				/**
				 * Pause the HCI event procssing until all connections belonging to the
				 * Isochronous Broadcaster is disconnected.
				 */
				bt_service.hci_event_paused = true;
				break;
			}

			/**
			 * Firmware does not have more data on any connection belonging to the Isochronous Broadcaster.
			 * Mark the all connections as inactive.
			 */
			scsc_bt_big_reset_connection_handles(big_handle);
			bt_service.big_handle_list[big_handle].big_handle = BSMHCP_INVALID_BIG_HANDLE;

		} else if (event_type == BSMHCP_EVENT_TYPE_IQ_REPORT_ENABLED) {
			bt_service.iq_reports_enabled = true;
		} else if (event_type == BSMHCP_EVENT_TYPE_IQ_REPORT_DISABLED) {
			bt_service.iq_reports_enabled = false;
		}

		/* Start a HCI event copy to userspace */
		bt_service.read_operation = BT_READ_OP_HCI_EVT;
		bt_service.read_index = bt_service.mailbox_hci_evt_read;
		ret = scsc_hci_evt_read(&buf[consumed], len - consumed);
		if (ret > 0) {
			/* All good - Update our consumed information */
			consumed += ret;
			ret = 0;

			/* Update the index if all the data could be copied to the userspace buffer
			 * otherwise stop processing the HCI events
			 */
			if (bt_service.read_operation == BT_READ_OP_NONE)
				BSMHCP_INCREASE_INDEX(bt_service.mailbox_hci_evt_read, BSMHCP_TRANSFER_RING_EVT_SIZE);
			else
				break;
		}
	}

	return ret == 0 ? consumed : ret;
}

static bool scsc_rx_read_iso_data_start_copy(char __user *buf, size_t len, ssize_t *ret, ssize_t *consumed)
{
	uint16_t data_load_len;
	struct BSMHCP_TD_ISO_RX *td =
		&bt_service.bsmhcp_protocol->iso_rx_transfer_ring[bt_service.mailbox_iso_rx_read];

	u8  ts_flag = td->header.flag & BSMHCP_ISO_TS_FLAG_MASK;

	bt_service.read_operation = BT_READ_OP_ISO_DATA;
	bt_service.read_index = bt_service.mailbox_iso_rx_read;

	/* Fully generate the H4 header + ISO data header regardless of the available amount of user memory */
	h4_read_data_header_len = H4DMUX_HEADER_ISO;
	h4_read_data_header[H4_HEADER_INDEX] = HCI_ISODATA_PKT;

	HCI_ISO_DATA_SET_CON_HDL(h4_read_data_header, td->header.hci_connection_handle, td->header.pb_flag, ts_flag);

	/* The fields Time_Stamp, Packet_Sequence_Number, Packet_Status_Flag and ISO_SDU_Length
	 * shall only be included in the HCI ISO Data packet if the PB_Flag equals 0b00 or 0b10
	 */
	if (td->header.pb_flag == BSMHCP_ISO_PB_FLAG_FIRST || td->header.pb_flag == BSMHCP_ISO_PB_FLAG_COMPLETE) {
		/* The Time_Stamp field shall only be included in the HCI ISO Data packet if the TS_Flag is set */
		if (ts_flag == BSMHCP_ISO_TS_FLAG_TS_INCL) {
			HCI_ISO_DATA_SET_TIMESTAMP(h4_read_data_header, h4_read_data_header_len, td->time_stamp);
			h4_read_data_header_len += ISO_TIMESTAMP_SIZE;
		}

		HCI_ISO_DATA_SET_PKT_SEQ_NUM(h4_read_data_header, h4_read_data_header_len, td->packet_sequence_number);
		h4_read_data_header_len += ISO_PKT_SEQ_NUM_SIZE;

		HCI_ISO_DATA_SET_SDU_LENGTH_AND_STATUS_FLAG(h4_read_data_header,
				    h4_read_data_header_len, td->sdu_length, td->packet_status_flag);
		h4_read_data_header_len += ISO_PKT_SDU_LENGTH_SIZE;
	}

	/* Add the length of the additional headers in the ISO Data load field to get the 'real' payload_len */
	data_load_len = td->header.length + (h4_read_data_header_len - H4DMUX_HEADER_ISO);

	if (data_load_len > ISO_DATA_LOAD_LENGTH_MAX_LEN) {
		SCSC_TAG_ERR(BT_H4, "Error - Received ISO Packet to large (data length=%u, max size=%u)\n",
			data_load_len, ISO_DATA_LOAD_LENGTH_MAX_LEN);
		atomic_inc(&bt_service.error_count);
		return -EIO;
	}

	HCI_ISO_DATA_SET_LENGTH(h4_read_data_header, data_load_len);

	*ret = scsc_rx_read_iso_data(&buf[*consumed], len - *consumed);
	if (*ret <= 0)
		return *ret < 0; /* Break the loop for errors */

	/* Update our consumed information */
	*consumed += *ret;
	*ret = 0;

	/* Stop processing if all the data could not be copied to userspace */
	if (bt_service.read_operation != BT_READ_OP_NONE)
		return true;

	BSMHCP_INCREASE_INDEX(bt_service.mailbox_iso_rx_read, BSMHCP_TRANSFER_RING_ISO_RX_SIZE);

	return false;
}

/**
 * Start the acl data to userspace copy
 *
 * Acl processing should be stopped if either unable to read a complete packet
 * or a complete packet is read and BlueZ is enabled
 *
 * @param[out]    ret      result of read operations written to here
 * @param[in,out] consumed read bytes added to this
 *
 * @return true if ACL data processing should stop
 */
static bool scsc_rx_read_acl_data_start_copy(char __user *buf, size_t len, ssize_t *ret, ssize_t *consumed)
{
	struct BSMHCP_TD_ACL_RX *td =
		&bt_service.bsmhcp_protocol->acl_rx_transfer_ring[bt_service.mailbox_acl_rx_read];

	bt_service.read_operation = BT_READ_OP_ACL_DATA;
	bt_service.read_index = bt_service.mailbox_acl_rx_read;

	/* Fully generate the H4 header + ACL data header regardless of the available amount of user memory */
	h4_read_data_header_len = H4DMUX_HEADER_ACL;

	h4_read_data_header[H4_HEADER_INDEX] = HCI_ACLDATA_PKT;

	HCI_ACL_DATA_SET_CON_HDL(h4_read_data_header,
				 td->header.hci_connection_handle, td->header.pb_flag, td->header.flag);

	HCI_ACL_DATA_SET_LENGTH(h4_read_data_header, td->header.length);

	*ret = scsc_rx_read_acl_data(&buf[*consumed], len - *consumed);
	if (*ret <= 0)
		return *ret < 0; /* Break the loop for errors */

	/* Update our consumed information */
	*consumed += *ret;
	*ret = 0;

	/* Stop processing if all the data could not be copied to userspace */
	if (bt_service.read_operation != BT_READ_OP_NONE)
		return true;

	BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE);

	return false;
}

static bool scsc_rx_read_connection_active(uint16_t hci_connection_handle, uint8_t disconnected)
{
	/* Sanity check of the HCI connection handle */
	if (hci_connection_handle >= SCSC_BT_CONNECTION_INFO_MAX) {
		SCSC_TAG_ERR(BT_H4,
			     "connection handle is beyond max (hci_connection_handle=0x%03x)\n",
			     hci_connection_handle);
		atomic_inc(&bt_service.error_count);
		return false;
	}

	/* Only process ACL or ISO data if the connection is marked active aka a
	 * HCI connection complete event has arrived
	 */
	if (bt_service.connection_handle_list[hci_connection_handle].state == CONNECTION_ACTIVE) {
		/* Is this the final packet for the indicated ACL or ISO connection */
		if (disconnected) {
			u16 big_handle = bt_service.connection_handle_list[hci_connection_handle].big_handle;

			SCSC_TAG_DEBUG(BT_H4,
				       "CONN disconnected (hci_connection_handle=0x%03x, state=%u, big_handle=0x%04x)\n",
				       hci_connection_handle,
				       bt_service.connection_handle_list[hci_connection_handle].state,
				       big_handle);

			/* Update the connection table to mark it as disconnected */
			bt_service.connection_handle_list[hci_connection_handle].state = CONNECTION_DISCONNECTED;

			if (big_handle != BSMHCP_INVALID_BIG_HANDLE &&
			    bt_service.big_handle_list[big_handle].num_of_connections > 0) {

				bt_service.big_handle_list[big_handle].num_of_connections--;

				/* Keep blocking HCI event processing until all connections belonging to the
				 * Isochronous Broadcaster are disconnected.
				 */
				if (bt_service.big_handle_list[big_handle].num_of_connections > 0)
					return true;
			}

			/* Clear the HCI event processing to allow for the HCI disconnect event
			 * to be transferred to userspace
			 */
			bt_service.hci_event_paused = false;
		}

		return true;
	}

	/* If the connection state is inactive the HCI connection complete information hasn't yet arrived.
	 * Stop processing ACL or ISO data
	 */
	if (bt_service.connection_handle_list[hci_connection_handle].state == CONNECTION_NONE) {
		SCSC_TAG_DEBUG(BT_H4,
			       "DATA empty (hci_connection_handle=0x%03x, state=%u)\n",
			       hci_connection_handle,
			       bt_service.connection_handle_list[hci_connection_handle].state);

		bt_service.data_paused = true;
		bt_service.data_paused_conn_hdl = hci_connection_handle;

	/* If the connection state is disconnection the firmware sent ACL or ISO after the ACL or ISO
	 * disconnect packet which is an FW error
	 */
	} else {
		SCSC_TAG_ERR(BT_H4, "DATA received after disconnected indication\n");
		atomic_inc(&bt_service.error_count);
	}

	return false;
}

static ssize_t scsc_bt_shm_h4_read_iso_data(char __user *buf, size_t len)
{
	ssize_t ret = 0;
	ssize_t consumed = 0;

	while (bt_service.read_operation == BT_READ_OP_NONE &&
	       !bt_service.data_paused &&
	       bt_service.mailbox_iso_rx_read != bt_service.mailbox_iso_rx_write) {
		struct BSMHCP_TD_ISO_RX *td =
			&bt_service.bsmhcp_protocol->iso_rx_transfer_ring[bt_service.mailbox_iso_rx_read];

		/* Only process ISO data if the connection is marked active aka a
		 * HCI connection complete event has arrived
		 */
		if (scsc_rx_read_connection_active(td->header.hci_connection_handle, td->header.disconnected)) {
			/* Is this the final packet for the indicated ISO connection */
			if (td->header.disconnected) {
				/* Update the read pointer */
				BSMHCP_INCREASE_INDEX(bt_service.mailbox_iso_rx_read, BSMHCP_TRANSFER_RING_ISO_RX_SIZE);
			} else {
				if (scsc_rx_read_iso_data_start_copy(buf, len, &ret, &consumed))
					break;
			}
		} else {
			break;
		}
	}

	return ret == 0 ? consumed : ret;
}

static ssize_t scsc_bt_shm_h4_read_acl_data(char __user *buf, size_t len)
{
	ssize_t ret = 0;
	ssize_t consumed = 0;

	while (bt_service.read_operation == BT_READ_OP_NONE &&
	       !bt_service.data_paused &&
	       bt_service.mailbox_acl_rx_read != bt_service.mailbox_acl_rx_write) {
		struct BSMHCP_TD_ACL_RX *td =
			&bt_service.bsmhcp_protocol->acl_rx_transfer_ring[bt_service.mailbox_acl_rx_read];

		/* Bypass packet inspection and connection handling for data dump */
		if ((td->header.hci_connection_handle & SCSC_BT_ACL_RAW_MASK) == SCSC_BT_ACL_RAW) {
			if (scsc_rx_read_acl_data_start_copy(buf, len, &ret, &consumed))
				break;
		}

		/* Only process ACL data if the connection is marked active aka a
		 * HCI connection complete event has arrived
		 */
		if (scsc_rx_read_connection_active(td->header.hci_connection_handle, td->header.disconnected)) {
			/* Is this the final packet for the indicated ACL connection */
			if (td->header.disconnected) {
				/* Update the read pointer */
				BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_rx_read, BSMHCP_TRANSFER_RING_ACL_SIZE);
			} else {
				if (scsc_rx_read_acl_data_start_copy(buf, len, &ret, &consumed))
					break;
			}
		} else {
			break;
		}
	}

	return ret == 0 ? consumed : ret;
}

static void scsc_bt_shm_h4_insert_credit_entries(uint16_t conn_hdl, u32 start_index, u16 tr_size, u32 *entries)
{
	uint16_t sanitized_conn_handle = conn_hdl & SCSC_BT_ACL_HANDLE_MASK;
	u32 i;

	if (bt_service.connection_handle_list[sanitized_conn_handle].state == CONNECTION_ACTIVE) {
		for (i = 0; i < tr_size; i++) {
			if (h4_hci_credit_entries[start_index + i].hci_connection_handle == 0) {
				h4_hci_credit_entries[start_index + i].hci_connection_handle = conn_hdl;
				h4_hci_credit_entries[start_index + i].credits = 1;
				*entries += 1;
				break;
			} else if ((h4_hci_credit_entries[start_index + i].hci_connection_handle &
				SCSC_BT_ACL_HANDLE_MASK) == sanitized_conn_handle) {
				h4_hci_credit_entries[start_index + i].hci_connection_handle = conn_hdl;
				h4_hci_credit_entries[start_index + i].credits++;
				break;
			}
		}
	} else {
		SCSC_TAG_WARNING(BT_H4,
			"No active connection ((hci_connection_handle=0x%03x)\n", sanitized_conn_handle);
	}
}

static void scsc_bt_shm_h4_read_acl_credit(u32 *entries)
{
	u32 start_index = *entries;

	while (bt_service.mailbox_acl_free_read != bt_service.mailbox_acl_free_write) {
		struct BSMHCP_TD_ACL_TX_FREE *td =
			&bt_service.bsmhcp_protocol->acl_tx_free_transfer_ring[bt_service.mailbox_acl_free_read];

		scsc_bt_shm_h4_insert_credit_entries(td->hci_connection_handle,
				start_index, BSMHCP_TRANSFER_RING_ACL_SIZE, entries);

		BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_free_read, BSMHCP_TRANSFER_RING_ACL_SIZE);
	}
}

static void scsc_bt_shm_h4_read_iso_credit(u32 *entries)
{
	u32 start_index = *entries;

	while (bt_service.mailbox_iso_free_read != bt_service.mailbox_iso_free_write) {
		struct BSMHCP_TD_ISO_TX_FREE *td =
			&bt_service.bsmhcp_protocol->iso_tx_free_transfer_ring[bt_service.mailbox_iso_free_read];

		scsc_bt_shm_h4_insert_credit_entries(td->hci_connection_handle,
				start_index, BSMHCP_TRANSFER_RING_ISO_TX_SIZE, entries);

		BSMHCP_INCREASE_INDEX(bt_service.mailbox_iso_free_read, BSMHCP_TRANSFER_RING_ISO_TX_SIZE);
	}
}

static ssize_t scsc_bt_shm_h4_read_credit(char __user *buf, size_t len)
{
	ssize_t ret = 0;

	if (bt_service.read_operation == BT_READ_OP_NONE) {
		u32 entries = 0;

		memset(h4_hci_event_ncp_header, 0, sizeof(h4_hci_event_ncp_header));

		/* Find how many ACL data packet have been completed */
		scsc_bt_shm_h4_read_acl_credit(&entries);

		/* Find how many ISO data packet have been completed */
		scsc_bt_shm_h4_read_iso_credit(&entries);

		if (entries > 0) {
			/* Fill the number of completed packets data into the temp buffer */
			h4_hci_event_ncp_header[0] = HCI_EVENT_PKT;
			h4_hci_event_ncp_header[1] = HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS_EVENT; /* Event Code */
			h4_hci_event_ncp_header[2] = 1 + (sizeof(struct hci_credit_entry) * entries); /* Param length */
			h4_hci_event_ncp_header[3] = entries; /* Number_of_Handles */
			h4_hci_event_ncp_header_len = HCI_EVENT_NCP_HEADER_LEN +
						      (sizeof(struct hci_credit_entry) * entries);

			/* Start copy the Number of Completed Packet Event to userspace */
			bt_service.read_operation = BT_READ_OP_CREDIT;
			ret = scsc_read_credit(buf, len);
		}
	}

	return ret;
}

ssize_t scsc_bt_shm_h4_queue_sync_helper(char __user *buf, size_t len)
{
	uint16_t conn_hdl_acl;
	uint16_t conn_hdl_iso;

	ssize_t ret = 0;
	bool found = false;
	u32 mailbox_hci_evt_read = bt_service.mailbox_hci_evt_read;

	if (!bt_service.hci_event_paused)
		return ret;

	conn_hdl_acl = bt_service.bsmhcp_protocol->acl_rx_transfer_ring[
				bt_service.mailbox_acl_rx_read].header.hci_connection_handle;

	conn_hdl_iso = bt_service.bsmhcp_protocol->iso_rx_transfer_ring[
				bt_service.mailbox_iso_rx_read].header.hci_connection_handle;

	/* If both the HCI event transfer ring and data transfer rings, ACL or ISO, has been
	 * paused the entire HCI event transfer ring is scanned for the presence
	 * of the connected indication. Once present this is transferred to the host
	 * stack and marked as processed. This will unlock the hci event processing
	 */
	while (bt_service.data_paused) {
		while (mailbox_hci_evt_read != bt_service.mailbox_hci_evt_write) {
			struct BSMHCP_TD_HCI_EVT *td =
				&bt_service.bsmhcp_protocol->hci_evt_transfer_ring[mailbox_hci_evt_read];

			uint16_t event_type = BSMHCP_GET_EVENT_TYPE(td->event_type);

			if ((event_type == BSMHCP_EVENT_TYPE_CONNECTED ||
			     event_type == BSMHCP_EVENT_TYPE_BIG_CONNECTED) &&
			    (conn_hdl_acl == td->hci_connection_handle ||
			     conn_hdl_iso == td->hci_connection_handle)) {
				/* Update the connection table to mark it as active */
				bt_service.connection_handle_list[td->hci_connection_handle].state = CONNECTION_ACTIVE;
				bt_service.connection_handle_list[td->hci_connection_handle].remaining_length = 0;
				bt_service.connection_handle_list[td->hci_connection_handle].big_handle =
					BSMHCP_GET_BIG_HANDLE(td->event_type);

				/* ACL and ISO data processing can now continue */
				bt_service.data_paused = false;

				/* Mark the event as processed */
				bt_service.processed[mailbox_hci_evt_read] = true;

				/* Indicate the event have been found */
				found = true;

				/* Start a HCI event copy to userspace */
				bt_service.read_operation = BT_READ_OP_HCI_EVT;
				bt_service.read_index = mailbox_hci_evt_read;
				ret = scsc_hci_evt_read(buf, len);
				break;
			}

			if (event_type == BSMHCP_EVENT_TYPE_DISCONNECTED &&
			    bt_service.data_paused_conn_hdl == td->hci_connection_handle) {
				SCSC_TAG_DEBUG(BT_H4,
				       "disconnected (hci_connection_handle=0x%03x, state=%u)\n",
				       td->hci_connection_handle,
				       bt_service.connection_handle_list[td->hci_connection_handle].state);

				/* If this ACL connection had an avdtp stream, mark it gone and interrupt the bg */
				if (scsc_avdtp_detect_reset_connection_handle(td->hci_connection_handle))
					wmb();

				/* Firmware does not have more ACL data - Mark the connection as inactive */
				bt_service.connection_handle_list[td->hci_connection_handle].state = CONNECTION_NONE;

				/* ACL and ISO data processing can now continue */
				bt_service.data_paused = false;

				/* Clear all ACL data having data_paused_conn_hdl from acl_rx_transfer_ring */
				scsc_bt_clear_paused_acl_rx(bt_service.data_paused_conn_hdl);

				/* Initialize the data_paused_conn_hdl for next data_paused */
				bt_service.data_paused_conn_hdl = 0;

				/* Mark the event as processed */
				bt_service.processed[mailbox_hci_evt_read] = true;

				/* Indicate the event have been found */
				found = true;
				break;
			}

			BSMHCP_INCREASE_INDEX(mailbox_hci_evt_read, BSMHCP_TRANSFER_RING_EVT_SIZE);
		}

		if (!found) {
			ret = wait_event_interruptible_timeout(
				bt_service.read_wait,
				((mailbox_hci_evt_read != bt_service.bsmhcp_protocol->header.mailbox_hci_evt_write ||
				atomic_read(&bt_service.error_count) != 0 ||
				bt_service.bsmhcp_protocol->header.panic_deathbed_confession)), HZ);

			if (ret == 0) {
				SCSC_TAG_ERR(BT_H4,
					     "firmware didn't send the connected event within the given timeframe\n");
				atomic_inc(&bt_service.error_count);
				break;
			} else if (ret != 1) {
				SCSC_TAG_INFO(BT_H4, "user interrupt\n");
				ret = -EAGAIN;
				break;
			}
		}
	}

	return ret;
}

static void scsc_bt_free_acl_credits(void)
{
	while (bt_service.mailbox_acl_free_read_scan != bt_service.mailbox_acl_free_write) {
		struct BSMHCP_TD_ACL_TX_FREE *td =
			&bt_service.bsmhcp_protocol->acl_tx_free_transfer_ring[bt_service.mailbox_acl_free_read_scan];

		/* Free the buffer in the allocation table */
		if (td->buffer_index < BSMHCP_DATA_BUFFER_TX_ACL_SIZE) {
			bt_service.allocated[td->buffer_index] = 0;
			bt_service.freed_count++;

			SCSC_TAG_DEBUG(BT_TX,
				       "ACL[CREDIT] (index=%u, buffer=%u, credits=%u)\n",
				       bt_service.mailbox_acl_free_read_scan, td->buffer_index,
				       GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ACL_SIZE,
								bt_service.allocated_count,
								bt_service.freed_count));
		}

		BSMHCP_INCREASE_INDEX(bt_service.mailbox_acl_free_read_scan, BSMHCP_TRANSFER_RING_ACL_SIZE);
	}
}

static void scsc_bt_free_iso_credits(void)
{
	while (bt_service.mailbox_iso_free_read_scan != bt_service.mailbox_iso_free_write) {
		struct BSMHCP_TD_ISO_TX_FREE *td =
		       &bt_service.bsmhcp_protocol->iso_tx_free_transfer_ring[bt_service.mailbox_iso_free_read_scan];

		/* Free the buffer in the ISO allocation table */
		if (td->buffer_index < BSMHCP_DATA_BUFFER_TX_ISO_SIZE) {
			bt_service.iso_allocated[td->buffer_index] = 0;
			bt_service.iso_freed_count++;

			SCSC_TAG_DEBUG(BT_TX,
				       "ISO[CREDIT] (index=%u, buffer=%u, credits=%u)\n",
				       bt_service.mailbox_iso_free_read_scan, td->buffer_index,
				       GET_AVAILABLE_TX_CREDITS(BSMHCP_DATA_BUFFER_TX_ISO_SIZE,
								bt_service.iso_allocated_count,
								bt_service.iso_freed_count));
		}

		BSMHCP_INCREASE_INDEX(bt_service.mailbox_iso_free_read_scan, BSMHCP_TRANSFER_RING_ISO_TX_SIZE);
	}
}

static void scsc_bt_free_credits(void)
{
	scsc_bt_free_acl_credits();
	scsc_bt_free_iso_credits();
}

static void scsc_update_cached_write(void)
{
	/* Update the write cached indexes for all transfer rings */
	bt_service.mailbox_hci_evt_write   = bt_service.bsmhcp_protocol->header.mailbox_hci_evt_write;
	bt_service.mailbox_acl_rx_write    = bt_service.bsmhcp_protocol->header.mailbox_acl_rx_write;
	bt_service.mailbox_acl_free_write  = bt_service.bsmhcp_protocol->header.mailbox_acl_free_write;
	bt_service.mailbox_iq_report_write = bt_service.bsmhcp_protocol->header.mailbox_iq_report_write;
	bt_service.mailbox_iso_rx_write    = bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_write;
	bt_service.mailbox_iso_free_write  = bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_write;
}

static void scsc_update_read_indexes(void)
{
	/* Update the read indexes for all transfer rings */
	bt_service.bsmhcp_protocol->header.mailbox_hci_evt_read = bt_service.mailbox_hci_evt_read;
	bt_service.bsmhcp_protocol->header.mailbox_acl_rx_read = bt_service.mailbox_acl_rx_read;
	bt_service.bsmhcp_protocol->header.mailbox_acl_free_read = bt_service.mailbox_acl_free_read;
	bt_service.bsmhcp_protocol->header.mailbox_iq_report_read = bt_service.mailbox_iq_report_read;
	bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_read = bt_service.mailbox_iso_rx_read;
	bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_read = bt_service.mailbox_iso_free_read;
}

ssize_t scsc_bt_shm_h4_read(struct file *file, char __user *buf, size_t len, loff_t *offset)
{
	ssize_t consumed = 0;
	ssize_t ret = 0;
	ssize_t res;
	bool    gen_bg_int = false;
	bool    gen_fg_int = false;

	if (len == 0)
		return 0;

	/* Special handling in case read is called after service has closed */
	if (!bt_service.service_started)
		return -EIO;

	/* Only 1 reader is allowed */
	if (atomic_inc_return(&bt_service.h4_readers) != 1) {
		atomic_dec(&bt_service.h4_readers);
		return -EIO;
	}

	/* Update the cached variables with the non-cached variables */
	scsc_update_cached_write();

	/* Only generate the HCI hardware error event if any pending operation has been completed
	 * and the event hasn't already neen sent. This check assume the main while loop will exit
	 * on a completed operation in the next section
	 */
	if (atomic_read(&bt_service.error_count) != 0 && bt_service.read_operation == BT_READ_OP_NONE)
		bt_service.read_operation = BT_READ_OP_HCI_EVT_ERROR;

	/* put the remaining data from the transfer ring into the available userspace buffer */
	if (bt_service.read_operation != BT_READ_OP_NONE) {
		ret = scsc_bt_shm_h4_read_continue(buf, len);
		/* Update the consumed variable in case a operation was ongoing */
		if (ret > 0) {
			consumed = ret;
			ret = 0;
		}
	}

	/* Main loop - Can only be entered when no operation is present on entering this function
	 * or no hardware error has been detected. It loops until data has been placed in the
	 * userspace buffer or an error has been detected
	 */
	while (0 == atomic_read(&bt_service.error_count) && 0 == consumed) {
		/* If both the HCI event processing and data (ACL or ISO) processing has been disabled
		 * this function helps exit this condition by scanning the HCI event queue for the
		 * connection established event and return it to userspace
		 */
		ret = scsc_bt_shm_h4_queue_sync_helper(buf, len);
		if (ret != 0) {
			consumed = ret;
			break;
		}

		/* Does any of the read/write pairs differs */
		if ((bt_service.mailbox_hci_evt_read == bt_service.mailbox_hci_evt_write ||
		     bt_service.hci_event_paused) &&
		    (bt_service.mailbox_acl_rx_read == bt_service.mailbox_acl_rx_write ||
		     bt_service.data_paused) &&
		    bt_service.mailbox_acl_free_read == bt_service.mailbox_acl_free_write &&
		    bt_service.mailbox_iq_report_read == bt_service.mailbox_iq_report_write &&
		    bt_service.mailbox_iso_free_read == bt_service.mailbox_iso_free_write &&
		    (bt_service.mailbox_iso_rx_read == bt_service.mailbox_iso_rx_write || bt_service.data_paused) &&
		    atomic_read(&bt_service.error_count) == 0 &&
		    bt_service.bsmhcp_protocol->header.panic_deathbed_confession == 0) {
			/* Don't wait if in NONBLOCK mode */
			if (file->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			/* All read/write pairs are identical - wait for the firmware. The conditional
			 * check is used to verify that a read/write pair has actually changed
			 */
			ret = wait_event_interruptible(
				bt_service.read_wait,
				((bt_service.mailbox_hci_evt_read !=
				  bt_service.bsmhcp_protocol->header.mailbox_hci_evt_write &&
				  !bt_service.hci_event_paused) ||
				 (bt_service.mailbox_acl_rx_read !=
				  bt_service.bsmhcp_protocol->header.mailbox_acl_rx_write &&
				  !bt_service.data_paused) ||
				 (bt_service.mailbox_acl_free_read !=
				  bt_service.bsmhcp_protocol->header.mailbox_acl_free_write) ||
				 (bt_service.mailbox_iq_report_read !=
				  bt_service.bsmhcp_protocol->header.mailbox_iq_report_write) ||
				 (bt_service.mailbox_iso_free_read !=
				  bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_write) ||
				 (bt_service.mailbox_iso_rx_read !=
				  bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_write &&
				  !bt_service.data_paused) ||
				 atomic_read(&bt_service.error_count) != 0 ||
				 bt_service.bsmhcp_protocol->header.panic_deathbed_confession));

			/* Has an error been detected elsewhere in the driver then just return from this function */
			if (atomic_read(&bt_service.error_count) != 0)
				break;

			/* Any failures is handled by the userspace application */
			if (ret)
				break;

			/* Refresh our write indexes before starting to process the protocol */
			scsc_update_cached_write();
		}

		SCSC_TAG_DEBUG(BT_H4,
			"hci_evt_read=%u, hci_evt_write=%u, acl_rx_read=%u, acl_rx_write=%u, iso_rx_read=%u, iso_rx_write=%u\n",
			bt_service.mailbox_hci_evt_read,
			bt_service.mailbox_hci_evt_write,
			bt_service.mailbox_acl_rx_read,
			bt_service.mailbox_acl_rx_write,
			bt_service.mailbox_iso_rx_read,
			bt_service.mailbox_iso_rx_write);

		SCSC_TAG_DEBUG(BT_H4,
			"acl_free_read=%u, acl_free_write=%u, iso_free_read=%u, iso_free_write=%u, iq_report_read=%u iq_report_write=%u\n",
			bt_service.mailbox_acl_free_read,
			bt_service.mailbox_acl_free_write,
			bt_service.mailbox_iso_free_read,
			bt_service.mailbox_iso_free_write,
			bt_service.mailbox_iq_report_read,
			bt_service.mailbox_iq_report_write);

		SCSC_TAG_DEBUG(BT_H4,
			"read_operation=%u, hci_event_paused=%u, data_paused=%u\n",
			bt_service.read_operation, bt_service.hci_event_paused, bt_service.data_paused);

		/* First: process TX buffer that needs to marked free */
		scsc_bt_free_credits();

#ifdef CONFIG_SCSC_QOS
		/* Second: Update the quality of service module with the number of used entries */
		scsc_bt_qos_update(NUMBER_OF_HCI_EVT, NUMBER_OF_ACL_RX);
#endif

		/* Third: process any pending HCI event that needs to be sent to userspace */
		res = scsc_bt_shm_h4_read_hci_evt(&buf[consumed], len - consumed);
		if (res < 0) {
			ret = res;
			break;
		}
		consumed += res;

		/* Fourth: process any pending ISO data that needs to be sent to userspace */
		res = scsc_bt_shm_h4_read_iso_data(&buf[consumed], len - consumed);
		if (res < 0) {
			ret = res;
			break;
		}
		consumed += res;

		/* Fifth: process any pending ACL data that needs to be sent to userspace */
		res = scsc_bt_shm_h4_read_acl_data(&buf[consumed], len - consumed);
		if (res < 0) {
			ret = res;
			break;
		}
		consumed += res;

		/* Sixth: process any number of complete packet that needs to be sent to userspace */
		res = scsc_bt_shm_h4_read_credit(&buf[consumed], len - consumed);
		if (res < 0) {
			ret = res;
			break;
		}
		consumed += res;

		/* Seventh: process any pending IQ report that needs to be sent to userspace */
		res = scsc_bt_shm_h4_read_iq_report_evt(&buf[consumed], len - consumed);
		if (res < 0) {
			ret = res;
			break;
		}
		consumed += res;
	}
#ifdef CONFIG_SCSC_QOS
	/* Last: Update the qos module to disable if possible */
	scsc_bt_qos_update(NUMBER_OF_HCI_EVT, NUMBER_OF_ACL_RX);
#endif

	if (ret == 0 && consumed == 0) {
		if (atomic_read(&bt_service.error_count) != 0 && bt_service.read_operation == BT_READ_OP_NONE)
			bt_service.read_operation = BT_READ_OP_HCI_EVT_ERROR;

		if (bt_service.read_operation == BT_READ_OP_HCI_EVT_ERROR) {
			SCSC_TAG_ERR(BT_H4, "BT_READ_OP_HCI_EVT_ERROR\n");

			/* Copy data into the userspace buffer */
			ret = scsc_hci_evt_error_read(buf, len);
			if (ret > 0) {
				consumed += ret;
				ret = 0;
			}

			if (bt_service.read_operation == BT_READ_OP_NONE)
				/* All done - set the stop condition */
				bt_service.read_operation = BT_READ_OP_STOP;
		}
	}

	/* If anything was read, generate the appropriate interrupt(s) */
	if (bt_service.bsmhcp_protocol->header.mailbox_hci_evt_read !=
	    bt_service.mailbox_hci_evt_read)
		gen_bg_int = true;

	if (bt_service.bsmhcp_protocol->header.mailbox_acl_rx_read !=
	    bt_service.mailbox_acl_rx_read ||
	    bt_service.bsmhcp_protocol->header.mailbox_acl_free_read !=
	    bt_service.mailbox_acl_free_read ||
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_rx_read !=
	    bt_service.mailbox_iso_rx_read ||
	    bt_service.bsmhcp_protocol->header_2.mailbox_iso_free_read !=
	    bt_service.mailbox_iso_free_read)
		gen_fg_int = true;

	if (bt_service.bsmhcp_protocol->header.mailbox_iq_report_read !=
	    bt_service.mailbox_iq_report_read)
		gen_fg_int = true;

	/* Update the read index for all transfer rings */
	scsc_update_read_indexes();

	/* Ensure the data is updating correctly in memory */
	wmb();

	if (gen_bg_int)
		scsc_service_mifintrbit_bit_set(bt_service.service,
						bt_service.bsmhcp_protocol->header.ap_to_bg_int_src, SCSC_MIFINTR_TARGET_WPAN);

	if (gen_fg_int)
		/* Trigger the interrupt in the mailbox */
		scsc_service_mifintrbit_bit_set(bt_service.service,
						bt_service.bsmhcp_protocol->header.ap_to_fg_int_src, SCSC_MIFINTR_TARGET_WPAN);

	if (BT_READ_OP_STOP != bt_service.read_operation)
		SCSC_TAG_DEBUG(BT_H4, "hci_evt_read=%u, acl_rx_read=%u, acl_free_read=%u, read_operation=%u, consumed=%zd, ret=%zd\n",
			       bt_service.mailbox_hci_evt_read, bt_service.mailbox_acl_rx_read, bt_service.mailbox_acl_free_read, bt_service.read_operation, consumed, ret);

	/* Decrease the H4 readers counter */
	atomic_dec(&bt_service.h4_readers);

	return ret == 0 ? consumed : ret;
}

ssize_t scsc_bt_shm_h4_write(struct file *file, const char __user *buf, size_t count, loff_t *offset)
{
	size_t  length;
	size_t  hci_pkt_len;
	ssize_t written = 0;
	ssize_t ret = 0;

	SCSC_TAG_DEBUG(BT_H4, "enter\n");

	UNUSED(file);
	UNUSED(offset);

	/* Don't allow any writes after service has been closed */
	if (!bt_service.service_started)
		return -EIO;

	/* Only 1 writer is allowed */
	if (atomic_inc_return(&bt_service.h4_writers) != 1) {
		atomic_dec(&bt_service.h4_writers);
		return -EIO;
	}

	/* Has en error been detect then just return with an error */
	if (atomic_read(&bt_service.error_count) != 0) {
		ret = -EIO;
		if (bt_service.recovery_waiting) {
			SCSC_TAG_DEBUG(BT_H4, "waiting for reset after recovery\n");
			ret = wait_event_interruptible_timeout(bt_service.read_wait, bt_service.recovery_waiting, HZ);
			if (ret == 0)
				ret = -EAGAIN;
		}
		atomic_dec(&bt_service.h4_writers);
		return ret;
	}

	while (written != count && ret == 0) {
		length = min(count - written, sizeof(h4_write_buffer) - bt_service.h4_write_offset);

		SCSC_TAG_DEBUG(BT_H4,
			       "count: %zu, length: %zu, h4_write_offset: %zu, written:%zu, size:%zu\n",
			       count, length, bt_service.h4_write_offset, written, sizeof(h4_write_buffer));

		/* Is there room in the temp buffer */
		if (length == 0) {
			SCSC_TAG_ERR(BT_H4, "no room in the buffer\n");
			atomic_inc(&bt_service.error_count);
			ret = -EIO;
			break;
		}

		/* Copy the userspace data to the target buffer */
		ret = copy_from_user(&h4_write_buffer[bt_service.h4_write_offset], &buf[written], length);
		if (ret == 0) {
			/* Is there enough data to include a HCI command header and is the type a HCI_COMMAND_PKT */
			if ((length + bt_service.h4_write_offset) >= H4DMUX_HEADER_HCI &&
			    h4_write_buffer[H4_HEADER_INDEX] == HCI_COMMAND_PKT) {
				/* Extract the HCI command packet length */
				hci_pkt_len = (size_t)(h4_write_buffer[3] + 3);

				/* Is it a complete packet available */
				if ((hci_pkt_len + 1) <= (length + bt_service.h4_write_offset)) {
					/* Transfer the packet to the HCI command transfer ring */
					ret = scsc_bt_shm_h4_hci_cmd_write(&h4_write_buffer[1], hci_pkt_len);
					if (ret >= 0) {
						written += ((hci_pkt_len + 1) - bt_service.h4_write_offset);
						bt_service.h4_write_offset = 0;
						ret = 0;
					}
				} else {
					/* Still needing data to have the complete packet */
					SCSC_TAG_WARNING(BT_H4,
							 "missing data (need=%zu, got=%zu)\n",
							 (hci_pkt_len + 1), (length + bt_service.h4_write_offset));

					written += length;
					bt_service.h4_write_offset += (u32)length;
				}
				/* Is there enough data to include a ACL data header and is the
				 * type a HCI_ACLDATA_PKT
				 */
			} else if ((length + bt_service.h4_write_offset) >= H4DMUX_HEADER_ACL &&
				    h4_write_buffer[H4_HEADER_INDEX] == HCI_ACLDATA_PKT) {
				/* Extract the ACL data packet length */
				hci_pkt_len = HCI_ACL_DATA_GET_LENGTH(&h4_write_buffer[H4_HEADER_SIZE]);

				/* Sanity check on the packet length */
				if (hci_pkt_len > BSMHCP_ACL_PACKET_SIZE) {
					SCSC_TAG_ERR(BT_H4,
					   "ACL packet length is larger than read buffer size specifies (%zu > %u)\n",
					   hci_pkt_len, BSMHCP_ACL_PACKET_SIZE);

					atomic_inc(&bt_service.error_count);
					ret = -EIO;
					break;
				}

				/* Is it a complete packet available */
				if ((hci_pkt_len + H4DMUX_HEADER_ACL) <= (length + bt_service.h4_write_offset)) {
					/* Transfer the packet to the ACL data transfer ring */
					ret = scsc_bt_shm_h4_acl_write(&h4_write_buffer[1], hci_pkt_len + 4);
					if (ret >= 0) {
						written += ((hci_pkt_len + 5) - bt_service.h4_write_offset);
						bt_service.h4_write_offset = 0;
						ret = 0;
					}
				} else {
					/* Still needing data to have the complete packet */
					SCSC_TAG_WARNING(BT_H4,
							 "missing data (need=%zu, got=%zu)\n",
							 (hci_pkt_len + 5), (length - bt_service.h4_write_offset));

					written += length;
					bt_service.h4_write_offset += (u32)length;
				}

			/* Is there enough data to include a ISO data header and is the
			 * type a HCI_ISODATA_PKT
			 */
			} else if ((length + bt_service.h4_write_offset) >= H4DMUX_HEADER_ISO &&
				   h4_write_buffer[H4_HEADER_INDEX] == HCI_ISODATA_PKT) {
				/* Extract the ISO Data packet length */
				hci_pkt_len = HCI_ISO_DATA_GET_LENGTH(&h4_write_buffer[H4_HEADER_SIZE]);

				/* Sanity check on the packet length */
				if (hci_pkt_len > BSMHCP_ISO_PACKET_SIZE) {
					SCSC_TAG_ERR(BT_H4,
					     "ISO Data length is larger than read buffer size specifies (%zu > %u)\n",
					     hci_pkt_len, BSMHCP_ISO_PACKET_SIZE);

					atomic_inc(&bt_service.error_count);
					ret = -EIO;
					break;
				}

				/* Is a complete packet available */
				if ((hci_pkt_len + H4DMUX_HEADER_ISO) <= (length + bt_service.h4_write_offset)) {
					/* Transfer the packet to the ISO data transfer ring */
					ret = scsc_bt_shm_h4_iso_write(&h4_write_buffer[H4_HEADER_SIZE],
								       hci_pkt_len + ISODATA_HEADER_SIZE);

					if (ret >= 0) {
						written += ((hci_pkt_len + H4DMUX_HEADER_ISO) -
							    bt_service.h4_write_offset);

						bt_service.h4_write_offset = 0;
						ret = 0;
					}
				} else {
					/* Still needing data to have the complete packet */
					SCSC_TAG_WARNING(BT_H4,
							 "missing ISO data (need=%zu, got=%zu)\n",
							 (hci_pkt_len + H4DMUX_HEADER_ISO),
							 (length - bt_service.h4_write_offset));

					written += length;
					bt_service.h4_write_offset += (u32)length;
				}

				/* Is there less data than a header then just wait for more */
			} else if (length <= H4DMUX_HEADER_MAX_SIZE) {
				bt_service.h4_write_offset += length;
				written += length;
				/* Header is unknown - unable to proceed */
			} else {
				atomic_inc(&bt_service.error_count);
				ret = -EIO;
			}
		} else {
			SCSC_TAG_WARNING(BT_H4, "copy_from_user returned: %zu\n", ret);
			ret = -EACCES;
		}
	}

	SCSC_TAG_DEBUG(BT_H4,
		       "h4_write_offset=%zu, ret=%zu, written=%zu\n",
		       bt_service.h4_write_offset, ret, written);

	/* Decrease the H4 readers counter */
	atomic_dec(&bt_service.h4_writers);

	return ret == 0 ? written : ret;
}

unsigned int scsc_bt_shm_h4_poll(struct file *file, poll_table *wait)
{
	/* Add the wait queue to the polling queue */
	poll_wait(file, &bt_service.read_wait, wait);

	/* Return immediately if service has been closed */
	if (!bt_service.service_started)
		return POLLOUT;

	/* Has en error been detect then just return with an error */
	if ((scsc_read_data_available() &&
	     bt_service.read_operation != BT_READ_OP_STOP) ||
	    (bt_service.read_operation != BT_READ_OP_NONE &&
	     bt_service.read_operation != BT_READ_OP_STOP) ||
	    (bt_service.read_operation != BT_READ_OP_STOP &&
	     (atomic_read(&bt_service.error_count) != 0 ||
	     bt_service.bsmhcp_protocol->header.panic_deathbed_confession))) {

		if (bt_service.recovery_waiting) {
			SCSC_TAG_DEBUG(BT_H4, "waiting for reset after recovery\n");
			return 0;	/* skip */
		}

		SCSC_TAG_DEBUG(BT_H4, "queue(s) changed\n");
		return POLLIN | POLLRDNORM; /* readeable */
	}

	SCSC_TAG_DEBUG(BT_H4, "no change\n");

	return POLLOUT; /* writeable */
}

/* Initialise the shared memory interface */
int scsc_bt_shm_init(void)
{
	/* Get kmem pointer to the shared memory ref */
	bt_service.bsmhcp_protocol = scsc_mx_service_mif_addr_to_ptr(bt_service.service, bt_service.bsmhcp_ref);
	if (bt_service.bsmhcp_protocol == NULL) {
		SCSC_TAG_ERR(BT_COMMON, "couldn't map kmem to shm_ref 0x%08x\n", (u32)bt_service.bsmhcp_ref);
		return -ENOMEM;
	}

	/* Clear the protocol shared memory area */
	memset(bt_service.bsmhcp_protocol, 0, sizeof(*bt_service.bsmhcp_protocol));
	bt_service.bsmhcp_protocol->header.magic_value = BSMHCP_PROTOCOL_MAGICVALUE;
	bt_service.bsmhcp_protocol->header_2.magic_value = BSMHCP_PROTOCOL_V2_MAGICVALUE;
	bt_service.bsmhcp_protocol->header.bsmhcp_version = BSMHCP_VERSION;
	bt_service.mailbox_hci_evt_read = 0;
	bt_service.mailbox_acl_rx_read = 0;
	bt_service.mailbox_acl_free_read = 0;
	bt_service.mailbox_acl_free_read_scan = 0;
	bt_service.mailbox_iq_report_read = 0;
	bt_service.mailbox_iso_rx_read = 0;
	bt_service.mailbox_iso_free_read = 0;
	bt_service.mailbox_iso_free_read_scan = 0;
	bt_service.read_index = 0;
	bt_service.allocated_count = 0;
	bt_service.freed_count = 0;
	bt_service.iq_reports_enabled = false;
	bt_service.iso_allocated_count = 0;
	bt_service.iso_freed_count = 0;
	h4_irq_mask = 0;

	/* Initialise the interrupt handlers */
	if (scsc_bt_shm_init_interrupt() < 0) {
		SCSC_TAG_ERR(BT_COMMON, "Failed to register IRQ bits\n");
		return -EIO;
	}
	scsc_bt_big_handle_list_init();

	return 0;
}

/* Terminate the shared memory interface, stopping its thread.
 *
 * Note: The service must be stopped prior to calling this function.
 *       The shared memory can only be released after calling this function.
 */
void scsc_bt_shm_exit(void)
{
	u16 irq_num = 0;

	/* Release IRQs */
	if (bt_service.bsmhcp_protocol != NULL) {
		if (h4_irq_mask & 1 << irq_num++)
			scsc_service_mifintrbit_unregister_tohost(
			    bt_service.service, bt_service.bsmhcp_protocol->header.bg_to_ap_int_src, SCSC_MIFINTR_TARGET_WPAN);
		if (h4_irq_mask & 1 << irq_num++)
			scsc_service_mifintrbit_free_fromhost(
			    bt_service.service, bt_service.bsmhcp_protocol->header.ap_to_bg_int_src, SCSC_MIFINTR_TARGET_WPAN);
		if (h4_irq_mask & 1 << irq_num++)
			scsc_service_mifintrbit_free_fromhost(
			    bt_service.service, bt_service.bsmhcp_protocol->header.ap_to_fg_int_src, SCSC_MIFINTR_TARGET_WPAN);
	}

	/* Clear all control structures */
	bt_service.last_alloc = 0;
	bt_service.iso_last_alloc = 0;
	bt_service.hci_event_paused = false;
	bt_service.data_paused = false;
	bt_service.data_paused_conn_hdl = 0;
	bt_service.bsmhcp_protocol = NULL;

	memset(bt_service.allocated, 0, sizeof(bt_service.allocated));
	memset(bt_service.iso_allocated, 0, sizeof(bt_service.iso_allocated));
	memset(bt_service.connection_handle_list, 0, sizeof(bt_service.connection_handle_list));
}
