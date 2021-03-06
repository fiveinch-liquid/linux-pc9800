/*
 * Interrupt handling assembler and defines for Linux/CRIS
 *
 * Copyright (c) 2000 Axis Communications AB
 *
 * Authors:   Bjorn Wesen (bjornw@axis.com)
 *
 * $Id: irq.h,v 1.11 2001/06/01 14:57:17 starvik Exp $
 */

#ifndef _ASM_IRQ_H
#define _ASM_IRQ_H

/*
 *	linux/include/asm-cris/irq.h
 */

#include <linux/linkage.h>
#include <asm/segment.h>

#include <asm/sv_addr_ag.h>

#define NR_IRQS 32
#define SOME_IRQ_NBR        IO_BITNR(R_VECT_MASK_RD, some)   /* 0 ? */
#define NMI_IRQ_NBR         IO_BITNR(R_VECT_MASK_RD, nmi)    /* 1 */
#define TIMER0_IRQ_NBR      IO_BITNR(R_VECT_MASK_RD, timer0) /* 2 */
#define TIMER1_IRQ_NBR      IO_BITNR(R_VECT_MASK_RD, timer1) /* 3 */
/* mio, ata, par0, scsi0 on 4 */
/* par1, scsi1 on 5 */
#define NETWORK_STATUS_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, network) /* 6 */

#define SERIAL_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, network) /* 8 */
#define PA_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, pa) /* 11 */
/* extdma0 and extdma1 is at irq 12 and 13 and/or same as dma5 and dma6 ? */
#define EXTDMA0_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, ext_dma0)
#define EXTDMA1_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, ext_dma1)

/* dma0-9 is irq 16..25 */
/* 16,17: network */
#define DMA0_TX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma0)
#define DMA1_RX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma1)
#define NETWORK_DMA_TX_IRQ_NBR DMA0_TX_IRQ_NBR
#define NETWORK_DMA_RX_IRQ_NBR DMA1_RX_IRQ_NBR

/* 18,19: dma2 and dma3 shared by par0, scsi0, ser2 and ata */
#define DMA2_TX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma2)
#define DMA3_RX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma3)
#define SER2_DMA_TX_IRQ_NBR DMA2_TX_IRQ_NBR
#define SER2_DMA_RX_IRQ_NBR DMA3_RX_IRQ_NBR

/* 20,21: dma4 and dma5 shared by par1, scsi1, ser3 and extdma0 */
#define DMA4_TX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma4)
#define DMA5_RX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma5)
#define SER3_DMA_TX_IRQ_NBR DMA4_TX_IRQ_NBR
#define SER3_DMA_RX_IRQ_NBR DMA5_RX_IRQ_NBR

/* 22,23: dma6 and dma7 shared by ser0, extdma1 and mem2mem */
#define DMA6_TX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma6)
#define DMA7_RX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma7)
#define SER0_DMA_TX_IRQ_NBR DMA6_TX_IRQ_NBR
#define SER0_DMA_RX_IRQ_NBR DMA7_RX_IRQ_NBR
#define MEM2MEM_DMA_TX_IRQ_NBR DMA6_TX_IRQ_NBR
#define MEM2MEM_DMA_RX_IRQ_NBR DMA7_RX_IRQ_NBR

/* 24,25: dma8 and dma9 shared by ser1 and usb */
#define DMA8_TX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma8)
#define DMA9_RX_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, dma9)
#define SER1_DMA_TX_IRQ_NBR DMA8_TX_IRQ_NBR
#define SER1_DMA_RX_IRQ_NBR DMA9_RX_IRQ_NBR
#define USB_DMA_TX_IRQ_NBR DMA8_TX_IRQ_NBR
#define USB_DMA_RX_IRQ_NBR DMA9_RX_IRQ_NBR

/* usb: controller at irq 31 + uses DMA8 and DMA9 */
#define USB_HC_IRQ_NBR IO_BITNR(R_VECT_MASK_RD, usb)





extern void disable_irq(unsigned int);
extern void enable_irq(unsigned int);

#define disable_irq_nosync      disable_irq
#define enable_irq_nosync       enable_irq

/* our fine, global, etrax irq vector! the pointer lives in the head.S file. */

typedef void (*irqvectptr)(void);

struct etrax_interrupt_vector {
	irqvectptr v[256];
};

extern struct etrax_interrupt_vector *etrax_irv;
void set_int_vector(int n, irqvectptr addr, irqvectptr saddr);
void set_break_vector(int n, irqvectptr addr);

#define __STR(x) #x
#define STR(x) __STR(x)
 
/* SAVE_ALL saves registers so they match pt_regs */

#define SAVE_ALL \
  "move irp,[sp=sp-16]\n\t" /* push instruction pointer and fake SBFS struct */ \
  "push srp\n\t"       /* push subroutine return pointer */ \
  "push dccr\n\t"      /* push condition codes */ \
  "push mof\n\t"       /* push multiply overflow reg */ \
  "di\n\t"             /* need to disable irq's at this point */\
  "subq 14*4,sp\n\t"   /* make room for r0-r13 */ \
  "movem r13,[sp]\n\t" /* push the r0-r13 registers */ \
  "push r10\n\t"       /* push orig_r10 */ \
  "clear.d [sp=sp-4]\n\t"  /* frametype - this is a normal stackframe */

  /* BLOCK_IRQ and UNBLOCK_IRQ do the same as mask_irq and unmask_irq in irq.c */

#define BLOCK_IRQ(mask,nr) \
  "move.d " #mask ",r0\n\t" \
  "move.d r0,[0xb00000d8]\n\t" 
  
#define UNBLOCK_IRQ(mask) \
  "move.d " #mask ",r0\n\t" \
  "move.d r0,[0xb00000dc]\n\t" 

#define IRQ_NAME2(nr) nr##_interrupt(void)
#define IRQ_NAME(nr) IRQ_NAME2(IRQ##nr)
#define sIRQ_NAME(nr) IRQ_NAME2(sIRQ##nr)
#define BAD_IRQ_NAME(nr) IRQ_NAME2(bad_IRQ##nr)

  /* the asm IRQ handler makes sure the causing IRQ is blocked, then it calls
   * do_IRQ (with irq disabled still). after that it unblocks and jumps to
   * ret_from_intr (entry.S)
   */

#define BUILD_IRQ(nr,mask) \
void IRQ_NAME(nr); \
void sIRQ_NAME(nr); \
void BAD_IRQ_NAME(nr); \
__asm__ ( \
          ".text\n\t" \
          "_IRQ" #nr "_interrupt:\n\t" \
	  SAVE_ALL \
	  "_sIRQ" #nr "_interrupt:\n\t" /* shortcut for the multiple irq handler */ \
	  BLOCK_IRQ(mask,nr) /* this must be done to prevent irq loops when we ei later */ \
	  "moveq "#nr",r10\n\t" \
	  "move.d sp,r11\n\t" \
	  "jsr _do_IRQ\n\t" /* irq.c, r10 and r11 are arguments */ \
	  UNBLOCK_IRQ(mask) \
	  "moveq 0,r9\n\t" /* make ret_from_intr realise we came from an irq */ \
	  "jump _ret_from_intr\n\t" \
          "_bad_IRQ" #nr "_interrupt:\n\t" \
	  "push r0\n\t" \
	  BLOCK_IRQ(mask,nr) \
	  "pop r0\n\t" \
          "reti\n\t" \
          "nop\n");


#endif  /* _ASM_IRQ_H */


