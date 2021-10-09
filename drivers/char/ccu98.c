/*
 * Serial driver for PC-9800's legacy serial port
 *
 *  Copyright (C) 2001	Linux/98 project
 *			Kyoto University Microcomputer Clib.
 *
 * Written by TAKAI Kousuke  <tak@kmc.kyoto-u.ac.jp>.
 *
 * Based on:
 *  - 8251 (SIO) driver for 2.1.57 kernel (integrated in serial.c)
 *	primarily written by TABATA Yusuke
 */

/*
 * To do:
 *  - FIFOCCU/98 support
 *  - V-FAST mode support
 *
 * Plan:
 *  - PC-9861/PC-9801-101 support?
 *	(but I have none of these cards - tak)
 */

/*
 * Current problem:
 *  - With this driver 8251 always honors CTS signal despite of CRTSCTS
 *    settings of termios.
 */

#define DEBUG 1

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/major.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/serial.h>
#include <linux/serialP.h>
#include <linux/generic_serial.h>
#include <asm/io.h>
#include <asm/pc9800.h>
#include <asm/uaccess.h>

#define CCU98_MAGIC	0x98019821

#define CCU98_IRQ	4

#define CCU98_RX		(0x0030)
#define CCU98_TX		(0x0030)

#define CCU98_CMR		(0x0032)
#define  USART_CMR_TXE		(1 << 0)
#define  USART_CMR_DTR		(1 << 1)
#define  USART_CMR_RXE		(1 << 2)
#define  USART_CMR_SBRK		(1 << 3)
#define  USART_CMR_ER		(1 << 4)
#define  USART_CMR_RTS		(1 << 5)
#define  USART_CMR_IR		(1 << 6)
#define  USART_CMR_EH		(1 << 7)
#define CCU98_MDR		(0x0032)
#define  USART_MDR_B_SYNC	(0 << 0) /* synchronous mode	*/
#define  USART_MDR_B_x1		(1 << 0) /* baud rate		*/
#define  USART_MDR_B_x16	(2 << 0)
#define  USART_MDR_B_x64	(3 << 0)
#define  USART_MDR_L_5		(0 << 2) /* character length	*/
#define  USART_MDR_L_6		(1 << 2)
#define  USART_MDR_L_7		(2 << 2)
#define  USART_MDR_L_8		(3 << 2)
#define  USART_MDR_PEN		(1 << 4) /* parity enable	*/
#define  USART_MDR_EP		(1 << 5) /* even parity		*/
/*				(0 << 6)    (invalid)		*/
#define  USART_MDR_S_1		(1 << 6) /* # of stop bits	*/
#define  USART_MDR_S_1_5	(2 << 6)
#define  USART_MDR_S_2		(3 << 6)
#define  USART_MDR_ESD		(1 << 6) /* ext. sync detect	*/
#define  USART_MDR_SCS		(1 << 7) /* single char sync	*/
#define CCU98_LSR		(0x0032)
#define  USART_LSR_TXRDY	(1 << 0)
#define  USART_LSR_RXRDY	(1 << 1)
#define  USART_LSR_TE		(1 << 2)
#define  USART_LSR_PE		(1 << 3)
#define  USART_LSR_OE		(1 << 4)
#define  USART_LSR_FE		(1 << 5)
#define  USART_LSR_SYNDET	(1 << 6)
#define  USART_LSR_DSR		(1 << 7)

#define CCU98_MSR		(0x0033) /* 8255 PIO port B */
#define  CCU98_MSR_CD		(1 << 5)
#define  CCU98_MSR_CS		(1 << 6)
#define  CCU98_MSR_CI		(1 << 7)
#define CCU98_IER		(0x0035) /* 8255 PIO port C */
#define  CCU98_IER_RXRE		(1 << 0)
#define  CCU98_IER_TXEE		(1 << 1)
#define  CCU98_IER_TXRE		(1 << 2)
#define CCU98_IERC		(0x0037) /* 8255 PIO control register */
#define  CCU98_IERC_RXRE	(0 << 1)
#define  CCU98_IERC_TXEE	(1 << 1)
#define  CCU98_IERC_TXRE	(2 << 1)

#define CCU98_EXTCTL		(0x0434)
#define  CCU98_EXTCTL_DISABLE	(1 << 0)
#define  CCU98_EXTCTL_NOQUAD	(1 << 1)

#define CCU98_ISR_PASS_LIMIT	16

#define NR_CCU98_PORTS	1

#if NR_CCU98_PORTS == 1
# define OUT_CMR(p, data)	outb ((data), CCU98_CMR)
# define OUT_MDR(p, data)	outb ((data), CCU98_MDR)
# define IN_LSR(p)		inb (CCU98_LSR)
# define IN_MSR(p)		inb (CCU98_MSR)
#else
# define OUT_CMR(p, data)	outb ((data), (p)->port[PORT_INDEX_CMR])
# define OUT_MDR(p, data)	outb ((data), (p)->port[PORT_INDEX_MDR])
# define IN_LSR(p)		outb ((p)->port[PORT_INDEX_LSR])
# define IN_MSR(p)		outb ((p)->port[PORT_INDEX_MSR])
#endif

static int irq[NR_CCU98_PORTS] = {
	4,
#if NR_CCU98_PORTS > 1
	3,
#endif
#if NR_CCU98_PORTS > 2
	12,
#endif
};
MODULE_PARM (irq, "1-" __MODULE_STRING (NR_CCU98_PORTS) "i");
MODULE_PARM_DESC (irq, "IRQ list for PC-9800 8251 based serial ports");

struct ccu98_port {
	struct gs_port	gs;

	u8 oldcmr;
	u8 oldmsr;
	char xchar;
	u8 irq;
	spinlock_t lock;
	struct async_icount icount;
	struct tq_struct tqueue;
	wait_queue_head_t delta_msr_wait;
	int current_baud;
	unsigned int	broken :1;
};

static struct ccu98_port ccu98_port[NR_CCU98_PORTS];
static struct tty_struct *ccu98_table[NR_CCU98_PORTS];
static struct termios *ccu98_termios[NR_CCU98_PORTS];
static struct termios *ccu98_termios_locked[NR_CCU98_PORTS];

#define PORT	((struct ccu98_port *) _port)

static void ccu98_disable_tx_interrupts (void *_port)
{
	pr_debug ("ccu98: disable Tx interrupts\n");
	/* This operation is atomic so that no cli/sti is needed.  */
	outb (CCU98_IERC_TXRE, CCU98_IERC);
}

static void ccu98_enable_tx_interrupts (void *_port)
{
	pr_debug ("ccu98: enable Tx interrupts\n");
	outb (CCU98_IERC_TXRE | 1, CCU98_IERC);
}

static void ccu98_disable_rx_interrupts (void *_port)
{
	pr_debug ("ccu98: disable Rx interrupts\n");
	/* This operation is atomic so that no cli/sti is needed.  */
	outb (CCU98_IERC_RXRE, CCU98_IERC);
}

static void ccu98_enable_rx_interrupts (void *_port)
{
	pr_debug ("ccu98: enable Rx interrupts\n");
	outb (CCU98_IERC_RXRE | 1, CCU98_IERC);
}

static int ccu98_get_CD (void *_p)
{
	int CD = inb (CCU98_MSR) & CCU98_MSR_CD;
	pr_debug ("ccu98: got CD = %c\n", CD ? '1' : '0');
	return CD;
}

static void shutdown_port (void *_port)
{
	pr_debug ("ccu98: shutdown port\n");

	if (PORT->gs.tty && (PORT->gs.tty->termios->c_cflag & HUPCL)) {
		/* Hangup */
		PORT->oldcmr &= ~(USART_CMR_RTS | USART_CMR_RXE
				  | USART_CMR_DTR | USART_CMR_TXE);
		OUT_CMR (PORT, PORT->oldcmr);
	}
}

#define _W	asm ("out%B0 %%al,%0" : : "N" (0x5F))
#define WAIT5	_W; _W; _W; _W; _W	

static void reset_usart (void)
{
	outb (0x00, CCU98_CMR);	/* dummy command */
	WAIT5;
	outb (0x00, CCU98_CMR);	/* dummy command */
	WAIT5;
	outb (0x00, CCU98_CMR);	/* dummy command */
	WAIT5;
	outb (USART_CMR_IR, CCU98_CMR);
}

static int set_speed (void *_port)
{
	int baud = PORT->gs.baud ? PORT->gs.baud : 9600;
	int divisor = PORT->gs.baud_base / baud;
	int error = PORT->gs.baud / 100;
	int real_speed;

	if (PORT->gs.baud
	    && (!divisor
		|| ((real_speed = PORT->gs.baud_base / divisor)
		    <= PORT->gs.baud - error)
		|| PORT->gs.baud + error <= real_speed)) {
		printk (KERN_ERR "ccu98: cannot set speed to %dbps\n",
			PORT->gs.baud);
		return -EIO;
	}
	/* Set speed to 8253 timer.  */
	outb (0xB6, 0x77);	/* set counter#2 to mode 3 */
	outb (divisor, 0x75);	/* LSB */
	outb (divisor >> 8, 0x75); /* MSB */
	pr_debug ("ccu98: TCU divisor is %04x\n", divisor);

	return 0;
}

static int ccu98_set_real_termios (void *_port)
{
	unsigned long flags;
	u8 tmp;

	if (!PORT->gs.tty)
		return 0;

	set_speed (PORT);

	/* Transition to B0 */
	if (PORT->current_baud && !PORT->gs.baud) {
		spin_lock_irqsave (&PORT->lock, flags);
		PORT->oldcmr &= ~(USART_CMR_DTR | USART_CMR_RTS);
		OUT_CMR (PORT, PORT->oldcmr);
		spin_unlock_irqrestore (&PORT->lock, flags);
	}
	/* Transition away from B0 */
	else if (!PORT->current_baud && PORT->gs.baud) {
		spin_lock_irqsave (&PORT->lock, flags);
		PORT->oldcmr |= USART_CMR_DTR;
		if (!C_CRTSCTS (PORT->gs.tty)
		    || !test_bit (TTY_THROTTLED, &PORT->gs.tty->flags))
			PORT->oldcmr |= USART_CMR_RTS;
		OUT_CMR (PORT,PORT->oldcmr);
		spin_unlock_irqrestore (&PORT->lock, flags);
	}

	OUT_CMR (PORT, USART_CMR_IR);
	WAIT5;
	tmp = USART_MDR_B_x16;
	switch (C_CSIZE (PORT->gs.tty)) {
	case CS5:	tmp |= USART_MDR_L_5;	break;
	case CS6:	tmp |= USART_MDR_L_6;	break;
	case CS7:	tmp |= USART_MDR_L_7;	break;
	case CS8:	tmp |= USART_MDR_L_8;	break;
	}
	if (C_PARENB (PORT->gs.tty))
		tmp |= USART_MDR_PEN;
	if (!C_PARODD (PORT->gs.tty))
		tmp |= USART_MDR_EP;
	if (C_CSTOPB (PORT->gs.tty))
		tmp |= USART_MDR_S_2;
	else
		tmp |= USART_MDR_S_1;
	OUT_MDR (PORT, tmp);
	WAIT5;
	OUT_CMR (PORT, PORT->oldcmr);
	PORT->current_baud = PORT->gs.baud;
	return 0;
}

static int chars_in_buffer (void *_port)
{
	return IN_LSR (PORT) & USART_LSR_RXRDY ? 1 : 0;
}

static void close (void *_port)
{
	/* Only called at PORT->gs.count == 0  */
	MOD_DEC_USE_COUNT;
	free_irq (PORT->irq, PORT);
}

static void ccu98_hungup (void *_port)
{
	MOD_DEC_USE_COUNT;
}

static void getserial (void *_port, struct serial_struct *sp)
{
	sp->type = PORT_8251;
	sp->xmit_fifo_size = 1;
	sp->line = 0;
	sp->port = CCU98_TX;
	sp->irq = CCU98_IRQ;
}

static struct real_driver ccu98_real_driver = {
	disable_tx_interrupts:	ccu98_disable_tx_interrupts,
	enable_tx_interrupts:	ccu98_enable_tx_interrupts,
	disable_rx_interrupts:	ccu98_disable_rx_interrupts,
	enable_rx_interrupts:	ccu98_enable_rx_interrupts,
	get_CD:			ccu98_get_CD,
	shutdown_port:		shutdown_port,
	set_real_termios:	ccu98_set_real_termios,
	chars_in_buffer:	chars_in_buffer,
	close:			close,
	hungup:			ccu98_hungup,
	getserial:		getserial,
};

static inline void ccu98_sched_event (void *_port, int event)
{
	PORT->gs.event |= 1 << event;
	queue_task (&PORT->tqueue, &tq_immediate);
	mark_bh (IMMEDIATE_BH);
}

static inline void ccu98_receive_char (void *_port, u8 status)
{
	char ch = inb (CCU98_RX);
	char flag;

#if 0
	pr_debug ("ccu98: received 0x%02x\n", (unsigned char) ch);
#endif

	if (PORT->gs.tty->flip.count >= TTY_FLIPBUF_SIZE)
		return;
	PORT->gs.tty->flip.char_buf_ptr[0] = ch;
	flag = 0;
	if (status & (USART_LSR_SYNDET | USART_LSR_PE
		      | USART_LSR_FE | USART_LSR_OE)) {
		if (status & USART_LSR_SYNDET) {
			if (PORT->broken)
				return;

			/* status &= ~(USART_LSR_FE | USART_LSR_PE); */
			PORT->icount.brk++;
			flag = TTY_BREAK;
			if (PORT->gs.flags & ASYNC_SAK)
				do_SAK (PORT->gs.tty);
			PORT->broken = 1;
		}
		else {
			PORT->broken = 0;
			if (status & USART_LSR_PE) {
				PORT->icount.parity++;
				flag = TTY_PARITY;
			}
			else if (status & USART_LSR_FE) {
				PORT->icount.frame++;
				flag = TTY_FRAME;
			}
		}
		/* Overrun may occure with other errors */
		if (status & USART_LSR_OE) {
			PORT->icount.overrun++;
			if (PORT->gs.tty->flip.count < TTY_FLIPBUF_SIZE) {
				PORT->gs.tty->flip.count++;
				PORT->gs.tty->flip.char_buf_ptr++;
				*PORT->gs.tty->flip.flag_buf_ptr++ = flag;
				flag = TTY_OVERRUN;
			}
		}
		if (status & (USART_LSR_FE | USART_LSR_PE | USART_LSR_OE))
			/* Reset error condition.  */
			OUT_CMR (PORT, PORT->oldcmr | USART_CMR_ER);
	}
	PORT->icount.rx++;
	PORT->gs.tty->flip.count++;
	PORT->gs.tty->flip.char_buf_ptr++;
	*PORT->gs.tty->flip.flag_buf_ptr++ = flag;
	tty_flip_buffer_push (PORT->gs.tty);
}

static inline void ccu98_check_modem_status (void *_port)
{
	u8 msr = inb (CCU98_MSR);
	u8 delta = PORT->oldmsr ^ msr;

	PORT->oldmsr = msr;
	if (delta & (CCU98_MSR_CI | CCU98_MSR_CS | CCU98_MSR_CD)) {
		if (delta & CCU98_MSR_CI)
			PORT->icount.rng++;
		if (delta & CCU98_MSR_CD)
			PORT->icount.dcd++;
		if (delta & CCU98_MSR_CS)
			PORT->icount.cts++;
		wake_up_interruptible (&PORT->delta_msr_wait);
	}
	if ((PORT->gs.flags & ASYNC_CHECK_CD) && (delta & CCU98_MSR_CD)) {
		if (msr & CCU98_MSR_CD)
			wake_up_interruptible (&PORT->gs.open_wait);
		else if (!(PORT->gs.flags & (ASYNC_CALLOUT_ACTIVE
					     | ASYNC_CALLOUT_NOHUP)))
			if (PORT->gs.tty)
				tty_hangup (PORT->gs.tty);
	}
	if (PORT->gs.flags & ASYNC_CTS_FLOW) {
		if (PORT->gs.tty->hw_stopped) {
			if (msr & CCU98_MSR_CS) {
				PORT->gs.tty->hw_stopped = 0;
				outb (CCU98_IERC_TXRE | 1, CCU98_IERC);
				ccu98_sched_event (PORT,
						   RS_EVENT_WRITE_WAKEUP);
			}
		}
		else if (!(msr & CCU98_MSR_CS)) {
			PORT->gs.tty->hw_stopped = 1;
			outb (CCU98_IERC_TXRE, CCU98_IERC);
		}
	}
}

static inline int ccu98_transmit_char (void *_port, u8 status)
{
	if (PORT->xchar) {
		pr_debug ("ccu98: transmitting urgent character 0x%02x\n",
			  PORT->xchar);
		outb (PORT->xchar, CCU98_TX);
		PORT->xchar = 0;
		PORT->icount.tx++;
	}
	else if (PORT->gs.xmit_cnt) {
		/* Send one character */
#if 0
		pr_debug ("ccu98: transmitting 0x%02x\n",
			  PORT->gs.xmit_buf[PORT->gs.xmit_tail]);
#endif
		outb (PORT->gs.xmit_buf[PORT->gs.xmit_tail], CCU98_TX);
		PORT->gs.xmit_tail++;
		PORT->gs.xmit_tail &= SERIAL_XMIT_SIZE - 1;
		PORT->icount.tx++;

		if (--PORT->gs.xmit_cnt <= PORT->gs.wakeup_chars)
			ccu98_sched_event (PORT, RS_EVENT_WRITE_WAKEUP);
	}
	return (!PORT->gs.xmit_cnt
		|| PORT->gs.tty->stopped || PORT->gs.tty->hw_stopped);
}

static void ccu98_interrupt (int irq, void *_port, struct pt_regs *regs)
{
	int pass_counter = CCU98_ISR_PASS_LIMIT;
	u8 ier = inb (CCU98_IER);
	u8 status;
	u8 mask = USART_LSR_RXRDY | USART_LSR_TXRDY;

#ifdef DEBUG
	u8 oldier = ier;
#endif
	outb (ier & ~(CCU98_IER_TXRE|CCU98_IER_TXEE|CCU98_IER_RXRE),
	      CCU98_IER);

	while ((status = inb (CCU98_LSR)) & mask) {
		if (status & USART_LSR_RXRDY)
			ccu98_receive_char (PORT, status);
		ccu98_check_modem_status (PORT);
		if (status & USART_LSR_TXRDY)
			if (ccu98_transmit_char (PORT, status)) {
				ier &= ~(CCU98_IER_TXRE | CCU98_IER_TXEE);
				PORT->gs.flags &= ~GS_TX_INTEN;
				mask &= ~USART_LSR_TXRDY;
			}

		if (!--pass_counter)
			break;
	}
	outb (ier, CCU98_IER);
#ifdef DEBUG
	if (ier != oldier)
		pr_debug ("ccu98: IER changed from 0x%02x to 0x%02x\n",
			  oldier, ier);
#endif
}

static int ccu98_open (struct tty_struct *tty, struct file *file)
{
	int error;
	int line;
	struct ccu98_port *port;

	/* Sanity check */
	if (MAJOR (tty->device) != tty->driver.major
	    || (line = MINOR (tty->device) - tty->driver.minor_start) < 0
	    || line >= tty->driver.num)
		return -ENODEV;
	tty->driver_data = port = &ccu98_port[line];

	port->gs.tty = tty;
	port->gs.event = 0;

	if ((error = gs_init_port (&port->gs)))
		return error;

	if (!port->gs.count
	    && (error = request_irq (port->irq, ccu98_interrupt, SA_INTERRUPT,
				     "serial (ccu98)", port)))
		return error;

	port->gs.count++;
	MOD_INC_USE_COUNT;
	port->gs.flags |= GS_ACTIVE;

	port->oldcmr |= (USART_CMR_RTS | USART_CMR_RXE
			 | USART_CMR_DTR | USART_CMR_TXE);
	OUT_CMR (port, port->oldcmr);

	error = gs_block_til_ready (&port->gs, file);
	if (error) {
		MOD_DEC_USE_COUNT;
		port->gs.count--;
		free_irq (port->irq, port);
		return error;
	}
	if (port->gs.count == 1
	    && (port->gs.flags & ASYNC_SPLIT_TERMIOS)) {
		*tty->termios = (tty->driver.subtype == SERIAL_TYPE_NORMAL
				 ? port->gs.normal_termios
				 : port->gs.callout_termios);
		ccu98_set_real_termios (port);
	}
	ccu98_enable_rx_interrupts (port);

	port->gs.session = current->session;
	port->gs.pgrp = current->pgrp;

	return 0;
}

static int tiocgicount (struct ccu98_port *port,
			struct serial_icounter_struct *result)
{
	unsigned long flags;
	struct serial_icounter_struct icount;

	spin_lock_irqsave (&port->lock, flags);
	icount.cts = port->icount.cts;
	icount.dsr = port->icount.dsr;
	icount.rng = port->icount.rng;
	icount.dcd = port->icount.dcd;
	icount.rx = port->icount.rx;
	icount.tx = port->icount.tx;
	icount.frame = port->icount.frame;
	icount.overrun = port->icount.overrun;
	icount.parity = port->icount.parity;
	icount.brk = port->icount.brk;
	icount.buf_overrun = port->icount.buf_overrun;
	spin_unlock_irqrestore (&port->lock, flags);

	if (copy_to_user (result, &icount, sizeof (icount)))
		return -EFAULT;
	return 0;
}

static int ccu98_ioctl (struct tty_struct *tty, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	struct ccu98_port *port = tty->driver_data;
	unsigned long flags;
	unsigned int value;
	u8 tmp;
	int error;

	switch (cmd) {
	case TIOCMGET:
		value = 0;
		spin_lock_irqsave (&port->lock, flags);
		if (port->oldcmr & USART_CMR_RTS)
			value |= TIOCM_RTS;
		if (port->oldcmr & USART_CMR_DTR)
			value |= TIOCM_DTR;
		tmp = IN_MSR (port);
		if (tmp & CCU98_MSR_CD)
			value |= TIOCM_CAR;
		if (tmp & CCU98_MSR_CI)
			value |= TIOCM_RNG;
		if (tmp & CCU98_MSR_CS)
			value |= TIOCM_CTS;
		if (IN_LSR (port) & USART_LSR_DSR)
			value |= TIOCM_DSR;
		spin_unlock_irqrestore (&port->lock, flags);
		return put_user (value, (int *) arg);

	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
		if ((error = get_user (value, (int *) arg)))
			return error;
		spin_lock_irqsave (&port->lock, flags);
		switch (cmd) {
		case TIOCMSET:
			port->oldcmr &= ~(USART_CMR_DTR | USART_CMR_RTS);
		case TIOCMBIS:
			if (value & TIOCM_RTS)
				port->oldcmr |= USART_CMR_RTS;
			if (value & TIOCM_DTR)
				port->oldcmr |= USART_CMR_DTR;
			break;
		default: /* TIOCMBIC */
			if (value & TIOCM_RTS)
				port->oldcmr &= ~USART_CMR_RTS;
			if (value & TIOCM_DTR)
				port->oldcmr &= ~USART_CMR_DTR;
		}
		OUT_CMR (port, port->oldcmr);
		spin_unlock_irqrestore (&port->lock, flags);
		return 0;

	case TIOCGSERIAL:
		/* !!! gs_getseiral should handle EFAULT etc. */
		gs_getserial (&port->gs, (struct serial_struct *) arg);
		break;
	case TIOCSSERIAL:
		return gs_setserial (&port->gs, (struct serial_struct *) arg);

	case TIOCGICOUNT:
		return tiocgicount (port, (void *) arg);

	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

#define	_port	(tty->driver_data) /* black magic! */

static void break_ctl (struct tty_struct *tty, int break_state)
{
	unsigned long flags;

	spin_lock_irqsave (&PORT->lock, flags);
	PORT->oldcmr &= ~USART_CMR_SBRK;
	if (break_state == -1)
		PORT->oldcmr |= USART_CMR_SBRK;
	OUT_CMR (PORT, PORT->oldcmr);
	spin_unlock_irqrestore (&PORT->lock, flags);
}

static inline void send_xchar (struct tty_struct *tty, char ch)
{
	PORT->xchar = ch;
	if (ch)
		/* Make sure transmit interrupts are on */
		ccu98_enable_tx_interrupts (PORT);
}

static void ccu98_throttle (struct tty_struct *tty)
{
	if (I_IXOFF (tty))
		send_xchar (tty, STOP_CHAR (tty));
	if (C_CRTSCTS (tty)) {
		PORT->oldcmr &= ~USART_CMR_RTS;
		OUT_CMR (PORT, PORT->oldcmr);
	}
}

static void ccu98_unthrottle (struct tty_struct *tty)
{
	if (I_IXOFF (tty)) {
		if (PORT->xchar)
			PORT->xchar = 0;
		else
			send_xchar (tty, START_CHAR (tty));
	}
	if (C_CRTSCTS (tty)) {
		PORT->oldcmr |= USART_CMR_RTS;
		OUT_CMR (PORT, PORT->oldcmr);
	}
}

static int read_proc (char *page, char **start, off_t off, int count,
		      int *eof, void *data)
{
	int i;
	int len;
	off_t begin = 0;

	len = sprintf (page, "serinfo:1.0 driver:ccu98 revision:unknown\n");
	for (i = 0; i < NR_CCU98_PORTS && len < 4000; i++) {
		struct ccu98_port *port = &ccu98_port[i];
		len += sprintf (page + len, "%d: uart:%s port:%X irq:%d",
				i, "8251", CCU98_TX, port->irq);
		if (port->gs.magic == CCU98_MAGIC) {
			int divisor;
			char delim;
			u8 tmplsr, tmpmsr;

			if (port->gs.baud
			    && (divisor = port->gs.baud_base / port->gs.baud))
				len += sprintf (page + len, " baud:%d",
						port->gs.baud_base / divisor);
			len += sprintf (page + len, " tx:%d rx:%d",
					port->icount.tx, port->icount.rx);
			if (port->icount.frame)
				len += sprintf (page + len, " fe:%d",
						port->icount.frame);
			if (port->icount.parity)
				len += sprintf (page + len, " pe:%d",
						port->icount.parity);
			if (port->icount.brk)
				len += sprintf (page + len, " brk:%d",
						port->icount.brk);
			if (port->icount.overrun)
				len += sprintf (page + len, " oe:%d",
						port->icount.overrun);
			delim = ' ';

#define ADDFLAG(STR)	(page[len++] = delim,				\
			 memcpy (page + len, STR, sizeof (STR) - 1),	\
			 len += sizeof (STR) - 1,			\
			 delim = '|')

			tmplsr = IN_LSR (port);
			tmpmsr = IN_MSR (port);
			if (port->oldcmr & USART_CMR_RTS)
				ADDFLAG ("RTS");
			if (!(tmpmsr & CCU98_MSR_CS))
				ADDFLAG ("CTS");
			if (port->oldcmr & USART_CMR_DTR)
				ADDFLAG ("DTR");
			if (tmplsr & USART_LSR_DSR)
				ADDFLAG ("DSR");
			if (!(tmpmsr & CCU98_MSR_CD))
				ADDFLAG ("CD");
			if (!(tmpmsr & CCU98_MSR_CI))
				ADDFLAG ("RI");

#ifdef DEBUG
			len += sprintf (page + len, " txbuf:%d",
					port->gs.xmit_cnt);
			if (i == 0) {
				u8 tmpier = inb (CCU98_IER);
				memcpy (page + len, " ier", 4);
				len += 4;
				delim = ':';
				if (tmpier & CCU98_IER_TXRE)
					ADDFLAG ("TXRE");
				if (tmpier & CCU98_IER_TXEE)
					ADDFLAG ("TXEE");
				if (tmpier & CCU98_IER_RXRE)
					ADDFLAG ("RXRE");
				if (delim == ':')
					ADDFLAG ("-");
			}
#endif
		}
		page[len++] = '\n';

		if (len + begin > off + count)
			goto done;
		if (len + begin < off) {
			begin += len;
			len = 0;
		}
	}
	*eof = 1;
 done:
	if (off >= len + begin)
		return 0;
	*start = page + (off - begin);
	return count < begin + len - off ? count : begin + len - off;
}

#undef _port			/* end black magic */

static int ccu98_refcount;

static struct tty_driver ccu98_driver = {
	magic:		TTY_DRIVER_MAGIC,
	driver_name:	"ccu98",
	name:		"ttyS",
	major:		TTY_MAJOR,
	minor_start:	64,
	num:		1,
	type:		TTY_DRIVER_TYPE_SERIAL,
	subtype:	SERIAL_TYPE_NORMAL,

	flags:		TTY_DRIVER_REAL_RAW,
	refcount:	&ccu98_refcount,
	table:		ccu98_table,
	termios:	ccu98_termios,
	termios_locked:	ccu98_termios_locked,

	open:		ccu98_open,
	close:		gs_close,
	write:		gs_write,
	put_char:	gs_put_char,
	flush_chars:	gs_flush_chars,
	write_room:	gs_write_room,
	chars_in_buffer: gs_chars_in_buffer,
	flush_buffer:	gs_flush_buffer,
	ioctl:		ccu98_ioctl,
	throttle:	ccu98_throttle,
	unthrottle:	ccu98_unthrottle,
	set_termios:	gs_set_termios,
	stop:		gs_stop,
	start:		gs_start,
	hangup:		gs_hangup,
	break_ctl:	break_ctl,
	send_xchar:	send_xchar,
	read_proc:	read_proc,
};

static struct tty_driver ccu98_callout_driver;

static u8 have_extreg;

static int __init ccu98_init (void)
{
	int i;
	int error;
	pr_debug ("ccu98: inirializing\n");

	ccu98_driver.init_termios = tty_std_termios;
	ccu98_driver.init_termios.c_cflag
		= B9600 | CS8 | CREAD | HUPCL | CLOCAL | CRTSCTS,

	ccu98_callout_driver = ccu98_driver;
	ccu98_callout_driver.name = "cua";
	ccu98_callout_driver.major = TTYAUX_MAJOR;
	ccu98_callout_driver.subtype = SERIAL_TYPE_CALLOUT;
	ccu98_callout_driver.read_proc = NULL;
	ccu98_callout_driver.proc_entry = NULL;

	for (i = 0; i < NR_CCU98_PORTS; i++) {
		init_waitqueue_head (&ccu98_port[i].gs.open_wait);
		init_waitqueue_head (&ccu98_port[i].gs.close_wait);
		init_waitqueue_head (&ccu98_port[i].delta_msr_wait);
		ccu98_port[i].gs.close_delay = HZ / 2;
		ccu98_port[i].gs.closing_wait = 30 * HZ;
		ccu98_port[i].gs.rd = &ccu98_real_driver;
		ccu98_port[i].gs.callout_termios
			= ccu98_callout_driver.init_termios;
		ccu98_port[i].gs.normal_termios = ccu98_driver.init_termios;
		ccu98_port[i].lock = SPIN_LOCK_UNLOCKED;
		ccu98_port[i].tqueue.routine = gs_do_softint;
		ccu98_port[i].tqueue.data = &ccu98_port[i];
		ccu98_port[i].irq = irq[i];
	}

	if (!irq[0])
		;
	else if (!request_region (CCU98_TX, -3, "serial (ccu98)"))
		printk (KERN_ERR "ccu98: I/O port %#x already in use\n",
			CCU98_TX);
	else if (!check_region (CCU98_EXTCTL, 1)
		 && inb (CCU98_EXTCTL) != 0xFF
		 && (have_extreg = 1,
		     !request_region (CCU98_EXTCTL, 1, "serial (ccu98)"))) {
		printk (KERN_ERR "ccu98: I/O port %#x already in use\n",
			CCU98_EXTCTL);
		release_region (CCU98_TX, -3);
	}
	else {
		ccu98_port[0].gs.magic = CCU98_MAGIC;
		ccu98_port[0].gs.baud_base
			= PC9800_8MHz_P () ? 1996800 / 16 : 2457600 / 16;
		if (have_extreg) {
			u8 tmp = inb_p (CCU98_EXTCTL) ^ CCU98_EXTCTL_NOQUAD;
			outb_p (tmp, CCU98_EXTCTL);
			if ((inb_p (CCU98_EXTCTL) ^ tmp)
			    & (CCU98_EXTCTL_NOQUAD | CCU98_EXTCTL_DISABLE))
				have_extreg = 0;
			else {
				if (!(tmp & CCU98_EXTCTL_NOQUAD)
				    || (tmp & CCU98_EXTCTL_DISABLE))
					outb ((tmp | CCU98_EXTCTL_NOQUAD)
					      & ~CCU98_EXTCTL_DISABLE,
					      CCU98_EXTCTL);
				ccu98_port[0].gs.baud_base *= 4;
			}
		}
	}

	if ((error = tty_register_driver (&ccu98_driver))) {
		printk (KERN_ERR "ccu98: could not register tty driver\n");
		goto out;
	}
	if ((error = tty_register_driver (&ccu98_callout_driver))) {
		printk (KERN_ERR "ccu98: could not register callout driver\n");
		tty_unregister_driver (&ccu98_driver);
		goto out;
	}

	return 0;

out:
	if (ccu98_port[0].gs.magic) {
		release_region (CCU98_TX, -3);
		if (have_extreg)
			release_region (CCU98_EXTCTL, 1);
	}
	return error;
}

static void __exit ccu98_exit (void)
{
	tty_unregister_driver (&ccu98_callout_driver);
	tty_unregister_driver (&ccu98_driver);
	if (ccu98_port[0].gs.magic) {
		release_region (CCU98_TX, -3);
		if (have_extreg)
			release_region (CCU98_EXTCTL, 1);
	}
	pr_debug ("ccu98: unloaded\n");
}

module_init (ccu98_init);
module_exit (ccu98_exit);

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
