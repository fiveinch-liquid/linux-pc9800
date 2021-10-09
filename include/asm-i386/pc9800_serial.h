/*
 *  Macro & inline functions for PC-9800 series serial interface.
 *  Copyright (C) 1997-2001  Linux/98 project
 *			     at Kyoto University Microcomputer Club.
 */

#ifndef _ASM_I386_PC9800_SERIAL_H
#define _ASM_I386_PC9800_SERIAL_H

/* `u' is Greek \mu. */

#define uPD8251_DATA	(0x0030) /* R/W */
#define uPD8251_STATUS	(0x0032) /* R/- */
#define  uPD8251_STATUS_TXRDY	(1 << 0) /* 1: Tx Ready */
#define  uPD8251_STATUS_RXRDY	(1 << 1) /* 1: Rx Ready */
#define  uPD8251_STATUS_TXE	(1 << 2) /* 1: Tx buffer Empty */
#define  uPD8251_STATUS_PE	(1 << 3) /* 1: Parity Error */
#define  uPD8251_STATUS_OE	(1 << 4) /* 1: Overrun Error */
#define  uPD8251_STATUS_FE	(1 << 5) /* 1: Framing Error */
#define  uPD8251_STATUS_SYNDET	(1 << 6)
#define  uPD8251_STATUS_DSR	(1 << 7) /* 1: Data Set Ready */
#define uPD8251_CMD	(0x0032) /* -/W */
#define  uPD8251_CMD_TXE	(1 << 0) /* 1: Tx Enable */
#define  uPD8251_CMD_DTR	(1 << 1) /* 1: DTR <= 0 */
#define  uPD8251_CMD_RXE	(1 << 2) /* 1: Rx Enable */
#define  uPD8251_CMD_SBRK	(1 << 3) /* 1: TxD <= 0 (send break) */
#define  uPD8251_CMD_ER		(1 << 4) /* 1: Error Reset */
#define  uPD8251_CMD_RTS	(1 << 5) /* 1: RTS <= 0 */
#define  uPD8251_CMD_IR		(1 << 6) /* 1: Internal Reset */
#define  uPD8251_CMD_EH		(1 << 7) /* 1: Enter HUNT mode */
#define uPD8251_MODE	(0x0032) /* -/W after reset */
#define  uPD8251_MODE_B_SYNC	(0 << 0)
#define  uPD8251_MODE_B_X1	(1 << 0)
#define  uPD8251_MODE_B_X16	(2 << 0)
#define  uPD8251_MODE_B_X64	(3 << 0)
#define  uPD8251_MODE_L_5	(0 << 2)
#define  uPD8251_MODE_L_6	(1 << 2)
#define  uPD8251_MODE_L_7	(2 << 2)
#define  uPD8251_MODE_L_8	(3 << 2)
#define  uPD8251_MODE_PEN	(1 << 4)
#define  uPD8251_MODE_EP	(1 << 5)
#define  uPD8251_MODE_S_1	(1 << 6)
#define  uPD8251_MODE_S_1_5	(2 << 6)
#define  uPD8251_MODE_S_2	(3 << 6)
#define  uPD8251_MODE_ESD	(1 << 6) /* synchronous mode only */
#define  uPD8251_MODE_SCS	(1 << 7) /* synchronous mode only */

/* IER is really not on 8251; it is on 8255 PIO for PC-9800.  */
#define uPD8251_MSR	(0x0033) /* R/- */
#define  uPD8251_MSR_nCD	(1 << 5) /* 0: Carrier Detect */
#define  uPD8251_MSR_nCS	(1 << 6)
#define  uPD8251_MSR_nCI	(1 << 7) /* 0: Call In */
#define uPD8251_IER	(0x0035) /* R/W */
#define  uPD8251_IER_RXRE	(1 << 0) /* 1: Enable interrupt for RxRDY */
#define  uPD8251_IER_TXEE	(1 << 1) /* 1: Enable interrupt for TxEMPTY */
#define  uPD8251_IER_TXRE	(1 << 2) /* 1: Enable interrupt for TxRDY */

static inline int
pc9800_com1_fifo_p (void)
{
	u8 tmp0 = inb_p (IIR_8251F);
	u8 tmp1 = inb_p (IIR_8251F);

	/* On FIFO-capble machines, bit6 toggles for each read
	   while bit5 remains zero.  */
	if (!((tmp0 ^ tmp1) & (1 << 6)) || ((tmp0 | tmp1) & (1 << 5)))
		return 0;
	tmp1 = inb_p (IIR_8251F);
	if ((tmp0 ^ tmp1) & ((1 << 6) | (1 << 5)))
		return 0;
	return 1;
}

static inline void
upd8251_outb (u8 data, u16 port, int consecutive)
{
	outb (data, port);
	if (consecutive) {
		/* 5 dummy writes to 0x5F are required between
		   consecutive writing to 8251.
		   Output value to 0x5F can be any value, but
		   specifying DATA can eliminate unnecessary load to %al. */
		outb (data, 0x5F);
		outb (data, 0x5F);
		outb (data, 0x5F);
		outb (data, 0x5F);
		outb (data, 0x5F);
	}
}

static inline void
upd8251_mode_set (u8 mode)
{
	/* 3 dummy commands before resetting */
	upd8251_outb (0, uPD8251_CMD, 1);
	upd8251_outb (0, uPD8251_CMD, 1);
	upd8251_outb (0, uPD8251_CMD, 1);
	upd8251_outb (uPD8251_CMD_IR, uPD8251_CMD, 1); /* reset! */
	upd8251_outb (mode, uPD8251_MODE, 0);
	mdelay (1);
}

static inline void
upd8251_autoconfig (struct serial_state *state, struct async_struct *info)
{
	state->type = PORT_8251;
	state->io_type = SERIAL_IO_8251;
	if (!nofifo
	    && !check_region (RX_8251F, -9)
	    && pc9800_com1_fifo_p ()) {
		state->type = PORT_8251_FIFO;
		state->io_type = SERIAL_IO_8251_FIFO;
		if (!novfast && PC9821_COM1_VFAST_P ()) {
			if (check_region (VFAST_8251F, 1))
				printk (KERN_WARNING "\
ttyS0: V-Fast mode register already in use, go anyway\n");
			state->type = PORT_8251_VFAST;
		}
		if (check_region (PC9800_CCU_EXTREG, 1))
			printk (KERN_WARNING "\
ttyS0: Extended RS-232C control register already in use, go anyway\n");
	}
	else if (!check_region (PC9800_CCU_EXTREG, 1)
		 && inb (PC9800_CCU_EXTREG) != 0xFF)
		state->type = PORT_8251_19K;

	info->magic = SERIAL_MAGIC;
	info->state = state;
	info->port = state->port;
	info->flags = state->flags;
	info->io_type = state->io_type;

	upd8251_mode_set (uPD8251_MODE_S_2 | uPD8251_MODE_EP | uPD8251_MODE_PEN
			  | uPD8251_MODE_L_5 | uPD8251_MODE_B_X16);
	outb (uPD8251_CMD_TXE, uPD8251_CMD);
	mdelay (1);
	if (!(inb (uPD8251_STATUS) & uPD8251_STATUS_TXRDY))
		return;

	/* Disable all interrupt from 8251.  */
	outb (inb (uPD8251_IER)
	      & ~(uPD8251_IER_RXRE|uPD8251_IER_TXEE|uPD8251_IER_TXRE),
	      uPD8251_IER);

	upd8251_mode_set (uPD8251_MODE_S_1|uPD8251_MODE_EP
			  |uPD8251_MODE_L_8|uPD8251_MODE_B_X16);

	state->xmit_fifo_size = uart_config[state->type].dfl_xmit_fifo_size;

	switch (state->type) {
	case PORT_8251_VFAST:
		request_region (uPD8251_VFAST, 1, "serial(8251/V-Fast)");
	case PORT_8251_FIFO:
		request_region (uPD8251F_DATA, -9, "serial(8251/FIFO)");
	case PORT_8251_19K:
		request_region (uPD8251_EXT, 1, "serial(8251/extended)");
	default:
		request_region (uPD8251_DATA, -3, "serial(8251)");
	}

	printk (KERN_DEBUG "ttyS0: autoconfig8251 finished\n");
}


static void
ccu98_update_IER (struct async_struct *info)
{
	u8 tmp, tmp1;

	switch (info->io_type) {
	case SERIAL_IO_FIFOCCU98:
		tmp = inb (CCU98_FIFOCONTROL);
		if ((info->IER & UART_IER_RLSI)
		    && !(tmp & CCU98_FIFOCONTROL_RLI))
			outb (tmp | CCU98_FIFOCONTROL_RLI, CCU98_FIFOCONTROL);
		else if (!(info->IER & UART_IER_RLSI)
			 && (tmp & CCU98_FIFOCONTROL_RLI))
			outb (tmp & ~CCU98_FIFOCONTROL_RLI, CCU98_FIFOCONTROL);
	default:
		tmp = tmp1 = inb (CCU98_IER);
		tmp &= ~(CCU98_IER_TXRE | CCU98_IER_TXEE | CCU98_IER_RXRE);
		if (info->IER & UART_THRI)
			tmp |= CCU98_IER_TXEE;
		if (info->IER & UART_RDI)
			tmp |= CCU98_IER_RXRE;
		if (tmp != tmp1)
			outb (tmp, CCU98_IER);
	}
}

#endif
