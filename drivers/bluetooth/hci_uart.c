/* 
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2000-2001 Qualcomm Incorporated

   Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation;

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
   CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES 
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN 
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF 
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

   ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS, 
   COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS 
   SOFTWARE IS DISCLAIMED.
*/

/*
 * BlueZ HCI UART driver.
 *
 * $Id: hci_uart.c,v 1.1 2001/06/01 08:12:10 davem Exp $    
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/version.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/poll.h>

#include <linux/malloc.h>
#include <linux/tty.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/signal.h>
#include <linux/ioctl.h>
#include <linux/skbuff.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/bluez.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_uart.h>

#ifndef HCI_UART_DEBUG
#undef  DBG
#define DBG( A... )
#undef  DMP
#define DMP( A... )
#endif

/* ------- Interface to HCI layer ------ */
/* Initialize device */
int n_hci_open(struct hci_dev *hdev)
{
	DBG("%s %p", hdev->name, hdev);

	/* Nothing to do for UART driver */

	hdev->flags |= HCI_RUNNING;

	return 0;
}

/* Reset device */
int n_hci_flush(struct hci_dev *hdev)
{
	struct n_hci *n_hci  = (struct n_hci *) hdev->driver_data;
	struct tty_struct *tty = n_hci->tty;

	DBG("hdev %p tty %p", hdev, tty);

	/* Drop TX queue */
	bluez_skb_queue_purge(&n_hci->txq);

	/* Flush any pending characters in the driver and discipline. */
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);

	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);

	return 0;
}

/* Close device */
int n_hci_close(struct hci_dev *hdev)
{
	DBG("hdev %p", hdev);

	hdev->flags &= ~HCI_RUNNING;

	n_hci_flush(hdev);

	return 0;
}

int n_hci_tx_wakeup(struct n_hci *n_hci)
{
	register struct tty_struct *tty = n_hci->tty;

	if (test_and_set_bit(TRANS_SENDING, &n_hci->tx_state)) {
		set_bit(TRANS_WAKEUP, &n_hci->tx_state);
		return 0;
	}

	DBG("");
	do {
		register struct sk_buff *skb;
		register int len;

		clear_bit(TRANS_WAKEUP, &n_hci->tx_state);

		if (!(skb = skb_dequeue(&n_hci->txq)))
			break;

		DMP(skb->data, skb->len);

		/* Send frame to TTY driver */
		tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
		len = tty->driver.write(tty, 0, skb->data, skb->len);

		n_hci->hdev.stat.byte_tx += len;

		DBG("sent %d", len);

		if (len == skb->len) {
			/* Full frame was sent */
			bluez_skb_free(skb);
		} else {
			/* Subtract sent part and requeue  */
			skb_pull(skb, len);
			skb_queue_head(&n_hci->txq, skb);
		}
	} while (test_bit(TRANS_WAKEUP, &n_hci->tx_state));
	clear_bit(TRANS_SENDING, &n_hci->tx_state);

	return 0;
}

/* Send frames from HCI layer */
int n_hci_send_frame(struct sk_buff *skb)
{
	struct hci_dev* hdev = (struct hci_dev *) skb->dev;
	struct tty_struct *tty;
	struct n_hci *n_hci;

	if (!hdev) {
		ERR("Frame for uknown device (hdev=NULL)");
		return -ENODEV;
	}

	if (!(hdev->flags & HCI_RUNNING))
		return -EBUSY;

	n_hci = (struct n_hci *) hdev->driver_data;
	tty = n_hci2tty(n_hci);

	DBG("%s: type %d len %d", hdev->name, skb->pkt_type, skb->len);

	switch (skb->pkt_type) {
		case HCI_COMMAND_PKT:
			hdev->stat.cmd_tx++;
			break;

                case HCI_ACLDATA_PKT:
			hdev->stat.acl_tx++;
                        break;

		case HCI_SCODATA_PKT:
			hdev->stat.cmd_tx++;
                        break;
	};

	/* Prepend skb with frame type and queue */
	memcpy(skb_push(skb, 1), &skb->pkt_type, 1);
	skb_queue_tail(&n_hci->txq, skb);

	n_hci_tx_wakeup(n_hci);

	return 0;
}

/* ------ LDISC part ------ */

/* n_hci_tty_open
 * 
 *     Called when line discipline changed to N_HCI.
 *     
 * Arguments:    
 *     tty    pointer to tty info structure
 * Return Value:    
 *     0 if success, otherwise error code
 */
static int n_hci_tty_open(struct tty_struct *tty)
{
	struct n_hci *n_hci = tty2n_hci(tty);
	struct hci_dev *hdev;

	DBG("tty %p", tty);

	if (n_hci)
		return -EEXIST;

	if (!(n_hci = kmalloc(sizeof(struct n_hci), GFP_KERNEL))) {
		ERR("Can't allocate controll structure");
		return -ENFILE;
	}
	memset(n_hci, 0, sizeof(struct n_hci));

	/* Initialize and register HCI device */
	hdev = &n_hci->hdev;

	hdev->type = HCI_UART;
	hdev->driver_data = n_hci;

	hdev->open  = n_hci_open;
	hdev->close = n_hci_close;
	hdev->flush = n_hci_flush;
	hdev->send  = n_hci_send_frame;

	if (hci_register_dev(hdev) < 0) {
		ERR("Can't register HCI device %s", hdev->name);
		kfree(n_hci);
		return -ENODEV;
	}

	tty->disc_data = n_hci;
	n_hci->tty = tty;

	spin_lock_init(&n_hci->rx_lock);
	n_hci->rx_state = WAIT_PACKET_TYPE;

	skb_queue_head_init(&n_hci->txq);

	MOD_INC_USE_COUNT;

	/* Flush any pending characters in the driver and discipline. */
	if (tty->ldisc.flush_buffer)
		tty->ldisc.flush_buffer(tty);

	if (tty->driver.flush_buffer)
		tty->driver.flush_buffer(tty);

	return 0;
}

/* n_hci_tty_close()
 *
 *    Called when the line discipline is changed to something
 *    else, the tty is closed, or the tty detects a hangup.
 */
static void n_hci_tty_close(struct tty_struct *tty)
{
	struct n_hci *n_hci = tty2n_hci(tty);
	struct hci_dev *hdev = &n_hci->hdev;

	DBG("tty %p hdev %p", tty, hdev);

	if (n_hci != NULL) {
		n_hci_close(hdev);

		if (hci_unregister_dev(hdev) < 0) {
			ERR("Can't unregister HCI device %s",hdev->name);
		}

		hdev->driver_data = NULL;
		tty->disc_data = NULL;
		kfree(n_hci);

		MOD_DEC_USE_COUNT;
	}
}

/* n_hci_tty_wakeup()
 *
 *    Callback for transmit wakeup. Called when low level
 *    device driver can accept more send data.
 *
 * Arguments:        tty    pointer to associated tty instance data
 * Return Value:    None
 */
static void n_hci_tty_wakeup( struct tty_struct *tty )
{
	struct n_hci *n_hci = tty2n_hci(tty);

	DBG("");

	if (!n_hci)
		return;

	tty->flags &= ~(1 << TTY_DO_WRITE_WAKEUP);

	if (tty != n_hci->tty)
		return;

	n_hci_tx_wakeup(n_hci);
}

/* n_hci_tty_room()
 * 
 *    Callback function from tty driver. Return the amount of 
 *    space left in the receiver's buffer to decide if remote
 *    transmitter is to be throttled.
 *
 * Arguments:        tty    pointer to associated tty instance data
 * Return Value:    number of bytes left in receive buffer
 */
static int n_hci_tty_room (struct tty_struct *tty)
{
	return 65536;
}

static inline int n_hci_check_data_len(struct n_hci *n_hci, int len)
{
	register int room = skb_tailroom(n_hci->rx_skb);

	DBG("len %d room %d", len, room);
	if (!len) {
		DMP(n_hci->rx_skb->data, n_hci->rx_skb->len);
		hci_recv_frame(n_hci->rx_skb);
	} else if (len > room) {
		ERR("Data length is to large");
		bluez_skb_free(n_hci->rx_skb);
		n_hci->hdev.stat.err_rx++;
	} else {
		n_hci->rx_state = WAIT_DATA;
		n_hci->rx_count = len;
		return len;
	}

	n_hci->rx_state = WAIT_PACKET_TYPE;
	n_hci->rx_skb   = NULL;
	n_hci->rx_count = 0;
	return 0;
}

static inline void n_hci_rx(struct n_hci *n_hci, const __u8 * data, char *flags, int count)
{
	register const char *ptr;
	hci_event_hdr *eh;
	hci_acl_hdr   *ah;
	hci_sco_hdr   *sh;
	register int len, type, dlen;

	DBG("count %d state %d rx_count %d", count, n_hci->rx_state, n_hci->rx_count);

	n_hci->hdev.stat.byte_rx += count;

	ptr = data;
	while (count) {
		if (n_hci->rx_count) {
			len = MIN(n_hci->rx_count, count);
			memcpy(skb_put(n_hci->rx_skb, len), ptr, len);
			n_hci->rx_count -= len; count -= len; ptr += len;

			if (n_hci->rx_count)
				continue;

			switch (n_hci->rx_state) {
				case WAIT_DATA:
					DBG("Complete data");

					DMP(n_hci->rx_skb->data, n_hci->rx_skb->len);

					hci_recv_frame(n_hci->rx_skb);

					n_hci->rx_state = WAIT_PACKET_TYPE;
					n_hci->rx_skb = NULL;
					continue;

				case WAIT_EVENT_HDR:
					eh = (hci_event_hdr *) n_hci->rx_skb->data;

					DBG("Event header: evt 0x%2.2x plen %d", eh->evt, eh->plen);

					n_hci_check_data_len(n_hci, eh->plen);
					continue;

				case WAIT_ACL_HDR:
					ah = (hci_acl_hdr *) n_hci->rx_skb->data;
					dlen = __le16_to_cpu(ah->dlen);

					DBG("ACL header: dlen %d", dlen);

					n_hci_check_data_len(n_hci, dlen);
					continue;

				case WAIT_SCO_HDR:
					sh = (hci_sco_hdr *) n_hci->rx_skb->data;

					DBG("SCO header: dlen %d", sh->dlen);

					n_hci_check_data_len(n_hci, sh->dlen);
					continue;
			};
		}

		/* WAIT_PACKET_TYPE */
		switch (*ptr) {
			case HCI_EVENT_PKT:
				DBG("Event packet");
				n_hci->rx_state = WAIT_EVENT_HDR;
				n_hci->rx_count = HCI_EVENT_HDR_SIZE;
				type = HCI_EVENT_PKT;
				break;

			case HCI_ACLDATA_PKT:
				DBG("ACL packet");
				n_hci->rx_state = WAIT_ACL_HDR;
				n_hci->rx_count = HCI_ACL_HDR_SIZE;
				type = HCI_ACLDATA_PKT;
				break;

			case HCI_SCODATA_PKT:
				DBG("SCO packet");
				n_hci->rx_state = WAIT_SCO_HDR;
				n_hci->rx_count = HCI_SCO_HDR_SIZE;
				type = HCI_SCODATA_PKT;
				break;

			default:
				ERR("Unknown HCI packet type %2.2x", (__u8)*ptr);
				n_hci->hdev.stat.err_rx++;
				ptr++; count--;
				continue;
		};
		ptr++; count--;

		/* Allocate packet */
		if (!(n_hci->rx_skb = bluez_skb_alloc(HCI_MAX_READ, GFP_ATOMIC))) {
			ERR("Can't allocate mem for new packet");

			n_hci->rx_state = WAIT_PACKET_TYPE;
			n_hci->rx_count = 0;
			return;
		}
		n_hci->rx_skb->dev = (void *) &n_hci->hdev;
		n_hci->rx_skb->pkt_type = type;
	}
}

/* n_hci_tty_receive()
 * 
 *     Called by tty low level driver when receive data is
 *     available.
 *     
 * Arguments:  tty          pointer to tty isntance data
 *             data         pointer to received data
 *             flags        pointer to flags for data
 *             count        count of received data in bytes
 *     
 * Return Value:    None
 */
static void n_hci_tty_receive(struct tty_struct *tty, const __u8 * data, char *flags, int count)
{
	struct n_hci *n_hci = tty2n_hci(tty);

	if (!n_hci || tty != n_hci->tty)
		return;

	spin_lock(&n_hci->rx_lock);
	n_hci_rx(n_hci, data, flags, count);
	spin_unlock(&n_hci->rx_lock);

	if (test_and_clear_bit(TTY_THROTTLED,&tty->flags) && tty->driver.unthrottle)
		tty->driver.unthrottle(tty);
}

/* n_hci_tty_ioctl()
 *
 *    Process IOCTL system call for the tty device.
 *
 * Arguments:
 *
 *    tty        pointer to tty instance data
 *    file       pointer to open file object for device
 *    cmd        IOCTL command code
 *    arg        argument for IOCTL call (cmd dependent)
 *
 * Return Value:    Command dependent
 */
static int n_hci_tty_ioctl (struct tty_struct *tty, struct file * file,
                            unsigned int cmd, unsigned long arg)
{
	struct n_hci *n_hci = tty2n_hci(tty);
	int error = 0;

	DBG("");

	/* Verify the status of the device */
	if (!n_hci)
		return -EBADF;

	switch (cmd) {
		default:
			error = n_tty_ioctl(tty, file, cmd, arg);
			break;
	};

	return error;
}

/*
 * We don't provide read/write/poll interface for user space.
 */
static ssize_t n_hci_tty_read(struct tty_struct *tty, struct file *file, unsigned char *buf, size_t nr)
{
	return 0;
}
static ssize_t n_hci_tty_write(struct tty_struct *tty, struct file *file, const unsigned char *data, size_t count)
{
	return 0;
}
static unsigned int n_hci_tty_poll(struct tty_struct *tty, struct file *filp, poll_table *wait)
{
	return 0;
}

int __init n_hci_init(void)
{
	static struct tty_ldisc n_hci_ldisc;
	int err;

	INF("BlueZ HCI UART driver ver %s Copyright (C) 2000,2001 Qualcomm Inc", 
		BLUEZ_VER);
	INF("Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>");

	/* Register the tty discipline */

	memset(&n_hci_ldisc, 0, sizeof (n_hci_ldisc));
	n_hci_ldisc.magic       = TTY_LDISC_MAGIC;
	n_hci_ldisc.name        = "n_hci";
	n_hci_ldisc.open        = n_hci_tty_open;
	n_hci_ldisc.close       = n_hci_tty_close;
	n_hci_ldisc.read        = n_hci_tty_read;
	n_hci_ldisc.write       = n_hci_tty_write;
	n_hci_ldisc.ioctl       = n_hci_tty_ioctl;
	n_hci_ldisc.poll        = n_hci_tty_poll;
	n_hci_ldisc.receive_room= n_hci_tty_room;
	n_hci_ldisc.receive_buf = n_hci_tty_receive;
	n_hci_ldisc.write_wakeup= n_hci_tty_wakeup;

	if ((err = tty_register_ldisc(N_HCI, &n_hci_ldisc))) {
		ERR("Can't register HCI line discipline (%d)", err);
		return err;
	}

	return 0;
}

void n_hci_cleanup(void)
{
	int err;

	/* Release tty registration of line discipline */
	if ((err = tty_register_ldisc(N_HCI, NULL)))
		ERR("Can't unregister HCI line discipline (%d)", err);
}

module_init(n_hci_init);
module_exit(n_hci_cleanup);
