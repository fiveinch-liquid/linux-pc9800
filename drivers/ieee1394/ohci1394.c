/*
 * ohci1394.c - driver for OHCI 1394 boards
 * Copyright (C)1999,2000 Sebastien Rougeaux <sebastien.rougeaux@anu.edu.au>
 *                        Gord Peters <GordPeters@smarttech.com>
 *              2001      Ben Collins <bcollins@debian.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/*
 * Things known to be working:
 * . Async Request Transmit
 * . Async Response Receive
 * . Async Request Receive
 * . Async Response Transmit
 * . Iso Receive
 * . DMA mmap for iso receive
 * . Config ROM generation
 *
 * Things implemented, but still in test phase:
 * . Iso Transmit
 * 
 * Things not implemented:
 * . Async Stream Packets
 * . DMA error recovery
 *
 * Things to be fixed:
 * . Latency problems on UltraSPARC
 *
 * Known bugs:
 * . SelfID are sometimes not received properly 
 *   if card is initialized with no other nodes 
 *   on the bus
 * . Apple PowerBook detected but not working yet
 */

/* 
 * Acknowledgments:
 *
 * Adam J Richter <adam@yggdrasil.com>
 *  . Use of pci_class to find device
 *
 * Andreas Tobler <toa@pop.agri.ch>
 *  . Updated proc_fs calls
 *
 * Emilie Chung	<emilie.chung@axis.com>
 *  . Tip on Async Request Filter
 *
 * Pascal Drolet <pascal.drolet@informission.ca>
 *  . Various tips for optimization and functionnalities
 *
 * Robert Ficklin <rficklin@westengineering.com>
 *  . Loop in irq_handler
 *
 * James Goodwin <jamesg@Filanet.com>
 *  . Various tips on initialization, self-id reception, etc.
 *
 * Albrecht Dress <ad@mpifr-bonn.mpg.de>
 *  . Apple PowerBook detection
 *
 * Daniel Kobras <daniel.kobras@student.uni-tuebingen.de>
 *  . Reset the board properly before leaving + misc cleanups
 *
 * Leon van Stuivenberg <leonvs@iae.nl>
 *  . Bug fixes
 *
 * Ben Collins <bcollins@debian.org>
 *  . Working big-endian support
 *  . Updated to 2.4.x module scheme (PCI aswell)
 *  . Removed procfs support since it trashes random mem
 *  . Config ROM generation
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <asm/byteorder.h>
#include <asm/atomic.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/tqueue.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <asm/pgtable.h>
#include <asm/page.h>
#include <linux/sched.h>
#include <asm/segment.h>
#include <linux/types.h>
#include <linux/wrapper.h>
#include <linux/vmalloc.h>
#include <linux/init.h>

#include "ieee1394.h"
#include "ieee1394_types.h"
#include "hosts.h"
#include "ieee1394_core.h"
#include "highlevel.h"
#include "ohci1394.h"


#ifdef CONFIG_IEEE1394_VERBOSEDEBUG
#define OHCI1394_DEBUG
#endif

#ifdef DBGMSG
#undef DBGMSG
#endif

#ifdef OHCI1394_DEBUG
#define DBGMSG(card, fmt, args...) \
printk(KERN_INFO "ohci1394_%d: " fmt "\n" , card , ## args)
#else
#define DBGMSG(card, fmt, args...)
#endif

#ifdef CONFIG_IEEE1394_OHCI_DMA_DEBUG
#define OHCI_DMA_ALLOC(fmt, args...) \
	HPSB_ERR("ohci1394("__FUNCTION__")alloc(%d): "fmt, \
		++global_outstanding_dmas, ## args)
#define OHCI_DMA_FREE(fmt, args...) \
	HPSB_ERR("ohci1394("__FUNCTION__")free(%d): "fmt, \
		--global_outstanding_dmas, ## args)
u32 global_outstanding_dmas = 0;
#else
#define OHCI_DMA_ALLOC(fmt, args...)
#define OHCI_DMA_FREE(fmt, args...)
#endif

/* print general (card independent) information */
#define PRINT_G(level, fmt, args...) \
printk(level "ohci1394: " fmt "\n" , ## args)

/* print card specific information */
#define PRINT(level, card, fmt, args...) \
printk(level "ohci1394_%d: " fmt "\n" , card , ## args)

#define FAIL(fmt, args...)				\
do {							\
	PRINT_G(KERN_ERR, fmt , ## args);		\
	remove_card(ohci);				\
	return 1;					\
} while(0)

#define PCI_CLASS_FIREWIRE_OHCI     ((PCI_CLASS_SERIAL_FIREWIRE << 8) | 0x10)

static struct pci_device_id ohci1394_pci_tbl[] __devinitdata = {
	{
		class: 		PCI_CLASS_FIREWIRE_OHCI,
		class_mask: 	0x00ffffff,
		vendor:		PCI_ANY_ID,
		device:		PCI_ANY_ID,
		subvendor:	PCI_ANY_ID,
		subdevice:	PCI_ANY_ID,
	},
	{ 0, },
};
MODULE_DEVICE_TABLE(pci, ohci1394_pci_tbl);

static char version[] __devinitdata =
	"v0.50 15/Jul/01 Ben Collins <bcollins@debian.org>";

/* Module Parameters */
MODULE_PARM(attempt_root,"i");
MODULE_PARM_DESC(attempt_root, "Attempt to make the host root.");
static int attempt_root = 0;

#ifdef __LITTLE_ENDIAN
/* Don't waste cycles on same sex byte swaps */
#define packet_swab(w,x,y,z)
#define block_swab32(x,y)
#else
static void packet_swab(quadlet_t *data, char tcode, int len, int payload_swap);
static __inline__ void block_swab32(quadlet_t *data, size_t size);
#endif

static unsigned int card_id_counter = 0;

static void dma_trm_tasklet(unsigned long data);
static void remove_card(struct ti_ohci *ohci);
static void dma_trm_reset(struct dma_trm_ctx *d);

/***********************************
 * IEEE-1394 functionality section *
 ***********************************/

static u8 get_phy_reg(struct ti_ohci *ohci, u8 addr) 
{
	int i, flags;
	quadlet_t r;

	spin_lock_irqsave (&ohci->phy_reg_lock, flags);

	reg_write(ohci, OHCI1394_PhyControl, (((u16)addr << 8) & 0x00000f00) | 0x00008000);

	for (i = 0; i < OHCI_LOOP_COUNT; i++) {
		if (reg_read(ohci, OHCI1394_PhyControl) & 0x80000000)
			break;

		mdelay(1);
	}

	r = reg_read(ohci, OHCI1394_PhyControl);

	if (i >= OHCI_LOOP_COUNT)
		PRINT (KERN_ERR, ohci->id, "Get PHY Reg timeout [0x%08x/0x%08x/%d]\n",
		       r, r & 0x80000000, i);
  
	spin_unlock_irqrestore (&ohci->phy_reg_lock, flags);
     
	return (r & 0x00ff0000) >> 16;
}

static void set_phy_reg(struct ti_ohci *ohci, u8 addr, u8 data)
{
	int i, flags;
	u32 r;

	spin_lock_irqsave (&ohci->phy_reg_lock, flags);

	reg_write(ohci, OHCI1394_PhyControl, 0x00004000 | (((u16)addr << 8) & 0x00000f00) | data);

	for (i = 0; i < OHCI_LOOP_COUNT; i++) {
		r = reg_read(ohci, OHCI1394_PhyControl);
		if (!(r & 0x00004000))
			break;

		mdelay(1);
	}

	if (i == OHCI_LOOP_COUNT)
		PRINT (KERN_ERR, ohci->id, "Set PHY Reg timeout [0x%08x/0x%08x/%d]\n",
		       r, r & 0x00004000, i);

	spin_unlock_irqrestore (&ohci->phy_reg_lock, flags);

	return;
}

/* Or's our value into the current value */
static void set_phy_reg_mask(struct ti_ohci *ohci, u8 addr, u8 data)
{
	u8 old;

	old = get_phy_reg (ohci, addr);
	old |= data;
	set_phy_reg (ohci, addr, old);

	return;
}

static int handle_selfid(struct ti_ohci *ohci, struct hpsb_host *host,
				int phyid, int isroot)
{
	quadlet_t *q = ohci->selfid_buf_cpu;
	quadlet_t self_id_count=reg_read(ohci, OHCI1394_SelfIDCount);
	size_t size;
	quadlet_t q0, q1;

	/* SelfID handling seems much easier than for the aic5800 chip.
	   All the self-id packets, including this devices own self-id,
	   should be correctly arranged in the selfid buffer at this
	   stage */

	/* Check status of self-id reception */

	if (ohci->selfid_swap)
		q0 = le32_to_cpu(q[0]);
	else
		q0 = q[0];

	if ((self_id_count & 0x80000000) || 
	    ((self_id_count & 0x00FF0000) != (q0 & 0x00FF0000))) {
		PRINT(KERN_ERR, ohci->id, 
		      "Error in reception of SelfID packets [0x%08x/0x%08x]",
		      self_id_count, q0);

		/* Tip by James Goodwin <jamesg@Filanet.com>:
		 * We had an error, generate another bus reset in response.  */
		if (ohci->self_id_errors<OHCI1394_MAX_SELF_ID_ERRORS) {
			set_phy_reg_mask (ohci, 1, 0x40);
			ohci->self_id_errors++;
		} else {
			PRINT(KERN_ERR, ohci->id, 
			      "Too many errors on SelfID error reception, giving up!");
		}
		return -1;
	}
	
	size = ((self_id_count & 0x00001FFC) >> 2) - 1;
	q++;

	while (size > 0) {
		if (ohci->selfid_swap) {
			q0 = le32_to_cpu(q[0]);
			q1 = le32_to_cpu(q[1]);
		} else {
			q0 = q[0];
			q1 = q[1];
		}
		
		if (q0 == ~q1) {
			PRINT(KERN_DEBUG, ohci->id, "SelfID packet 0x%x received", q0);
			hpsb_selfid_received(host, cpu_to_be32(q0));
			if (((q0 & 0x3f000000) >> 24) == phyid)
				DBGMSG (ohci->id, "SelfID for this node is 0x%08x", q0);
		} else {
			PRINT(KERN_ERR, ohci->id,
			      "SelfID is inconsistent [0x%08x/0x%08x]", q0, q1);
		}
		q += 2;
		size -= 2;
	}

	PRINT(KERN_DEBUG, ohci->id, "SelfID complete");

	hpsb_selfid_complete(host, phyid, isroot);
	return 0;
}

static int ohci_soft_reset(struct ti_ohci *ohci) {
	int i;

	reg_write(ohci, OHCI1394_HCControlSet, 0x00010000);
  
	for (i = 0; i < OHCI_LOOP_COUNT; i++) {
		if (reg_read(ohci, OHCI1394_HCControlSet) & 0x00010000)
			break;
		mdelay(10);
	}

	PRINT(KERN_DEBUG, ohci->id, "Soft reset finished");

	return 0;
}

static int run_context(struct ti_ohci *ohci, int reg, char *msg)
{
	u32 nodeId;

	/* check that the node id is valid */
	nodeId = reg_read(ohci, OHCI1394_NodeID);
	if (!(nodeId&0x80000000)) {
		PRINT(KERN_ERR, ohci->id, 
		      "Running dma failed because Node ID is not valid");
		return -1;
	}

	/* check that the node number != 63 */
	if ((nodeId&0x3f)==63) {
		PRINT(KERN_ERR, ohci->id, 
		      "Running dma failed because Node ID == 63");
		return -1;
	}
	
	/* Run the dma context */
	reg_write(ohci, reg, 0x8000);
	
	if (msg) PRINT(KERN_DEBUG, ohci->id, "%s", msg);
	
	return 0;
}

/* Generate the dma receive prgs and start the context */
static void initialize_dma_rcv_ctx(struct dma_rcv_ctx *d)
{
	struct ti_ohci *ohci = (struct ti_ohci*)(d->ohci);
	int i;

	ohci1394_stop_context(ohci, d->ctrlClear, NULL);

	for (i=0; i<d->num_desc; i++) {
		
		d->prg_cpu[i]->control =
                   cpu_to_le32((0x280C << 16) | d->buf_size);

		/* End of descriptor list? */
		if ((i+1) < d->num_desc) {
			d->prg_cpu[i]->branchAddress =
				cpu_to_le32((d->prg_bus[i+1] & 0xfffffff0) | 0x1);
		} else {
			d->prg_cpu[i]->branchAddress =
				cpu_to_le32((d->prg_bus[0] & 0xfffffff0));
		}

		d->prg_cpu[i]->address = cpu_to_le32(d->buf_bus[i]);
		d->prg_cpu[i]->status = cpu_to_le32(d->buf_size);
	}

        d->buf_ind = 0;
        d->buf_offset = 0;

	/* Tell the controller where the first AR program is */
	reg_write(ohci, d->cmdPtr, d->prg_bus[0] | 0x1);

	/* Run AR context */
	reg_write(ohci, d->ctrlSet, 0x00008000);

	DBGMSG(ohci->id, "Receive DMA ctx=%d initialized", d->ctx);
}

/* Initialize the dma transmit context */
static void initialize_dma_trm_ctx(struct dma_trm_ctx *d)
{
	struct ti_ohci *ohci = (struct ti_ohci*)(d->ohci);

	/* Stop the context */
	ohci1394_stop_context(ohci, d->ctrlClear, NULL);

        d->prg_ind = 0;
	d->sent_ind = 0;
	d->free_prgs = d->num_desc;
        d->branchAddrPtr = NULL;
	d->fifo_first = NULL;
	d->fifo_last = NULL;
	d->pending_first = NULL;
	d->pending_last = NULL;

	DBGMSG(ohci->id, "Transmit dma ctx=%d initialized", d->ctx);
}

/* Count the number of available iso contexts */
static int get_nb_iso_ctx(struct ti_ohci *ohci, int reg)
{
	int i,ctx=0;
	u32 tmp;

	reg_write(ohci, reg, 0xffffffff);
	tmp = reg_read(ohci, reg);
	
	DBGMSG(ohci->id,"Iso contexts reg: %08x implemented: %08x", reg, tmp);

	/* Count the number of contexts */
	for(i=0; i<32; i++) {
	    	if(tmp & 1) ctx++;
		tmp >>= 1;
	}
	return ctx;
}

/* Global initialization */
static int ohci_initialize(struct hpsb_host *host)
{
	struct ti_ohci *ohci=host->hostdata;
	int retval, i;

	spin_lock_init(&ohci->phy_reg_lock);
	spin_lock_init(&ohci->event_lock);
  
	/*
	 * Tip by James Goodwin <jamesg@Filanet.com>:
	 * We need to add delays after the soft reset, setting LPS, and
	 * enabling our link. This might fixes the self-id reception 
	 * problem at initialization.
	 */ 

	/* Soft reset */
	if ((retval = ohci_soft_reset(ohci)) < 0)
		return retval;

	/* 
	 * Delay after soft reset to make sure everything has settled
	 * down (sanity)
	 */
	mdelay(10);    
  
	/* Set Link Power Status (LPS) */
	reg_write(ohci, OHCI1394_HCControlSet, 0x00080000);

	/*
	 * Delay after setting LPS in order to make sure link/phy
	 * communication is established
	 */
	mdelay(10);   

	/* Set the bus number */
	reg_write(ohci, OHCI1394_NodeID, 0x0000ffc0);

	/* Enable posted writes */
	reg_write(ohci, OHCI1394_HCControlSet, 0x00040000);

	/* Clear link control register */
	reg_write(ohci, OHCI1394_LinkControlClear, 0xffffffff);
  
	/* Enable cycle timer and cycle master */
	reg_write(ohci, OHCI1394_LinkControlSet, 0x00300000);

	/* Clear interrupt registers */
	reg_write(ohci, OHCI1394_IntMaskClear, 0xffffffff);
	reg_write(ohci, OHCI1394_IntEventClear, 0xffffffff);

	/* Set up self-id dma buffer */
	reg_write(ohci, OHCI1394_SelfIDBuffer, ohci->selfid_buf_bus);

	/* enable self-id dma */
	reg_write(ohci, OHCI1394_LinkControlSet, 0x00000200);

	/* Set the configuration ROM mapping register */
	reg_write(ohci, OHCI1394_ConfigROMmap, ohci->csr_config_rom_bus);

	ohci->max_packet_size = 
		1<<(((reg_read(ohci, OHCI1394_BusOptions)>>12)&0xf)+1);
	PRINT(KERN_DEBUG, ohci->id, "Max packet size = %d bytes",
	       ohci->max_packet_size);

	/* Don't accept phy packets into AR request context */ 
	reg_write(ohci, OHCI1394_LinkControlClear, 0x00000400);

	/* Initialize IR dma */
	ohci->nb_iso_rcv_ctx = 
		get_nb_iso_ctx(ohci, OHCI1394_IsoRecvIntMaskSet);
	DBGMSG(ohci->id, "%d iso receive contexts available",
	       ohci->nb_iso_rcv_ctx);
	for (i=0;i<ohci->nb_iso_rcv_ctx;i++) {
		reg_write(ohci, OHCI1394_IsoRcvContextControlClear+32*i,
			  0xffffffff);
		reg_write(ohci, OHCI1394_IsoRcvContextMatch+32*i, 0);
		reg_write(ohci, OHCI1394_IsoRcvCommandPtr+32*i, 0);
	}

	/* Set bufferFill, isochHeader, multichannel for IR context */
	reg_write(ohci, OHCI1394_IsoRcvContextControlSet, 0xd0000000);
			
	/* Set the context match register to match on all tags */
	reg_write(ohci, OHCI1394_IsoRcvContextMatch, 0xf0000000);

	/* Clear the interrupt mask */
	reg_write(ohci, OHCI1394_IsoRecvIntMaskClear, 0xffffffff);
	reg_write(ohci, OHCI1394_IsoRecvIntEventClear, 0xffffffff);

	/* Initialize IT dma */
	ohci->nb_iso_xmit_ctx = 
		get_nb_iso_ctx(ohci, OHCI1394_IsoXmitIntMaskSet);
	DBGMSG(ohci->id, "%d iso transmit contexts available",
	       ohci->nb_iso_xmit_ctx);
	for (i=0;i<ohci->nb_iso_xmit_ctx;i++) {
		reg_write(ohci, OHCI1394_IsoXmitContextControlClear+32*i,
			  0xffffffff);
		reg_write(ohci, OHCI1394_IsoXmitCommandPtr+32*i, 0);
	}
	
	/* Clear the interrupt mask */
	reg_write(ohci, OHCI1394_IsoXmitIntMaskClear, 0xffffffff);
	reg_write(ohci, OHCI1394_IsoXmitIntEventClear, 0xffffffff);

	/* Clear the multi channel mask high and low registers */
	reg_write(ohci, OHCI1394_IRMultiChanMaskHiClear, 0xffffffff);
	reg_write(ohci, OHCI1394_IRMultiChanMaskLoClear, 0xffffffff);

	/* Initialize AR dma */
	initialize_dma_rcv_ctx(ohci->ar_req_context);
	initialize_dma_rcv_ctx(ohci->ar_resp_context);

	/* Initialize AT dma */
	initialize_dma_trm_ctx(ohci->at_req_context);
	initialize_dma_trm_ctx(ohci->at_resp_context);

	/* Initialize IR dma */
	initialize_dma_rcv_ctx(ohci->ir_context);

        /* Initialize IT dma */
        initialize_dma_trm_ctx(ohci->it_context);

	/* Set up isoRecvIntMask to generate interrupts for context 0
	   (thanks to Michael Greger for seeing that I forgot this) */
	reg_write(ohci, OHCI1394_IsoRecvIntMaskSet, 0x00000001);

	/* Set up isoXmitIntMask to generate interrupts for context 0 */
	reg_write(ohci, OHCI1394_IsoXmitIntMaskSet, 0x00000001);

	/* 
	 * Accept AT requests from all nodes. This probably 
	 * will have to be controlled from the subsystem
	 * on a per node basis.
	 */
	reg_write(ohci,OHCI1394_AsReqFilterHiSet, 0x80000000);

	/* Specify AT retries */
	reg_write(ohci, OHCI1394_ATRetries, 
		  OHCI1394_MAX_AT_REQ_RETRIES |
		  (OHCI1394_MAX_AT_RESP_RETRIES<<4) |
		  (OHCI1394_MAX_PHYS_RESP_RETRIES<<8));

	/* We don't want hardware swapping */
	reg_write(ohci, OHCI1394_HCControlClear, 0x40000000);

	/* Enable interrupts */
	reg_write(ohci, OHCI1394_IntMaskSet, 
		  OHCI1394_masterIntEnable | 
		  OHCI1394_phyRegRcvd | 
		  OHCI1394_busReset | 
		  OHCI1394_selfIDComplete |
		  OHCI1394_RSPkt |
		  OHCI1394_RQPkt |
		  OHCI1394_respTxComplete |
		  OHCI1394_reqTxComplete |
		  OHCI1394_isochRx |
		  OHCI1394_isochTx |
		  OHCI1394_unrecoverableError
		);

	/* Enable link */
	reg_write(ohci, OHCI1394_HCControlSet, 0x00020000);

	return 1;
}

static void ohci_remove(struct hpsb_host *host)
{
	struct ti_ohci *ohci;

	if (host != NULL) {
		ohci = host->hostdata;
		remove_card(ohci);
	}
}

/* 
 * Insert a packet in the AT DMA fifo and generate the DMA prg
 * FIXME: rewrite the program in order to accept packets crossing
 *        page boundaries.
 *        check also that a single dma descriptor doesn't cross a 
 *        page boundary.
 */
static void insert_packet(struct ti_ohci *ohci,
			  struct dma_trm_ctx *d, struct hpsb_packet *packet)
{
	u32 cycleTimer;
	int idx = d->prg_ind;

	DBGMSG(ohci->id, "Inserting packet for node %d, tlabel=%d, tcode=0x%x, speed=%d\n",
			packet->node_id, packet->tlabel, packet->tcode, packet->speed_code);

	d->prg_cpu[idx]->begin.address = 0;
	d->prg_cpu[idx]->begin.branchAddress = 0;
	if (d->ctx==1) {
		/* 
		 * For response packets, we need to put a timeout value in
		 * the 16 lower bits of the status... let's try 1 sec timeout 
		 */ 
		cycleTimer = reg_read(ohci, OHCI1394_IsochronousCycleTimer);
		d->prg_cpu[idx]->begin.status = cpu_to_le32(
			(((((cycleTimer>>25)&0x7)+1)&0x7)<<13) | 
			((cycleTimer&0x01fff000)>>12));

		DBGMSG(ohci->id, "cycleTimer: %08x timeStamp: %08x",
		       cycleTimer, d->prg_cpu[idx]->begin.status);
	} else 
		d->prg_cpu[idx]->begin.status = 0;

        if ( (packet->type == async) || (packet->type == raw) ) {

                if (packet->type == raw) {
			d->prg_cpu[idx]->data[0] = cpu_to_le32(OHCI1394_TCODE_PHY<<4);
                        d->prg_cpu[idx]->data[1] = packet->header[0];
                        d->prg_cpu[idx]->data[2] = packet->header[1];
                } else {
                        d->prg_cpu[idx]->data[0] = packet->speed_code<<16 |
                                (packet->header[0] & 0xFFFF);
                        d->prg_cpu[idx]->data[1] =
                                (packet->header[1] & 0xFFFF) | 
                                (packet->header[0] & 0xFFFF0000);
                        d->prg_cpu[idx]->data[2] = packet->header[2];
                        d->prg_cpu[idx]->data[3] = packet->header[3];
			packet_swab(d->prg_cpu[idx]->data, packet->tcode,
					packet->header_size>>2, ohci->payload_swap);
                }

                if (packet->data_size) { /* block transmit */
                        d->prg_cpu[idx]->begin.control =
                                cpu_to_le32(OUTPUT_MORE_IMMEDIATE | 0x10);
                        d->prg_cpu[idx]->end.control =
                                cpu_to_le32(OUTPUT_LAST | packet->data_size);
                        /* 
                         * Check that the packet data buffer
                         * does not cross a page boundary.
                         */
                        if (cross_bound((unsigned long)packet->data, 
                                        packet->data_size)>0) {
                                /* FIXME: do something about it */
                                PRINT(KERN_ERR, ohci->id, __FUNCTION__
                                      ": packet data addr: %p size %Zd bytes "
                                      "cross page boundary", 
                                      packet->data, packet->data_size);
                        }

                        d->prg_cpu[idx]->end.address = cpu_to_le32(
                                pci_map_single(ohci->dev, packet->data,
                                               packet->data_size,
                                               PCI_DMA_TODEVICE));
			OHCI_DMA_ALLOC("single, block transmit packet");

			if (ohci->payload_swap)
				block_swab32(packet->data, packet->data_size >> 2);

                        d->prg_cpu[idx]->end.branchAddress = 0;
                        d->prg_cpu[idx]->end.status = 0;
                        if (d->branchAddrPtr) 
                                *(d->branchAddrPtr) =
					cpu_to_le32(d->prg_bus[idx] | 0x3);
                        d->branchAddrPtr =
                                &(d->prg_cpu[idx]->end.branchAddress);
                } else { /* quadlet transmit */
                        if (packet->type == raw)
                                d->prg_cpu[idx]->begin.control = cpu_to_le32(
                                        OUTPUT_LAST_IMMEDIATE |
                                        (packet->header_size+4));
                        else
                                d->prg_cpu[idx]->begin.control = cpu_to_le32(
                                        OUTPUT_LAST_IMMEDIATE |
                                        packet->header_size);

                        if (d->branchAddrPtr) 
                                *(d->branchAddrPtr) =
					cpu_to_le32(d->prg_bus[idx] | 0x2);
                        d->branchAddrPtr =
                                &(d->prg_cpu[idx]->begin.branchAddress);
                }

        } else { /* iso packet */
                d->prg_cpu[idx]->data[0] = packet->speed_code<<16 |
                        (packet->header[0] & 0xFFFF);
                d->prg_cpu[idx]->data[1] = packet->header[0] & 0xFFFF0000;
		packet_swab(d->prg_cpu[idx]->data, packet->tcode, packet->header_size>>2,
				ohci->payload_swap);
  
                d->prg_cpu[idx]->begin.control = cpu_to_le32(OUTPUT_MORE_IMMEDIATE | 0x8);
                d->prg_cpu[idx]->end.control = cpu_to_le32(
                        OUTPUT_LAST | 0x08000000 | packet->data_size);
                d->prg_cpu[idx]->end.address = cpu_to_le32(
				pci_map_single(ohci->dev, packet->data,
				packet->data_size, PCI_DMA_TODEVICE));
		OHCI_DMA_ALLOC("single, iso transmit packet");

		if (ohci->payload_swap)
			block_swab32(packet->data, packet->data_size>>2);

                d->prg_cpu[idx]->end.branchAddress = 0;
                d->prg_cpu[idx]->end.status = 0;
                DBGMSG(ohci->id, "iso xmit context info: header[%08x %08x]\n"
                       "                       begin=%08x %08x %08x %08x\n"
                       "                             %08x %08x %08x %08x\n"
                       "                       end  =%08x %08x %08x %08x",
                       d->prg_cpu[idx]->data[0], d->prg_cpu[idx]->data[1],
                       d->prg_cpu[idx]->begin.control,
                       d->prg_cpu[idx]->begin.address,
                       d->prg_cpu[idx]->begin.branchAddress,
                       d->prg_cpu[idx]->begin.status,
                       d->prg_cpu[idx]->data[0],
                       d->prg_cpu[idx]->data[1],
                       d->prg_cpu[idx]->data[2],
                       d->prg_cpu[idx]->data[3],
                       d->prg_cpu[idx]->end.control,
                       d->prg_cpu[idx]->end.address,
                       d->prg_cpu[idx]->end.branchAddress,
                       d->prg_cpu[idx]->end.status);
                if (d->branchAddrPtr) 
  		        *(d->branchAddrPtr) = cpu_to_le32(d->prg_bus[idx] | 0x3);
                d->branchAddrPtr = &(d->prg_cpu[idx]->end.branchAddress);
        }
	d->free_prgs--;

	/* queue the packet in the appropriate context queue */
	if (d->fifo_last) {
		d->fifo_last->xnext = packet;
		d->fifo_last = packet;
	} else {
		d->fifo_first = packet;
		d->fifo_last = packet;
	}
	d->prg_ind = (d->prg_ind+1)%d->num_desc;
}

/*
 * This function fills the AT FIFO with the (eventual) pending packets
 * and runs or wakes up the AT DMA prg if necessary.
 *
 * The function MUST be called with the d->lock held.
 */ 
static int dma_trm_flush(struct ti_ohci *ohci, struct dma_trm_ctx *d)
{
	int idx,z;

	if (d->pending_first == NULL || d->free_prgs == 0) 
		return 0;

	idx = d->prg_ind;
	z = (d->pending_first->data_size) ? 3 : 2;

	/* insert the packets into the at dma fifo */
	while (d->free_prgs>0 && d->pending_first) {
		insert_packet(ohci, d, d->pending_first);
		d->pending_first = d->pending_first->xnext;
	}
	if (d->pending_first == NULL) 
		d->pending_last = NULL;
	else
		PRINT(KERN_INFO, ohci->id, 
		      "Transmit DMA FIFO ctx=%d is full... waiting",d->ctx);

	/* Is the context running ? (should be unless it is 
	   the first packet to be sent in this context) */
	if (!(reg_read(ohci, d->ctrlSet) & 0x8000)) {
		DBGMSG(ohci->id,"Starting transmit DMA ctx=%d",d->ctx);
		reg_write(ohci, d->cmdPtr, d->prg_bus[idx]|z);
		run_context(ohci, d->ctrlSet, NULL);
	}
	else {
		/* Wake up the dma context if necessary */
		if (!(reg_read(ohci, d->ctrlSet) & 0x400)) {
			DBGMSG(ohci->id,"Waking transmit DMA ctx=%d",d->ctx);
			reg_write(ohci, d->ctrlSet, 0x1000);
		}
	}
	return 1;
}

/* Transmission of an async packet */
static int ohci_transmit(struct hpsb_host *host, struct hpsb_packet *packet)
{
	struct ti_ohci *ohci = host->hostdata;
	struct dma_trm_ctx *d;
	unsigned char tcode;
	unsigned long flags;

	if (packet->data_size > ohci->max_packet_size) {
		PRINT(KERN_ERR, ohci->id, 
		      "Transmit packet size %Zd is too big",
		      packet->data_size);
		return 0;
	}
	packet->xnext = NULL;

	/* Decide wether we have an iso, a request, or a response packet */
	tcode = (packet->header[0]>>4)&0xf;
	if (tcode == TCODE_ISO_DATA) d = ohci->it_context;
	else if (tcode & 0x02) d = ohci->at_resp_context;
	else d = ohci->at_req_context;

	spin_lock_irqsave(&d->lock,flags);

	/* queue the packet for later insertion into the dma fifo */
	if (d->pending_last) {
		d->pending_last->xnext = packet;
		d->pending_last = packet;
	}
	else {
		d->pending_first = packet;
		d->pending_last = packet;
	}
	
	dma_trm_flush(ohci, d);

	spin_unlock_irqrestore(&d->lock,flags);

	return 1;
}

static int ohci_devctl(struct hpsb_host *host, enum devctl_cmd cmd, int arg)
{
	struct ti_ohci *ohci = host->hostdata;
	int retval = 0;
	unsigned long flags;

	switch (cmd) {
	case RESET_BUS:
		PRINT (KERN_DEBUG, ohci->id, "Resetting bus on request%s",
		       ((host->attempt_root || attempt_root) ? 
		       " and attempting to become root" : ""));
		set_phy_reg_mask (ohci, 1, 0x40 | ((host->attempt_root || attempt_root) ?
				  0x80 : 0));
		break;

	case GET_CYCLE_COUNTER:
		retval = reg_read(ohci, OHCI1394_IsochronousCycleTimer);
		break;
	
	case SET_CYCLE_COUNTER:
		reg_write(ohci, OHCI1394_IsochronousCycleTimer, arg);
		break;
	
	case SET_BUS_ID:
		PRINT(KERN_ERR, ohci->id, "devctl command SET_BUS_ID err");
		break;

	case ACT_CYCLE_MASTER:
		if (arg) {
			/* check if we are root and other nodes are present */
			u32 nodeId = reg_read(ohci, OHCI1394_NodeID);
			if ((nodeId & (1<<30)) && (nodeId & 0x3f)) {
				/*
				 * enable cycleTimer, cycleMaster
				 */
				DBGMSG(ohci->id, "Cycle master enabled");
				reg_write(ohci, OHCI1394_LinkControlSet, 
					  0x00300000);
			}
		} else {
			/* disable cycleTimer, cycleMaster, cycleSource */
			reg_write(ohci, OHCI1394_LinkControlClear, 0x00700000);
		}
		break;

	case CANCEL_REQUESTS:
		DBGMSG(ohci->id, "Cancel request received");
		dma_trm_reset(ohci->at_req_context);
		dma_trm_reset(ohci->at_resp_context);
		break;

	case MODIFY_USAGE:
                if (arg) {
                        MOD_INC_USE_COUNT;
                } else {
                        MOD_DEC_USE_COUNT;
                }
                break;

	case ISO_LISTEN_CHANNEL:
        {
		u64 mask;

		if (arg<0 || arg>63) {
			PRINT(KERN_ERR, ohci->id, __FUNCTION__
			      "IS0 listne channel %d is out of range", 
			      arg);
			return -EFAULT;
		}

		mask = (u64)0x1<<arg;
		
                spin_lock_irqsave(&ohci->IR_channel_lock, flags);

		if (ohci->ISO_channel_usage & mask) {
			PRINT(KERN_ERR, ohci->id, __FUNCTION__
			      "IS0 listen channel %d is already used", 
			      arg);
			spin_unlock_irqrestore(&ohci->IR_channel_lock, flags);
			return -EFAULT;
		}
		
		ohci->ISO_channel_usage |= mask;

		if (arg>31) 
			reg_write(ohci, OHCI1394_IRMultiChanMaskHiSet, 
				  1<<(arg-32));			
		else
			reg_write(ohci, OHCI1394_IRMultiChanMaskLoSet, 
				  1<<arg);			

                spin_unlock_irqrestore(&ohci->IR_channel_lock, flags);
                DBGMSG(ohci->id, "Listening enabled on channel %d", arg);
                break;
        }
	case ISO_UNLISTEN_CHANNEL:
        {
		u64 mask;

		if (arg<0 || arg>63) {
			PRINT(KERN_ERR, ohci->id, __FUNCTION__
			      "IS0 unlisten channel %d is out of range", 
			      arg);
			return -EFAULT;
		}

		mask = (u64)0x1<<arg;
		
                spin_lock_irqsave(&ohci->IR_channel_lock, flags);

		if (!(ohci->ISO_channel_usage & mask)) {
			PRINT(KERN_ERR, ohci->id, __FUNCTION__
			      "IS0 unlisten channel %d is not used", 
			      arg);
			spin_unlock_irqrestore(&ohci->IR_channel_lock, flags);
			return -EFAULT;
		}
		
		ohci->ISO_channel_usage &= ~mask;

		if (arg>31) 
			reg_write(ohci, OHCI1394_IRMultiChanMaskHiClear, 
				  1<<(arg-32));			
		else
			reg_write(ohci, OHCI1394_IRMultiChanMaskLoClear, 
				  1<<arg);			

                spin_unlock_irqrestore(&ohci->IR_channel_lock, flags);
                DBGMSG(ohci->id, "Listening disabled on channel %d", arg);
                break;
        }
	default:
		PRINT_G(KERN_ERR, "ohci_devctl cmd %d not implemented yet",
			cmd);
		break;
	}
	return retval;
}

/***************************************
 * IEEE-1394 functionality section END *
 ***************************************/


/********************************************************
 * Global stuff (interrupt handler, init/shutdown code) *
 ********************************************************/

static void dma_trm_reset(struct dma_trm_ctx *d)
{
	struct ti_ohci *ohci;
	unsigned long flags;
        struct hpsb_packet *nextpacket;

	if (d==NULL) {
		PRINT_G(KERN_ERR, "dma_trm_reset called with NULL arg");
		return;
	}
	ohci = (struct ti_ohci *)(d->ohci);
	ohci1394_stop_context(ohci, d->ctrlClear, NULL);

	spin_lock_irqsave(&d->lock,flags);

	/* Is there still any packet pending in the fifo ? */
	while(d->fifo_first) {
		PRINT(KERN_INFO, ohci->id, 
		      "AT dma reset ctx=%d, aborting transmission", 
		      d->ctx);
                nextpacket = d->fifo_first->xnext;
		hpsb_packet_sent(ohci->host, d->fifo_first, ACKX_ABORTED);
		d->fifo_first = nextpacket;
	}
	d->fifo_first = d->fifo_last = NULL;

	/* is there still any packet pending ? */
	while(d->pending_first) {
		PRINT(KERN_INFO, ohci->id, 
		      "AT dma reset ctx=%d, aborting transmission", 
		      d->ctx);
                nextpacket = d->pending_first->xnext;
		hpsb_packet_sent(ohci->host, d->pending_first, 
				 ACKX_ABORTED);
		d->pending_first = nextpacket;
	}
	d->pending_first = d->pending_last = NULL;
	
	d->branchAddrPtr=NULL;
	d->sent_ind = d->prg_ind;
	d->free_prgs = d->num_desc;
	spin_unlock_irqrestore(&d->lock,flags);
}

static void ohci_irq_handler(int irq, void *dev_id,
                             struct pt_regs *regs_are_unused)
{
	quadlet_t event, node_id;
	struct ti_ohci *ohci = (struct ti_ohci *)dev_id;
	struct hpsb_host *host = ohci->host;
	int phyid = -1, isroot = 0, flags;

	/* Read the interrupt event register */
	spin_lock_irqsave(&ohci->event_lock, flags);
	event = reg_read(ohci, OHCI1394_IntEventClear);
	reg_write(ohci, OHCI1394_IntEventClear, event);
	spin_unlock_irqrestore(&ohci->event_lock, flags);

	if (!event) return;

	DBGMSG(ohci->id, "IntEvent: %08x", event);

	/* Die right here an now */
	if (event & OHCI1394_unrecoverableError) {
		PRINT(KERN_ERR, ohci->id, "Unrecoverable error, shutting down card!");
		remove_card(ohci);
		return;
	}

	/* Someone wants a bus reset. Better watch what you wish for...
	 *
	 * XXX: Read 6.1.1 of the OHCI1394 spec. We need to take special
	 * care with the BusReset Interrupt, before and until the SelfID
	 * phase is over. This is why the SelfID phase sometimes fails for
	 * this driver.  */
	if (event & OHCI1394_busReset) {
		if (!host->in_bus_reset) {
			PRINT(KERN_DEBUG, ohci->id, "Bus reset requested");
			
			/* Wait for the AT fifo to be flushed */
			dma_trm_reset(ohci->at_req_context);
			dma_trm_reset(ohci->at_resp_context);

			/* Subsystem call */
			hpsb_bus_reset(ohci->host);
			
			ohci->NumBusResets++;
		}
		event &= ~OHCI1394_busReset;
	}

	/* XXX: We need a way to also queue the OHCI1394_reqTxComplete,
	 * but for right now we simply run it upon reception, to make sure
	 * we get sent acks before response packets. This sucks mainly
	 * because it halts the interrupt handler.  */
	if (event & OHCI1394_reqTxComplete) {
		struct dma_trm_ctx *d = ohci->at_req_context;
		DBGMSG(ohci->id, "Got reqTxComplete interrupt "
		       "status=0x%08X", reg_read(ohci, d->ctrlSet));
		if (reg_read(ohci, d->ctrlSet) & 0x800)
			ohci1394_stop_context(ohci, d->ctrlClear,
					      "reqTxComplete");
		else
			dma_trm_tasklet ((unsigned long)d);
		event &= ~OHCI1394_reqTxComplete;
	}
	if (event & OHCI1394_respTxComplete) {
		struct dma_trm_ctx *d = ohci->at_resp_context;
		DBGMSG(ohci->id, "Got respTxComplete interrupt "
		       "status=0x%08X", reg_read(ohci, d->ctrlSet));
		if (reg_read(ohci, d->ctrlSet) & 0x800)
			ohci1394_stop_context(ohci, d->ctrlClear,
					      "respTxComplete");
		else
			tasklet_schedule(&d->task);
		event &= ~OHCI1394_respTxComplete;
	}
	if (event & OHCI1394_RQPkt) {
		struct dma_rcv_ctx *d = ohci->ar_req_context;
		DBGMSG(ohci->id, "Got RQPkt interrupt status=0x%08X",
		       reg_read(ohci, d->ctrlSet));
		if (reg_read(ohci, d->ctrlSet) & 0x800)
			ohci1394_stop_context(ohci, d->ctrlClear, "RQPkt");
		else
			tasklet_schedule(&d->task);
		event &= ~OHCI1394_RQPkt;
	}
	if (event & OHCI1394_RSPkt) {
		struct dma_rcv_ctx *d = ohci->ar_resp_context;
		DBGMSG(ohci->id, "Got RSPkt interrupt status=0x%08X",
		       reg_read(ohci, d->ctrlSet));
		if (reg_read(ohci, d->ctrlSet) & 0x800)
			ohci1394_stop_context(ohci, d->ctrlClear, "RSPkt");
		else
			tasklet_schedule(&d->task);
		event &= ~OHCI1394_RSPkt;
	}
	if (event & OHCI1394_isochRx) {
		quadlet_t isoRecvIntEvent;
		struct dma_rcv_ctx *d = ohci->ir_context;
		isoRecvIntEvent = 
			reg_read(ohci, OHCI1394_IsoRecvIntEventSet);
		reg_write(ohci, OHCI1394_IsoRecvIntEventClear,
			  isoRecvIntEvent);
		DBGMSG(ohci->id, "Got isochRx interrupt "
		       "status=0x%08X isoRecvIntEvent=%08x", 
		       reg_read(ohci, d->ctrlSet), isoRecvIntEvent);
		if (isoRecvIntEvent & 0x1) {
			if (reg_read(ohci, d->ctrlSet) & 0x800)
				ohci1394_stop_context(ohci, d->ctrlClear, 
					     "isochRx");
			else
				tasklet_schedule(&d->task);
		}
		if (ohci->video_tmpl) 
			ohci->video_tmpl->irq_handler(ohci->id, isoRecvIntEvent,
						      0);
		event &= ~OHCI1394_isochRx;
	}
	if (event & OHCI1394_isochTx) {
		quadlet_t isoXmitIntEvent;
		struct dma_trm_ctx *d = ohci->it_context;
		isoXmitIntEvent = 
			reg_read(ohci, OHCI1394_IsoXmitIntEventSet);
		reg_write(ohci, OHCI1394_IsoXmitIntEventClear,
			  isoXmitIntEvent);
                       DBGMSG(ohci->id, "Got isochTx interrupt "
                               "status=0x%08x isoXmitIntEvent=%08x",
                              reg_read(ohci, d->ctrlSet), isoXmitIntEvent);
		if (ohci->video_tmpl) 
			ohci->video_tmpl->irq_handler(ohci->id, 0,
						      isoXmitIntEvent);
		if (isoXmitIntEvent & 0x1) {
			if (reg_read(ohci, d->ctrlSet) & 0x800)
				ohci1394_stop_context(ohci, d->ctrlClear, "isochTx");
			else
				tasklet_schedule(&d->task);
		}
		event &= ~OHCI1394_isochTx;
	}
	if (event & OHCI1394_selfIDComplete) {
		if (host->in_bus_reset) {
			node_id = reg_read(ohci, OHCI1394_NodeID); 

			/* If our nodeid is not valid, give a msec delay
			 * to let it settle in and try again.  */
			if (!(node_id & 0x80000000)) {
				mdelay(1);
				node_id = reg_read(ohci, OHCI1394_NodeID);
			}

			if (node_id & 0x80000000) { /* NodeID valid */
				phyid =  node_id & 0x0000003f;
				isroot = (node_id & 0x40000000) != 0;

				PRINT(KERN_DEBUG, ohci->id,
				      "SelfID interrupt received "
				      "(phyid %d, %s)", phyid, 
				      (isroot ? "root" : "not root"));

				handle_selfid(ohci, host, 
					      phyid, isroot);
			} else 
				PRINT(KERN_ERR, ohci->id, 
				      "SelfID interrupt received, but "
				      "NodeID is not valid: %08X",
				      node_id);

			/* Accept Physical requests from all nodes. */
			reg_write(ohci,OHCI1394_AsReqFilterHiSet, 
				  0xffffffff);
			reg_write(ohci,OHCI1394_AsReqFilterLoSet, 
				  0xffffffff);
			/* Turn on phys dma reception. We should
			 * probably manage the filtering somehow, 
			 * instead of blindly turning it on.  */
			reg_write(ohci,OHCI1394_PhyReqFilterHiSet,
				  0xffffffff);
			reg_write(ohci,OHCI1394_PhyReqFilterLoSet,
				  0xffffffff);
                       	reg_write(ohci,OHCI1394_PhyUpperBound,
				  0xffff0000);
		} else
			PRINT(KERN_ERR, ohci->id, 
			      "SelfID received outside of bus reset sequence");
		event &= ~OHCI1394_selfIDComplete;
	}
	if (event & OHCI1394_phyRegRcvd) {
		if (host->in_bus_reset) {
			DBGMSG (ohci->id, "PhyControl: %08X", 
				reg_read(ohci, OHCI1394_PhyControl));
		} else
			PRINT(KERN_ERR, ohci->id, 
			      "Physical register received outside of bus reset sequence");
		event &= ~OHCI1394_phyRegRcvd;
	}
	if (event)
		PRINT(KERN_ERR, ohci->id, "Unhandled interrupt(s) 0x%08x\n",
		      event);
}

/* Put the buffer back into the dma context */
static void insert_dma_buffer(struct dma_rcv_ctx *d, int idx)
{
	struct ti_ohci *ohci = (struct ti_ohci*)(d->ohci);
	DBGMSG(ohci->id, "Inserting dma buf ctx=%d idx=%d", d->ctx, idx);

	d->prg_cpu[idx]->status = cpu_to_le32(d->buf_size);
	d->prg_cpu[idx]->branchAddress &= le32_to_cpu(0xfffffff0);
	idx = (idx + d->num_desc - 1 ) % d->num_desc;
	d->prg_cpu[idx]->branchAddress |= le32_to_cpu(0x00000001);

	/* wake up the dma context if necessary */
	if (!(reg_read(ohci, d->ctrlSet) & 0x400)) {
		PRINT(KERN_INFO, ohci->id, 
		      "Waking dma ctx=%d ... processing is probably too slow",
		      d->ctx);
		reg_write(ohci, d->ctrlSet, 0x1000);
	}
}

#define cond_le32_to_cpu(data, noswap) \
	(noswap ? data : le32_to_cpu(data))

static const int TCODE_SIZE[16] = {20, 0, 16, -1, 16, 20, 20, 0, 
			    -1, 0, -1, 0, -1, -1, 16, -1};

/* 
 * Determine the length of a packet in the buffer
 * Optimization suggested by Pascal Drolet <pascal.drolet@informission.ca>
 */
static __inline__ int packet_length(struct dma_rcv_ctx *d, int idx, quadlet_t *buf_ptr,
			 int offset, unsigned char tcode, int noswap)
{
	int length = -1;

	if (d->ctx < 2) { /* Async Receive Response/Request */
		length = TCODE_SIZE[tcode];
		if (length == 0) {
			if (offset + 12 >= d->buf_size) {
				length = (cond_le32_to_cpu(d->buf_cpu[(idx + 1) % d->num_desc]
						[3 - ((d->buf_size - offset) >> 2)], noswap) >> 16);
			} else {
				length = (cond_le32_to_cpu(buf_ptr[3], noswap) >> 16);
			}
			length += 20;
		}
	} else if (d->ctx == 2) { /* Iso receive */
		/* Assumption: buffer fill mode with header/trailer */
		length = (cond_le32_to_cpu(buf_ptr[0], noswap) >> 16) + 8;
	}

	if (length > 0 && length % 4)
		length += 4 - (length % 4);

	return length;
}

/* Tasklet that processes dma receive buffers */
static void dma_rcv_tasklet (unsigned long data)
{
	struct dma_rcv_ctx *d = (struct dma_rcv_ctx*)data;
	struct ti_ohci *ohci = (struct ti_ohci*)(d->ohci);
	unsigned int split_left, idx, offset, rescount;
	unsigned char tcode;
	int length, bytes_left, ack, flags;
	quadlet_t *buf_ptr;
	char *split_ptr;
	char msg[256];

	spin_lock_irqsave(&d->lock, flags);

	idx = d->buf_ind;
	offset = d->buf_offset;
	buf_ptr = d->buf_cpu[idx] + offset/4;

	dma_cache_wback_inv(&(d->prg_cpu[idx]->status), sizeof(d->prg_cpu[idx]->status));
	rescount = le32_to_cpu(d->prg_cpu[idx]->status) & 0xffff;

	bytes_left = d->buf_size - rescount - offset;
	dma_cache_wback_inv(buf_ptr, bytes_left);

	while (bytes_left > 0) {
		tcode = (cond_le32_to_cpu(buf_ptr[0], ohci->payload_swap) >> 4) & 0xf;

		/* packet_length() will return < 4 for an error */
		length = packet_length(d, idx, buf_ptr, offset, tcode, ohci->payload_swap);

		if (length < 4) { /* something is wrong */
			sprintf(msg,"Unexpected tcode 0x%x(0x%08x) in AR ctx=%d, length=%d",
				tcode, cond_le32_to_cpu(buf_ptr[0], ohci->payload_swap),
				d->ctx, length);
			ohci1394_stop_context(ohci, d->ctrlClear, msg);
			spin_unlock_irqrestore(&d->lock, flags);
			return;
		}

		/* The first case is where we have a packet that crosses
		 * over more than one descriptor. The next case is where
		 * it's all in the first descriptor.  */
		if ((offset + length) > d->buf_size) {
			DBGMSG(ohci->id,"Split packet rcv'd\n");
			if (length > d->split_buf_size) {
				ohci1394_stop_context(ohci, d->ctrlClear,
					     "Split packet size exceeded");
				d->buf_ind = idx;
				d->buf_offset = offset;
				spin_unlock_irqrestore(&d->lock, flags);
				return;
			}
#if 0
			if (le32_to_cpu(d->prg_cpu[(idx+1)%d->num_desc]->status)
			    == d->buf_size) {
				/* Other part of packet not written yet.
				 * this should never happen I think
				 * anyway we'll get it on the next call.  */
				PRINT(KERN_INFO, ohci->id,
				      "Got only half a packet!");
				d->buf_ind = idx;
				d->buf_offset = offset;
				spin_unlock_irqrestore(&d->lock, flags);
				return;
			}
#endif
			split_left = length;
			split_ptr = (char *)d->spb;
			memcpy(split_ptr,buf_ptr,d->buf_size-offset);
			split_left -= d->buf_size-offset;
			split_ptr += d->buf_size-offset;
			insert_dma_buffer(d, idx);
			idx = (idx+1) % d->num_desc;
			buf_ptr = d->buf_cpu[idx];
			dma_cache_wback_inv(buf_ptr, d->buf_size);
			offset=0;

			while (split_left >= d->buf_size) {
				memcpy(split_ptr,buf_ptr,d->buf_size);
				split_ptr += d->buf_size;
				split_left -= d->buf_size;
				insert_dma_buffer(d, idx);
				idx = (idx+1) % d->num_desc;
				buf_ptr = d->buf_cpu[idx];
                                dma_cache_wback_inv(buf_ptr, d->buf_size);
			}

			if (split_left > 0) {
				memcpy(split_ptr, buf_ptr, split_left);
				offset = split_left;
				buf_ptr += offset/4;
			}
		} else {
			DBGMSG(ohci->id,"Single packet rcv'd\n");
			memcpy(d->spb, buf_ptr, length);
			offset += length;
			buf_ptr += length/4;
			if (offset==d->buf_size) {
				insert_dma_buffer(d, idx);
				idx = (idx+1) % d->num_desc;
				buf_ptr = d->buf_cpu[idx];
				offset=0;
			}
		}
		
		/* We get one phy packet to the async descriptor for each
		 * bus reset. We always ignore it.  */
		if (tcode != OHCI1394_TCODE_PHY) {
			if (!ohci->payload_swap)
				packet_swab(d->spb, tcode, (length - 4) >> 2, 0);

			DBGMSG(ohci->id, "Packet received from node"
				" %d ack=0x%02X spd=%d tcode=0x%X"
				" length=%d ctx=%d tlabel=%d",
				(d->spb[1]>>16)&0x3f,
				(cond_le32_to_cpu(d->spb[length/4-1], ohci->payload_swap)>>16)&0x1f,
				(cond_le32_to_cpu(d->spb[length/4-1], ohci->payload_swap)>>21)&0x3,
				tcode, length, d->ctx,
				(cond_le32_to_cpu(d->spb[length/4-1], ohci->payload_swap)>>10)&0x3f);

			ack = (((cond_le32_to_cpu(d->spb[length/4-1], ohci->payload_swap)>>16)&0x1f)
				== 0x11) ? 1 : 0;

			hpsb_packet_received(ohci->host, d->spb, 
					     length-4, ack);
		}
#if OHCI1394_DEBUG
		else
			PRINT (KERN_DEBUG, ohci->id, "Got phy packet ctx=%d ... discarded",
			       d->ctx);
#endif

                dma_cache_wback_inv(&(d->prg_cpu[idx]->status),
                        sizeof(d->prg_cpu[idx]->status));
	       	rescount = le32_to_cpu(d->prg_cpu[idx]->status) & 0xffff;

		bytes_left = d->buf_size - rescount - offset;

	}

	d->buf_ind = idx;
	d->buf_offset = offset;

	spin_unlock_irqrestore(&d->lock, flags);
}

/* Bottom half that processes sent packets */
static void dma_trm_tasklet (unsigned long data)
{
	struct dma_trm_ctx *d = (struct dma_trm_ctx*)data;
	struct ti_ohci *ohci = (struct ti_ohci*)(d->ohci);
	struct hpsb_packet *packet, *nextpacket;
	unsigned long flags;
	u32 ack;
        size_t datasize;

	spin_lock_irqsave(&d->lock, flags);

	if (d->fifo_first == NULL) {
#if 0
		ohci1394_stop_context(ohci, d->ctrlClear, 
			     "Packet sent ack received but queue is empty");
#endif
		spin_unlock_irqrestore(&d->lock, flags);
		return;
	}

	while (d->fifo_first) {
		packet = d->fifo_first;
                datasize = d->fifo_first->data_size;
		if (datasize && packet->type != raw)
			ack = le32_to_cpu(
				d->prg_cpu[d->sent_ind]->end.status) >> 16;
		else 
			ack = le32_to_cpu(
				d->prg_cpu[d->sent_ind]->begin.status) >> 16;

		if (ack == 0) 
			/* this packet hasn't been sent yet*/
			break;

#ifdef OHCI1394_DEBUG
		if (datasize)
			DBGMSG(ohci->id,
			       "Packet sent to node %d tcode=0x%X tLabel="
			       "0x%02X ack=0x%X spd=%d dataLength=%d ctx=%d", 
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[1])
                                        >>16)&0x3f,
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[0])
                                        >>4)&0xf,
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[0])
                                        >>10)&0x3f,
                                ack&0x1f, (ack>>5)&0x3, 
                                le32_to_cpu(d->prg_cpu[d->sent_ind]->data[3])
                                        >>16,
                                d->ctx);
		else 
			DBGMSG(ohci->id,
			       "Packet sent to node %d tcode=0x%X tLabel="
			       "0x%02X ack=0x%X spd=%d data=0x%08X ctx=%d", 
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[1])
                                        >>16)&0x3f,
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[0])
                                        >>4)&0xf,
                                (le32_to_cpu(d->prg_cpu[d->sent_ind]->data[0])
                                        >>10)&0x3f,
                                ack&0x1f, (ack>>5)&0x3, 
                                le32_to_cpu(d->prg_cpu[d->sent_ind]->data[3]),
                                d->ctx);
#endif		

                nextpacket = packet->xnext;
		hpsb_packet_sent(ohci->host, packet, ack & 0xf);

		if (datasize) {
			pci_unmap_single(ohci->dev, 
					 cpu_to_le32(d->prg_cpu[d->sent_ind]->end.address),
					 datasize, PCI_DMA_TODEVICE);
			OHCI_DMA_FREE("single Xmit data packet");
		}

		d->sent_ind = (d->sent_ind+1)%d->num_desc;
		d->free_prgs++;
		d->fifo_first = nextpacket;
	}
	if (d->fifo_first == NULL)
		d->fifo_last = NULL;

	dma_trm_flush(ohci, d);

	spin_unlock_irqrestore(&d->lock, flags);
}

static int free_dma_rcv_ctx(struct dma_rcv_ctx **d)
{
	int i;
	struct ti_ohci *ohci;

	if (*d==NULL) return -1;

	ohci = (struct ti_ohci *)(*d)->ohci;

	DBGMSG(ohci->id, "Freeing dma_rcv_ctx %d",(*d)->ctx);
	
	ohci1394_stop_context(ohci, (*d)->ctrlClear, NULL);

	tasklet_kill(&(*d)->task);

	if ((*d)->buf_cpu) {
		for (i=0; i<(*d)->num_desc; i++)
			if ((*d)->buf_cpu[i] && (*d)->buf_bus[i]) {
				pci_free_consistent(
					ohci->dev, (*d)->buf_size, 
					(*d)->buf_cpu[i], (*d)->buf_bus[i]);
				OHCI_DMA_FREE("consistent dma_rcv buf[%d]", i);
			}
		kfree((*d)->buf_cpu);
		kfree((*d)->buf_bus);
	}
	if ((*d)->prg_cpu) {
		for (i=0; i<(*d)->num_desc; i++) 
			if ((*d)->prg_cpu[i] && (*d)->prg_bus[i]) {
				pci_free_consistent(
					ohci->dev, sizeof(struct dma_cmd), 
					(*d)->prg_cpu[i], (*d)->prg_bus[i]);
				OHCI_DMA_FREE("consistent dma_rcv prg[%d]", i);
			}
		kfree((*d)->prg_cpu);
		kfree((*d)->prg_bus);
	}
	if ((*d)->spb) kfree((*d)->spb);

	kfree(*d);
	*d = NULL;

	return 0;
}

static struct dma_rcv_ctx *
alloc_dma_rcv_ctx(struct ti_ohci *ohci, int ctx, int num_desc,
		  int buf_size, int split_buf_size, 
		  int ctrlSet, int ctrlClear, int cmdPtr)
{
	struct dma_rcv_ctx *d=NULL;
	int i;

	d = (struct dma_rcv_ctx *)kmalloc(sizeof(struct dma_rcv_ctx), 
					  GFP_KERNEL);

	if (d == NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate dma_rcv_ctx");
		return NULL;
	}

	memset (d, 0, sizeof (struct dma_rcv_ctx));

	d->ohci = (void *)ohci;
	d->ctx = ctx;

	d->num_desc = num_desc;
	d->buf_size = buf_size;
	d->split_buf_size = split_buf_size;
	d->ctrlSet = ctrlSet;
	d->ctrlClear = ctrlClear;
	d->cmdPtr = cmdPtr;

	d->buf_cpu = NULL;
	d->buf_bus = NULL;
	d->prg_cpu = NULL;
	d->prg_bus = NULL;
	d->spb = NULL;

	d->buf_cpu = kmalloc(d->num_desc * sizeof(quadlet_t*), GFP_KERNEL);
	d->buf_bus = kmalloc(d->num_desc * sizeof(dma_addr_t), GFP_KERNEL);

	if (d->buf_cpu == NULL || d->buf_bus == NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate dma buffer");
		free_dma_rcv_ctx(&d);
		return NULL;
	}
	memset(d->buf_cpu, 0, d->num_desc * sizeof(quadlet_t*));
	memset(d->buf_bus, 0, d->num_desc * sizeof(dma_addr_t));

	d->prg_cpu = kmalloc(d->num_desc * sizeof(struct dma_cmd*), 
			     GFP_KERNEL);
	d->prg_bus = kmalloc(d->num_desc * sizeof(dma_addr_t), GFP_KERNEL);

	if (d->prg_cpu == NULL || d->prg_bus == NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate dma prg");
		free_dma_rcv_ctx(&d);
		return NULL;
	}
	memset(d->prg_cpu, 0, d->num_desc * sizeof(struct dma_cmd*));
	memset(d->prg_bus, 0, d->num_desc * sizeof(dma_addr_t));

	d->spb = kmalloc(d->split_buf_size, GFP_KERNEL);

	if (d->spb == NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate split buffer");
		free_dma_rcv_ctx(&d);
		return NULL;
	}

	for (i=0; i<d->num_desc; i++) {
		d->buf_cpu[i] = pci_alloc_consistent(ohci->dev, 
						     d->buf_size,
						     d->buf_bus+i);
		OHCI_DMA_ALLOC("consistent dma_rcv buf[%d]", i);
		
		if (d->buf_cpu[i] != NULL) {
			memset(d->buf_cpu[i], 0, d->buf_size);
		} else {
			PRINT(KERN_ERR, ohci->id, 
			      "Failed to allocate dma buffer");
			free_dma_rcv_ctx(&d);
			return NULL;
		}

		
                d->prg_cpu[i] = pci_alloc_consistent(ohci->dev, 
						     sizeof(struct dma_cmd),
						     d->prg_bus+i);
		OHCI_DMA_ALLOC("consistent dma_rcv prg[%d]", i);

                if (d->prg_cpu[i] != NULL) {
                        memset(d->prg_cpu[i], 0, sizeof(struct dma_cmd));
		} else {
			PRINT(KERN_ERR, ohci->id, 
			      "Failed to allocate dma prg");
			free_dma_rcv_ctx(&d);
			return NULL;
		}
	}

        spin_lock_init(&d->lock);

	/* initialize tasklet */
	tasklet_init (&d->task, dma_rcv_tasklet, (unsigned long)d);

	return d;
}

static int free_dma_trm_ctx(struct dma_trm_ctx **d)
{
	struct ti_ohci *ohci;
	int i;

	if (*d==NULL) return -1;

	ohci = (struct ti_ohci *)(*d)->ohci;

	DBGMSG(ohci->id, "Freeing dma_trm_ctx %d",(*d)->ctx);

	ohci1394_stop_context(ohci, (*d)->ctrlClear, NULL);

	tasklet_kill(&(*d)->task);

	if ((*d)->prg_cpu) {
		for (i=0; i<(*d)->num_desc; i++) 
			if ((*d)->prg_cpu[i] && (*d)->prg_bus[i]) {
				pci_free_consistent(
					ohci->dev, sizeof(struct at_dma_prg), 
					(*d)->prg_cpu[i], (*d)->prg_bus[i]);
				OHCI_DMA_FREE("consistent dma_trm prg[%d]", i);
			}
		kfree((*d)->prg_cpu);
		kfree((*d)->prg_bus);
	}

	kfree(*d);
	*d = NULL;
	return 0;
}

static struct dma_trm_ctx *
alloc_dma_trm_ctx(struct ti_ohci *ohci, int ctx, int num_desc,
		  int ctrlSet, int ctrlClear, int cmdPtr)
{
	struct dma_trm_ctx *d=NULL;
	int i;

	d = (struct dma_trm_ctx *)kmalloc(sizeof(struct dma_trm_ctx), 
					  GFP_KERNEL);

	if (d==NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate dma_trm_ctx");
		return NULL;
	}

	memset (d, 0, sizeof (struct dma_trm_ctx));

	d->ohci = (void *)ohci;
	d->ctx = ctx;
	d->num_desc = num_desc;
	d->ctrlSet = ctrlSet;
	d->ctrlClear = ctrlClear;
	d->cmdPtr = cmdPtr;
	d->prg_cpu = NULL;
	d->prg_bus = NULL;

	d->prg_cpu = kmalloc(d->num_desc * sizeof(struct at_dma_prg*), 
			     GFP_KERNEL);
	d->prg_bus = kmalloc(d->num_desc * sizeof(dma_addr_t), GFP_KERNEL);

	if (d->prg_cpu == NULL || d->prg_bus == NULL) {
		PRINT(KERN_ERR, ohci->id, "Failed to allocate at dma prg");
		free_dma_trm_ctx(&d);
		return NULL;
	}
	memset(d->prg_cpu, 0, d->num_desc * sizeof(struct at_dma_prg*));
	memset(d->prg_bus, 0, d->num_desc * sizeof(dma_addr_t));

	for (i=0; i<d->num_desc; i++) {
                d->prg_cpu[i] = pci_alloc_consistent(ohci->dev, 
						     sizeof(struct at_dma_prg),
						     d->prg_bus+i);
		OHCI_DMA_ALLOC("consistent dma_trm prg[%d]", i);

                if (d->prg_cpu[i] != NULL) {
                        memset(d->prg_cpu[i], 0, sizeof(struct at_dma_prg));
		} else {
			PRINT(KERN_ERR, ohci->id, 
			      "Failed to allocate at dma prg");
			free_dma_trm_ctx(&d);
			return NULL;
		}
	}

        spin_lock_init(&d->lock);

        /* initialize bottom handler */
	tasklet_init (&d->task, dma_trm_tasklet, (unsigned long)d);

	return d;
}

static u16 ohci_crc16 (u32 *ptr, int length)
{
	int shift;
	u32 crc, sum, data;

	crc = 0;
	for (; length > 0; length--) {
		data = *ptr++;
		for (shift = 28; shift >= 0; shift -= 4) {
			sum = ((crc >> 12) ^ (data >> shift)) & 0x000f;
			crc = (crc << 4) ^ (sum << 12) ^ (sum << 5) ^ sum;
		}
		crc &= 0xffff;
	}
	return crc;
}

/* Config ROM macro implementation influenced by NetBSD OHCI driver */

struct config_rom_unit {
	u32 *start;
	u32 *refer;
	int length;
	int refunit;
};

struct config_rom_ptr {
	u32 *data;
	int unitnum;
	struct config_rom_unit unitdir[10];
};

#define cf_put_1quad(cr, q) (((cr)->data++)[0] = cpu_to_be32(q))

#define cf_put_4bytes(cr, b1, b2, b3, b4) \
	(((cr)->data++)[0] = cpu_to_be32(((b1) << 24) | ((b2) << 16) | ((b3) << 8) | (b4)))

#define cf_put_keyval(cr, key, val) (((cr)->data++)[0] = cpu_to_be32((key) << 24) | (val))

#define cf_put_crc16(cr, unit) \
	(*(cr)->unitdir[unit].start = cpu_to_be32(((cr)->unitdir[unit].length << 16) | \
	 ohci_crc16((cr)->unitdir[unit].start + 1, (cr)->unitdir[unit].length)))

#define cf_unit_begin(cr, unit)					\
do {								\
	if ((cr)->unitdir[unit].refer != NULL) {		\
		*(cr)->unitdir[unit].refer |=			\
			(cr)->data - (cr)->unitdir[unit].refer;	\
		cf_put_crc16(cr, (cr)->unitdir[unit].refunit);	\
        }							\
        (cr)->unitnum = (unit);					\
        (cr)->unitdir[unit].start = (cr)->data++;		\
} while (0)

#define cf_put_refer(cr, key, unit)			\
do {							\
	(cr)->unitdir[unit].refer = (cr)->data;		\
	(cr)->unitdir[unit].refunit = (cr)->unitnum;	\
	((cr)->data++)[0] = cpu_to_be32((key) << 24);		\
} while(0)

#define cf_unit_end(cr)						\
do {								\
	(cr)->unitdir[(cr)->unitnum].length = (cr)->data -	\
		((cr)->unitdir[(cr)->unitnum].start + 1);	\
	cf_put_crc16((cr), (cr)->unitnum);			\
} while(0)

static void ohci_init_config_rom(struct ti_ohci *ohci)
{
	struct config_rom_ptr cr;

	memset(&cr, 0, sizeof(cr));
	memset (ohci->csr_config_rom_cpu, 0, sizeof (ohci->csr_config_rom_cpu));

	cr.data = ohci->csr_config_rom_cpu;

	/* Bus info block */
	cf_unit_begin(&cr, 0);
	cf_put_1quad(&cr, reg_read(ohci, OHCI1394_BusID));
	cf_put_1quad(&cr, reg_read(ohci, OHCI1394_BusOptions));
	cf_put_1quad(&cr, reg_read(ohci, OHCI1394_GUIDHi));
	cf_put_1quad(&cr, reg_read(ohci, OHCI1394_GUIDLo));
	cf_unit_end(&cr);

	DBGMSG(ohci->id, "GUID: %08x:%08x\n", reg_read(ohci, OHCI1394_GUIDHi),
		reg_read(ohci, OHCI1394_GUIDLo));

	/* IEEE P1212 suggests the initial ROM header CRC should only
	 * cover the header itself (and not the entire ROM). Since we use
	 * this, then we can make our bus_info_len the same as the CRC
	 * length.  */
	ohci->csr_config_rom_cpu[0] |= cpu_to_be32(
		(be32_to_cpu(ohci->csr_config_rom_cpu[0]) & 0x00ff0000) << 8);
	reg_write(ohci, OHCI1394_ConfigROMhdr,
		  be32_to_cpu(ohci->csr_config_rom_cpu[0]));

	/* Root directory */
	cf_unit_begin(&cr, 1);
	cf_put_keyval(&cr, 0x03, 0x00005e);	/* Vendor ID */
	cf_put_refer(&cr, 0x81, 2);		/* Textual description unit */
	cf_put_keyval(&cr, 0x0c, 0x0083c0);	/* Node capabilities */
	cf_put_refer(&cr, 0xd1, 3);		/* IPv4 unit directory */
	cf_put_refer(&cr, 0xd1, 4);		/* IPv6 unit directory */
	/* NOTE: Add other unit referers here, and append at bottom */
	cf_unit_end(&cr);

	/* Textual description - "Linux 1394" */
	cf_unit_begin(&cr, 2);
	cf_put_keyval(&cr, 0, 0);
	cf_put_1quad(&cr, 0);
	cf_put_4bytes(&cr, 'L', 'i', 'n', 'u');
	cf_put_4bytes(&cr, 'x', ' ', '1', '3');
	cf_put_4bytes(&cr, '9', '4', 0x0, 0x0);
	cf_unit_end(&cr);

	/* IPv4 unit directory, RFC 2734 */
	cf_unit_begin(&cr, 3);
	cf_put_keyval(&cr, 0x12, 0x00005e);	/* Unit spec ID */
	cf_put_refer(&cr, 0x81, 6);		/* Textual description unit */
	cf_put_keyval(&cr, 0x13, 0x000001);	/* Unit software version */
	cf_put_refer(&cr, 0x81, 7);		/* Textual description unit */
	cf_unit_end(&cr);

	cf_unit_begin(&cr, 6);
	cf_put_keyval(&cr, 0, 0);
	cf_put_1quad(&cr, 0);
	cf_put_4bytes(&cr, 'I', 'A', 'N', 'A');
	cf_unit_end(&cr);

	cf_unit_begin(&cr, 7);
	cf_put_keyval(&cr, 0, 0);
	cf_put_1quad(&cr, 0);
	cf_put_4bytes(&cr, 'I', 'P', 'v', '4');
	cf_unit_end(&cr);

	/* IPv6 unit directory, draft-ietf-ipngwg-1394-01.txt */
	cf_unit_begin(&cr, 4);
	cf_put_keyval(&cr, 0x12, 0x00005e);	/* Unit spec ID */
	cf_put_refer(&cr, 0x81, 8);		/* Textual description unit */
	cf_put_keyval(&cr, 0x13, 0x000002);	/* (Proposed) Unit software version */
	cf_put_refer(&cr, 0x81, 9);		/* Textual description unit */
	cf_unit_end(&cr);

	cf_unit_begin(&cr, 8);
	cf_put_keyval(&cr, 0, 0);
	cf_put_1quad(&cr, 0);
	cf_put_4bytes(&cr, 'I', 'A', 'N', 'A');
	cf_unit_end(&cr);

	cf_unit_begin(&cr, 9);
	cf_put_keyval(&cr, 0, 0);
	cf_put_1quad(&cr, 0);
	cf_put_4bytes(&cr, 'I', 'P', 'v', '6');
	cf_unit_end(&cr);

	return;
}

static size_t get_ohci_rom(struct hpsb_host *host, const quadlet_t **ptr)
{
	struct ti_ohci *ohci=host->hostdata;

	DBGMSG(ohci->id, "request csr_rom address: %p",
		ohci->csr_config_rom_cpu);

	*ptr = ohci->csr_config_rom_cpu;

	return sizeof(ohci->csr_config_rom_cpu);
}

int ohci_compare_swap(struct ti_ohci *ohci, quadlet_t *data,
                      quadlet_t compare, int sel)
{
	int i;
	reg_write(ohci, OHCI1394_CSRData, *data);
	reg_write(ohci, OHCI1394_CSRCompareData, compare);
	reg_write(ohci, OHCI1394_CSRControl, sel & 0x3);

	for (i = 0; i < OHCI_LOOP_COUNT; i++) {
		if (reg_read(ohci, OHCI1394_CSRControl) & 0x80000000)
			break;

		mdelay(10);
	}

	*data = reg_read(ohci, OHCI1394_CSRData);
	return 0;
}

static quadlet_t ohci_hw_csr_reg(struct hpsb_host *host, int reg,
                                 quadlet_t data, quadlet_t compare)
{
	struct ti_ohci *ohci=host->hostdata;

	ohci_compare_swap (ohci, &data, compare, reg);

	return data;
}

struct hpsb_host_template *get_ohci_template(void)
{
	static struct hpsb_host_template tmpl;
	static int initialized = 0;

	if (!initialized) {
		memset (&tmpl, 0, sizeof (struct hpsb_host_template));

		/* Initialize by field names so that a template structure
		 * reorganization does not influence this code. */
		tmpl.name = "ohci1394";

		tmpl.initialize_host = ohci_initialize;
		tmpl.release_host = ohci_remove;
		tmpl.get_rom = get_ohci_rom;
		tmpl.transmit_packet = ohci_transmit;
		tmpl.devctl = ohci_devctl;
		tmpl.hw_csr_reg = ohci_hw_csr_reg;
		initialized = 1;
	}

	return &tmpl;
}

static int __devinit ohci1394_add_one(struct pci_dev *dev, const struct pci_device_id *ent)
{
	struct ti_ohci *ohci;	/* shortcut to currently handled device */
	struct hpsb_host *host;
	unsigned long ohci_base, ohci_len;
	static int version_printed = 0;

	if (version_printed++ == 0)
		PRINT_G(KERN_INFO, "%s", version);

        if (pci_enable_device(dev)) {
		/* Skip ID's that fail */
		PRINT_G(KERN_NOTICE, "Failed to enable OHCI hardware %d",
			card_id_counter++);
		return -ENXIO;
        }
        pci_set_master(dev);

	host = hpsb_get_host(get_ohci_template(), sizeof (struct ti_ohci));
	if (!host) {
		PRINT_G(KERN_ERR, "Out of memory trying to allocate host structure");
		return -ENOMEM;
	}
	ohci = host->hostdata;
	ohci->host = host;
	INIT_LIST_HEAD(&ohci->list);
	ohci->id = card_id_counter++;
	ohci->dev = dev;
	host->pdev = dev;
	ohci->host = host;
	pci_set_drvdata(dev, ohci);

	PRINT(KERN_INFO, ohci->id, "OHCI (PCI) IEEE-1394 Controller");

	/* We don't want hardware swapping */
	pci_write_config_dword(dev, OHCI1394_PCI_HCI_Control, 0);

	/* Some oddball Apple controllers do not order the selfid
	 * properly, so we make up for it here.  */
#ifndef __LITTLE_ENDIAN
	/* XXX: Need a better way to check this. I'm wondering if we can
	 * read the values of the OHCI1394_PCI_HCI_Control and the
	 * noByteSwapData registers to see if they were not cleared to
	 * zero. Should this work? Obviously it's not defined what these
	 * registers will read when they aren't supported. Bleh! */
	if (dev->vendor == PCI_VENDOR_ID_APPLE) {
		ohci->payload_swap = 1;
		if (dev->device != PCI_DEVICE_ID_APPLE_UNI_N_FW)
			ohci->selfid_swap = 1;
	} else
		ohci->selfid_swap = 1;
#endif

	/* csr_config rom allocation */
	ohci->csr_config_rom_cpu = 
		pci_alloc_consistent(ohci->dev, OHCI_CONFIG_ROM_LEN,
				     &ohci->csr_config_rom_bus);
	OHCI_DMA_ALLOC("consistent csr_config_rom");
	if (ohci->csr_config_rom_cpu == NULL)
		FAIL("Failed to allocate buffer config rom");

	/* 
	 * self-id dma buffer allocation
	 */
	ohci->selfid_buf_cpu = 
		pci_alloc_consistent(ohci->dev, OHCI1394_SI_DMA_BUF_SIZE,
                      &ohci->selfid_buf_bus);
	OHCI_DMA_ALLOC("consistent selfid_buf");
	if (ohci->selfid_buf_cpu == NULL)
		FAIL("Failed to allocate DMA buffer for self-id packets");

	if ((unsigned long)ohci->selfid_buf_cpu & 0x1fff)
		PRINT(KERN_INFO, ohci->id, "SelfID buffer %p is not aligned on "
		      "8Kb boundary... may cause problems on some CXD3222 chip", 
		      ohci->selfid_buf_cpu);  

	ohci->it_context =
		alloc_dma_trm_ctx(ohci, 2, IT_NUM_DESC,
				  OHCI1394_IsoXmitContextControlSet,
				  OHCI1394_IsoXmitContextControlClear,
				  OHCI1394_IsoXmitCommandPtr);

	if (ohci->it_context == NULL)
		FAIL("Failed to allocate IT context");

	ohci_base = pci_resource_start(dev, 0);
	ohci_len = pci_resource_len(dev, 0);

	if (!request_mem_region (ohci_base, ohci_len, host->template->name))
		FAIL("MMIO resource (0x%lx@0x%lx) unavailable, aborting.",
		     ohci_base, ohci_len);

	ohci->registers = ioremap(ohci_base, ohci_len);

	if (ohci->registers == NULL)
		FAIL("Failed to remap registers - card not accessible");

	DBGMSG(ohci->id, "Remapped memory spaces reg 0x%p",
	      ohci->registers);

	ohci->ar_req_context = 
		alloc_dma_rcv_ctx(ohci, 0, AR_REQ_NUM_DESC,
				  AR_REQ_BUF_SIZE, AR_REQ_SPLIT_BUF_SIZE,
				  OHCI1394_AsReqRcvContextControlSet,
				  OHCI1394_AsReqRcvContextControlClear,
				  OHCI1394_AsReqRcvCommandPtr);

	if (ohci->ar_req_context == NULL)
		FAIL("Failed to allocate AR Req context");

	ohci->ar_resp_context = 
		alloc_dma_rcv_ctx(ohci, 1, AR_RESP_NUM_DESC,
				  AR_RESP_BUF_SIZE, AR_RESP_SPLIT_BUF_SIZE,
				  OHCI1394_AsRspRcvContextControlSet,
				  OHCI1394_AsRspRcvContextControlClear,
				  OHCI1394_AsRspRcvCommandPtr);
	
	if (ohci->ar_resp_context == NULL)
		FAIL("Failed to allocate AR Resp context");

	ohci->at_req_context = 
		alloc_dma_trm_ctx(ohci, 0, AT_REQ_NUM_DESC,
				  OHCI1394_AsReqTrContextControlSet,
				  OHCI1394_AsReqTrContextControlClear,
				  OHCI1394_AsReqTrCommandPtr);
	
	if (ohci->at_req_context == NULL)
		FAIL("Failed to allocate AT Req context");

	ohci->at_resp_context = 
		alloc_dma_trm_ctx(ohci, 1, AT_RESP_NUM_DESC,
				  OHCI1394_AsRspTrContextControlSet,
				  OHCI1394_AsRspTrContextControlClear,
				  OHCI1394_AsRspTrCommandPtr);
	
	if (ohci->at_resp_context == NULL)
		FAIL("Failed to allocate AT Resp context");

	ohci->ir_context =
		alloc_dma_rcv_ctx(ohci, 2, IR_NUM_DESC,
				  IR_BUF_SIZE, IR_SPLIT_BUF_SIZE,
				  OHCI1394_IsoRcvContextControlSet,
				  OHCI1394_IsoRcvContextControlClear,
				  OHCI1394_IsoRcvCommandPtr);

	if (ohci->ir_context == NULL)
		FAIL("Failed to allocate IR context");

	ohci->ISO_channel_usage = 0;
        spin_lock_init(&ohci->IR_channel_lock);

	if (!request_irq(dev->irq, ohci_irq_handler, SA_SHIRQ,
			 OHCI1394_DRIVER_NAME, ohci))
		PRINT(KERN_DEBUG, ohci->id, "Allocated interrupt %d", dev->irq);
	else
		FAIL("Failed to allocate shared interrupt %d", dev->irq);

	ohci_init_config_rom(ohci);

	/* Tell the highlevel this host is ready */
	highlevel_add_one_host (host);

	return 0;
#undef FAIL
}

static void remove_card(struct ti_ohci *ohci)
{
	/* Reset the board properly before leaving */
	ohci_soft_reset(ohci);

	/* Free AR dma */
	free_dma_rcv_ctx(&ohci->ar_req_context);
	free_dma_rcv_ctx(&ohci->ar_resp_context);

	/* Free AT dma */
	free_dma_trm_ctx(&ohci->at_req_context);
	free_dma_trm_ctx(&ohci->at_resp_context);

	/* Free IR dma */
	free_dma_rcv_ctx(&ohci->ir_context);

        /* Free IT dma */
        free_dma_trm_ctx(&ohci->it_context);

	/* Free self-id buffer */
	if (ohci->selfid_buf_cpu) {
		pci_free_consistent(ohci->dev, OHCI1394_SI_DMA_BUF_SIZE, 
				    ohci->selfid_buf_cpu,
				    ohci->selfid_buf_bus);
		OHCI_DMA_FREE("consistent selfid_buf");
	}
	
	/* Free config rom */
	if (ohci->csr_config_rom_cpu) {
		pci_free_consistent(ohci->dev, OHCI_CONFIG_ROM_LEN,
				    ohci->csr_config_rom_cpu, 
				    ohci->csr_config_rom_bus);
		OHCI_DMA_FREE("consistent csr_config_rom");
	}
	
	/* Free the IRQ */
	free_irq(ohci->dev->irq, ohci);

	if (ohci->registers)
		iounmap(ohci->registers);

	release_mem_region (pci_resource_start(ohci->dev, 0),
			    pci_resource_len(ohci->dev, 0));

	pci_set_drvdata(ohci->dev, NULL);
}

void ohci1394_stop_context(struct ti_ohci *ohci, int reg, char *msg)
{
	int i=0;

	/* stop the channel program if it's still running */
	reg_write(ohci, reg, 0x8000);
   
	/* Wait until it effectively stops */
	while (reg_read(ohci, reg) & 0x400) {
		i++;
		if (i>5000) {
			PRINT(KERN_ERR, ohci->id, 
			      "Runaway loop while stopping context...");
			break;
		}
	}
	if (msg) PRINT(KERN_ERR, ohci->id, "%s: dma prg stopped", msg);
}

int ohci1394_register_video(struct ti_ohci *ohci,
			    struct video_template *tmpl)
{
	if (ohci->video_tmpl)
		return -ENFILE;
	ohci->video_tmpl = tmpl;
	MOD_INC_USE_COUNT;
	return 0;
}
	
void ohci1394_unregister_video(struct ti_ohci *ohci,
			       struct video_template *tmpl)
{
	if (ohci->video_tmpl != tmpl) {
		PRINT(KERN_ERR, ohci->id, 
		      "Trying to unregister wrong video device");
	} else {
		ohci->video_tmpl = NULL;
		MOD_DEC_USE_COUNT;
	}
}

#ifndef __LITTLE_ENDIAN

/* Swap a series of quads inplace. */
static __inline__ void block_swab32(quadlet_t *data, size_t size) {
	while (size--)
		data[size] = swab32(data[size]);
}

/* Swap headers and sometimes data too */
static void packet_swab(quadlet_t *data, char tcode, int len, int payload_swap)
{
	if (payload_swap) {
		block_swab32(data, len);
		return;
	}

        switch(tcode)
        {
		/* 4 quad header */
		case TCODE_READB_RESPONSE:
		case TCODE_LOCK_RESPONSE:
		case TCODE_LOCK_REQUEST:
		case TCODE_WRITEB:
		case TCODE_READB:
			block_swab32(data, 4);
			break;

		/* 3 quad header, 1 quad payload */
		case TCODE_WRITEQ:
		case TCODE_READQ_RESPONSE:
			block_swab32(data, 3);
			break;

		/* 3 quad header */
		case TCODE_WRITE_RESPONSE:
		case TCODE_READQ:
			block_swab32(data, 3);
			break;

		/* 2 quad header */
		case TCODE_ISO_DATA:
			block_swab32(data, 2);
			break;

		case OHCI1394_TCODE_PHY:
			break; /* should never happen anyway */

		case TCODE_CYCLE_START:
			PRINT_G(KERN_ERR, "Unhandled tcode in packet_swab (0x%x)", tcode);
			/* Atleast swap one quad */
			block_swab32(data, 1);
			break;
                default:
			PRINT_G(KERN_ERR, "Invalid tcode in packet_swab (0x%x)\n", tcode);
                        break;
        }
	return;
}

#endif /* !LITTLE_ENDIAN */


#if 0
int ohci1394_request_channel(struct ti_ohci *ohci, int channel)
{
	int csrSel;
	quadlet_t chan, data1=0, data2=0;
	int timeout = 32;

	if (channel<32) {
		chan = 1<<channel;
		csrSel = 2;
	}
	else {
		chan = 1<<(channel-32);
		csrSel = 3;
	}
	if (ohci_compare_swap(ohci, &data1, 0, csrSel)<0) {
		PRINT(KERN_INFO, ohci->id, "request_channel timeout");
		return -1;
	}
	while (timeout--) {
		if (data1 & chan) {
			PRINT(KERN_INFO, ohci->id, 
			      "request channel %d failed", channel);
			return -1;
		}
		data2 = data1;
		data1 |= chan;
		if (ohci_compare_swap(ohci, &data1, data2, csrSel)<0) {
			PRINT(KERN_INFO, ohci->id, "request_channel timeout");
			return -1;
		}
		if (data1==data2) {
			PRINT(KERN_INFO, ohci->id, 
			      "request channel %d succeded", channel);
			return 0;
		}
	}
	PRINT(KERN_INFO, ohci->id, "request channel %d failed", channel);
	return -1;
}
#endif

EXPORT_SYMBOL(ohci1394_stop_context);
EXPORT_SYMBOL(ohci1394_register_video);
EXPORT_SYMBOL(ohci1394_unregister_video);

MODULE_AUTHOR("Sebastien Rougeaux <sebastien.rougeaux@anu.edu.au>");
MODULE_DESCRIPTION("Driver for PCI OHCI IEEE-1394 controllers");

static void __devexit ohci1394_remove_one(struct pci_dev *pdev)
{
	struct ti_ohci *ohci = pci_get_drvdata(pdev);

	if (ohci) {
		remove_card (ohci);
		pci_set_drvdata(pdev, NULL);
	}
}

static struct pci_driver ohci1394_driver = {
	name:		"ohci1394",
	id_table:	ohci1394_pci_tbl,
	probe:		ohci1394_add_one,
	remove:		ohci1394_remove_one,
};

static void __exit ohci1394_cleanup (void)
{
	hpsb_unregister_lowlevel(get_ohci_template());
	pci_unregister_driver(&ohci1394_driver);
}

static int __init ohci1394_init(void)
{
	int ret;
	if (hpsb_register_lowlevel(get_ohci_template())) {
		PRINT_G(KERN_ERR, "Registering failed");
		return -ENXIO;
	}
	if ((ret = pci_module_init(&ohci1394_driver))) {
		PRINT_G(KERN_ERR, "PCI module init failed\n");
		hpsb_unregister_lowlevel(get_ohci_template());
		return ret;
	}
	return ret;
}

module_init(ohci1394_init);
module_exit(ohci1394_cleanup);
