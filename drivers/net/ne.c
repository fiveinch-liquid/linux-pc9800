/* ne.c: A general non-shared-memory NS8390 ethernet driver for linux. */
/*
    Written 1992-94 by Donald Becker.

    Copyright 1993 United States Government as represented by the
    Director, National Security Agency.

    This software may be used and distributed according to the terms
    of the GNU General Public License, incorporated herein by reference.

    The author may be reached as becker@scyld.com, or C/O
    Scyld Computing Corporation, 410 Severn Ave., Suite 210, Annapolis MD 21403

    This driver should work with many programmed-I/O 8390-based ethernet
    boards.  Currently it supports the NE1000, NE2000, many clones,
    and some Cabletron products.

    Changelog:

    Paul Gortmaker	: use ENISR_RDC to monitor Tx PIO uploads, made
			  sanity checks and bad clone support optional.
    Paul Gortmaker	: new reset code, reset card after probe at boot.
    Paul Gortmaker	: multiple card support for module users.
    Paul Gortmaker	: Support for PCI ne2k clones, similar to lance.c
    Paul Gortmaker	: Allow users with bad cards to avoid full probe.
    Paul Gortmaker	: PCI probe changes, more PCI cards supported.
    rjohnson@analogic.com : Changed init order so an interrupt will only
    occur after memory is allocated for dev->priv. Deallocated memory
    last in cleanup_modue()
    Richard Guenther    : Added support for ISAPnP cards
    Paul Gortmaker	: Discontinued PCI support - use ne2k-pci.c instead.

*/

/* Routines for the NatSemi-based designs (NE[12]000). */

static const char version1[] =
"ne.c:v1.10 9/23/94 Donald Becker (becker@scyld.com)\n";
static const char version2[] =
"Last modified Nov 1, 2000 by Paul Gortmaker\n";


#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/isapnp.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <asm/system.h>
#include <asm/io.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include "8390.h"

/* backword compatibility for kernel version 2.1.57 */
#include <linux/version.h>
#ifndef LINUX_VERSION_CODE
#warning LINUX_VERSION_CODE is no defined!
#endif
#if LINUX_VERSION_CODE < 0x20200
#ifdef CONFIG_PCI
#undef CONFIG_PCI
#endif
#ifdef CONFIG_PC98
#define CONFIG_PC9800
#endif
#define mdelay(n) ({unsigned long msec=(n); while (msec--) udelay(1000);})
#endif

#ifdef CONFIG_NET_CBUS
#undef ei_debug
#define ei_debug 9
#endif /* CONFIG_NET_CBUS */

/* Some defines that people can play with if so inclined. */

/* Do we support clones that don't adhere to 14,15 of the SAprom ? */
#define SUPPORT_NE_BAD_CLONES

/* Do we perform extra sanity checks on stuff ? */
/* #define NE_SANITY_CHECK */

/* Do we implement the read before write bugfix ? */
/* #define NE_RW_BUGFIX */

/* Do we have a non std. amount of memory? (in units of 256 byte pages) */
/* #define PACKETBUF_MEMSIZE	0x40 */

/* A zero-terminated list of I/O addresses to be probed at boot. */
#ifndef CONFIG_NET_CBUS
#ifndef MODULE
static unsigned int netcard_portlist[] __initdata = {
	0x300, 0x280, 0x320, 0x340, 0x360, 0x380, 0
};
#endif
#endif

static struct isapnp_device_id isapnp_clone_list[] __initdata = {
	{	ISAPNP_ANY_ID, ISAPNP_ANY_ID,
		ISAPNP_VENDOR('E','D','I'), ISAPNP_FUNCTION(0x0216),
		(long) "NN NE2000" },
	{	ISAPNP_ANY_ID, ISAPNP_ANY_ID,
		ISAPNP_VENDOR('P','N','P'), ISAPNP_FUNCTION(0x80d6),
		(long) "Generic PNP" },
	{ }	/* terminate list */
};

MODULE_DEVICE_TABLE(isapnp, isapnp_clone_list);

#ifdef SUPPORT_NE_BAD_CLONES
/* A list of bad clones that we none-the-less recognize. */
static struct { const char *name8, *name16; unsigned char SAprefix[4];}
bad_clone_list[] __initdata = {
#ifndef CONFIG_NET_CBUS
    {"DE100", "DE200", {0x00, 0xDE, 0x01,}},
    {"DE120", "DE220", {0x00, 0x80, 0xc8,}},
    {"DFI1000", "DFI2000", {'D', 'F', 'I',}}, /* Original, eh?  */
    {"EtherNext UTP8", "EtherNext UTP16", {0x00, 0x00, 0x79}},
    {"NE1000","NE2000-invalid", {0x00, 0x00, 0xd8}}, /* Ancient real NE1000. */
    {"NN1000", "NN2000",  {0x08, 0x03, 0x08}}, /* Outlaw no-name clone. */
    {"4-DIM8","4-DIM16", {0x00,0x00,0x4d,}},  /* Outlaw 4-Dimension cards. */
    {"Con-Intl_8", "Con-Intl_16", {0x00, 0x00, 0x24}}, /* Connect Int'nl */
    {"ET-100","ET-200", {0x00, 0x45, 0x54}}, /* YANG and YA clone */
    {"COMPEX","COMPEX16",{0x00,0x80,0x48}}, /* Broken ISA Compex cards */
    {"E-LAN100", "E-LAN200", {0x00, 0x00, 0x5d}}, /* Broken ne1000 clones */
    {"PCM-4823", "PCM-4823", {0x00, 0xc0, 0x6c}}, /* Broken Advantech MoBo */
    {"REALTEK", "RTL8019", {0x00, 0x00, 0xe8}}, /* no-name with Realtek chip */
    {"LCS-8834", "LCS-8836", {0x04, 0x04, 0x37}}, /* ShinyNet (SET) */
#else /* CONFIG_NET_CBUS */
    {"LA/T-98?","LA/T-98", {0x00,0xa0,0xb0}},  /* I/O Data */
    {"EGY-98?","EGY-98", {0x00,0x40,0x26}}, /* Melco EGY98 */
    {"ICM?","ICM-27xx-ET", {0x00,0x80,0xc8}}, /* ICM IF-27xx-ET */
    {"CNET-98/EL?","CNET(98)E/L", {0x00,0x80,0x4C}}, /* Contec CNET-98/EL */
#endif
    {0,}
};
#endif

/* ---- No user-serviceable parts below ---- */

#define NE_SHIFT(x) EI_SHIFT(x)
#define NE_BASE	 (dev->base_addr)
#define NE_CMD	 	NE_SHIFT(0x00)
#define NE_DATAPORT	NE_SHIFT(0x10)	/* NatSemi-defined port window offset. */
#ifndef CONFIG_NET_CBUS
#define NE_RESET	NE_SHIFT(0x1f) /* Issue a read to reset, a write to clear. */
#else
#define NE_RESET	NE_SHIFT(0x11) /* Issue a read to reset, a write to clear. */
#endif

#define NE_IO_EXTENT	0x20

#define NE1SM_START_PG	0x20	/* First page of TX buffer */
#define NE1SM_STOP_PG 	0x40	/* Last page +1 of RX ring */
#define NESM_START_PG	0x40	/* First page of TX buffer */
#define NESM_STOP_PG	0x80	/* Last page +1 of RX ring */
#ifdef CONFIG_NE2K_CBUS_CNET98EL
#ifndef CONFIG_NE2K_CBUS_CNET98EL_IO_BASE
#warning CONFIG_NE2K_CBUS_CNET98EL_IO_BASE is not defined(config error?)
#warning use 0xaaed as default
#define CONFIG_NE2K_CBUS_CNET98EL_IO_BASE 0xaaed /* or 0x55ed */
#endif
#define CNET98EL_START_PG 0x00
#define CNET98EL_STOP_PG 0x40
#endif

#ifdef CONFIG_NET_CBUS
#include "ne2k_cbus.h"
#endif

int ne_probe(struct net_device *dev);
static int ne_probe1(struct net_device *dev, int ioaddr);
static int ne_probe_isapnp(struct net_device *dev);
#ifdef CONFIG_NET_CBUS
static int ne_probe_cbus(struct net_device *dev, const struct ne2k_cbus_hwinfo *hw, int ioaddr);
#endif

static int ne_open(struct net_device *dev);
static int ne_close(struct net_device *dev);

static void ne_reset_8390(struct net_device *dev);
static void ne_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr,
			  int ring_page);
static void ne_block_input(struct net_device *dev, int count,
			  struct sk_buff *skb, int ring_offset);
static void ne_block_output(struct net_device *dev, const int count,
		const unsigned char *buf, const int start_page);


/*  Probe for various non-shared-memory ethercards.

   NEx000-clone boards have a Station Address PROM (SAPROM) in the packet
   buffer memory space.  NE2000 clones have 0x57,0x57 in bytes 0x0e,0x0f of
   the SAPROM, while other supposed NE2000 clones must be detected by their
   SA prefix.

   Reading the SAPROM from a word-wide card with the 8390 set in byte-wide
   mode results in doubled values, which can be detected and compensated for.

   The probe is also responsible for initializing the card and filling
   in the 'dev' and 'ei_status' structures.

   We use the minimum memory size for some ethercard product lines, iff we can't
   distinguish models.  You can increase the packet buffer size by setting
   PACKETBUF_MEMSIZE.  Reported Cabletron packet buffer locations are:
	E1010   starts at 0x100 and ends at 0x2000.
	E1010-x starts at 0x100 and ends at 0x8000. ("-x" means "more memory")
	E2010	 starts at 0x100 and ends at 0x4000.
	E2010-x starts at 0x100 and ends at 0xffff.  */

#ifndef CONFIG_NET_CBUS

int __init ne_probe(struct net_device *dev)
{
	unsigned int base_addr = dev->base_addr;

	SET_MODULE_OWNER(dev);

	/* First check any supplied i/o locations. User knows best. <cough> */
	if (base_addr > 0x1ff)	/* Check a single specified location. */
		return ne_probe1(dev, base_addr);
	else if (base_addr != 0)	/* Don't probe at all. */
		return -ENXIO;

	/* Then look for any installed ISAPnP clones */
	if (isapnp_present() && (ne_probe_isapnp(dev) == 0))
		return 0;

#ifndef MODULE
	/* Last resort. The semi-risky ISA auto-probe. */
	for (base_addr = 0; netcard_portlist[base_addr] != 0; base_addr++) {
		int ioaddr = netcard_portlist[base_addr];
		if (ne_probe1(dev, ioaddr) == 0)
			return 0;
	}
#endif

	return -ENODEV;
}

#else /* CONFIG_NET_CBUS */

int __init ne_probe(struct net_device *dev)
{
	unsigned int base_addr = dev->base_addr;

	SET_MODULE_OWNER(dev);

	if(ei_debug > 2)
		printk(KERN_DEBUG "ne_probe(): entered.\n");

	/* If CONFIG_NET_CBUS,
	   we need dev->priv->reg_offset BEFORE to probe */
	if(ne2k_cbus_init(dev)!=0){
		return -ENOMEM;
	}

	/* First check any supplied i/o locations. User knows best. <cough> */
	if(base_addr > 0) {
		int result;
		const struct ne2k_cbus_hwinfo *hw = ne2k_cbus_get_hwinfo((int)(dev->mem_start & NE2K_CBUS_HARDWARE_TYPE_MASK));

		if(ei_debug > 2)
			printk(KERN_DEBUG "ne_probe(): call ne_probe_cbus(base_addr=0x%x)\n",base_addr);

		result = ne_probe_cbus(dev, hw, base_addr);
		if (result != 0)
			ne2k_cbus_destroy(dev);

		return result;
	}

	if(ei_debug > 2)
		printk(KERN_DEBUG "ne_probe(): base_addr is not specified.\n");

#ifndef MODULE
	/* Last resort. The semi-risky C-Bus auto-probe. */
	if(ei_debug > 2)
		printk(KERN_DEBUG "ne_probe(): auto-probe start.\n");

	{
		const struct ne2k_cbus_hwinfo *hw = ne2k_cbus_get_hwinfo((int)(dev->mem_start & NE2K_CBUS_HARDWARE_TYPE_MASK));

		if(hw&&hw->hwtype){
			const unsigned short *plist;
			for(plist=hw->portlist; *plist; plist++){
				const struct ne2k_cbus_region *rlist;
				for(rlist = hw->regionlist; rlist->range; rlist++){
					if (check_region(*plist+rlist->start, rlist->range))
						break;
				}
				if(rlist->range){
					/* check_region() failed */ 
					continue; /* try next base port */
				}
				/* check_region() succeeded */
				if (ne_probe_cbus(dev,hw,*plist) == 0)
					return 0;
			}
		}else{
			for(hw = &ne2k_cbus_hwinfo_list[0]; hw->hwtype; hw++) {
				const unsigned short *plist;
				for(plist=hw->portlist; *plist; plist++){
					const struct ne2k_cbus_region *rlist;

					for(rlist = hw->regionlist; rlist->range; rlist++){
						if (check_region(*plist+rlist->start, rlist->range))
							break;
					}
					if(rlist->range){
						/* check_region() failed */ 
						continue; /* try next base port */
					}
					/* check_region() succeeded */
					if (ne_probe_cbus(dev,hw,*plist) == 0)
						return 0;
				}
			}
		}
	}
#endif

	ne2k_cbus_destroy(dev);

	return -ENODEV;
}

#endif /* CONFIG_NET_CBUS */

static int __init ne_probe_isapnp(struct net_device *dev)
{
	int i;

	for (i = 0; isapnp_clone_list[i].vendor != 0; i++) {
		struct pci_dev *idev = NULL;

		while ((idev = isapnp_find_dev(NULL,
					       isapnp_clone_list[i].vendor,
					       isapnp_clone_list[i].function,
					       idev))) {
			/* Avoid already found cards from previous calls */
			if (idev->prepare(idev))
				continue;
			if (idev->activate(idev))
				continue;
			/* if no irq, search for next */
			if (idev->irq_resource[0].start == 0)
				continue;
			/* found it */
			dev->base_addr = idev->resource[0].start;
			dev->irq = idev->irq_resource[0].start;
			printk(KERN_INFO "ne.c: ISAPnP reports %s at i/o %#lx, irq %d.\n",
				(char *) isapnp_clone_list[i].driver_data,

				dev->base_addr, dev->irq);
			if (ne_probe1(dev, dev->base_addr) != 0) {	/* Shouldn't happen. */
				printk(KERN_ERR "ne.c: Probe of ISAPnP card at %#lx failed.\n", dev->base_addr);
				return -ENXIO;
			}
			ei_status.priv = (unsigned long)idev;
			break;
		}
		if (!idev)
			continue;
		return 0;
	}

	return -ENODEV;
}

#ifdef CONFIG_NET_CBUS
static int __init ne_probe_cbus(struct net_device *dev, const struct ne2k_cbus_hwinfo *hw, int ioaddr)
{
	if(ei_debug > 2)
		printk(KERN_DEBUG "ne_probe_cbus(): entered. (called from %p)\n",
		       __builtin_return_address (0));

	if(hw && hw->hwtype){
		ne2k_cbus_set_hwtype(dev,hw,ioaddr);
		return ne_probe1(dev,ioaddr);
	}else{
		/* auto detect */

		printk(KERN_DEBUG "ne_probe_cbus(): try to determine hardware types.\n");
		for(hw=&ne2k_cbus_hwinfo_list[0]; hw->hwtype; hw++){
			ne2k_cbus_set_hwtype(dev,hw,ioaddr);
			if(ne_probe1(dev,ioaddr)==0)
				return 0;
		}
	}
	return ENODEV;
}
#endif /* CONFIG_NET_CBUS */

static int __init ne_probe1(struct net_device *dev, int ioaddr)
{
	int i;
	unsigned char SA_prom[32];
#ifndef CONFIG_NET_CBUS /* if CONFIG_NET_CBUS, wordlength is always 2! */
	int wordlength = 2;
#endif
	const char *name = NULL;
	int start_page, stop_page;
#ifndef CONFIG_NET_CBUS
	int neX000, ctron, copam, bad_card;
#else
	int neX000, bad_card;
#endif
	int reg0, ret;
	static unsigned version_printed;
#ifdef CONFIG_NET_CBUS
	const struct ne2k_cbus_hwinfo *hw = ne2k_cbus_get_hwinfo((int)(dev->mem_start & NE2K_CBUS_HARDWARE_TYPE_MASK));
	struct ei_device *ei_local = (struct ei_device *)(dev->priv);
#endif

#ifdef CONFIG_NET_CBUS
	if(ei_debug > 2){
		printk(KERN_DEBUG "ne_probe1(): entered\n"
			   "ioaddr=0x%x, hardware_type = %d(%s)\n"
			   "ei_local->reg_offset = \n"
			   "{ 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, \n"
			   "  0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, 0x%04x, \n"
			   "  0x%04x, 0x%04x }\n",
			   ioaddr, hw->hwtype, hw->hwident,
			   ei_local->reg_offset[0] , ei_local->reg_offset[1],
			   ei_local->reg_offset[2] , ei_local->reg_offset[3],
			   ei_local->reg_offset[4] , ei_local->reg_offset[5],
			   ei_local->reg_offset[6] , ei_local->reg_offset[7],
			   ei_local->reg_offset[8] , ei_local->reg_offset[9],
			   ei_local->reg_offset[10], ei_local->reg_offset[11],
			   ei_local->reg_offset[12], ei_local->reg_offset[13],
			   ei_local->reg_offset[14], ei_local->reg_offset[15],
			   ei_local->reg_offset[16], ei_local->reg_offset[17]);
	}
#endif

#ifdef CONFIG_NE2K_CBUS_CNET98EL
	if(hw->hwtype==NE2K_CBUS_HARDWARE_TYPE_CNET98EL){
		outb_p( 0, CONFIG_NE2K_CBUS_CNET98EL_IO_BASE);
		/* udelay(5000);	*/
		outb_p( 1, CONFIG_NE2K_CBUS_CNET98EL_IO_BASE);
		/* udelay(5000);	*/
		outb_p( (ioaddr & 0xf000) >> 8 | 0x08 | 0x01, CONFIG_NE2K_CBUS_CNET98EL_IO_BASE +2);
		/* udelay(5000); */
	}
#endif

	if (!request_region(ioaddr, NE_IO_EXTENT, dev->name))
		return -EBUSY;

	reg0 = inb_p(ioaddr+NE_SHIFT(0));
	if (reg0 == 0xFF) {
		ret = -ENODEV;
		goto err_out;
	}

	/* Do a preliminary verification that we have a 8390. */
#ifdef CONFIG_NE2K_CBUS_CNET98EL
	if(hw->hwtype!=NE2K_CBUS_HARDWARE_TYPE_CNET98EL)
#endif
	{
		int regd;
		outb_p(E8390_NODMA+E8390_PAGE1+E8390_STOP, ioaddr + E8390_CMD);
		regd = inb_p(ioaddr+NE_SHIFT(0x0d));
		outb_p(0xff, ioaddr+NE_SHIFT(0x0d));
		outb_p(E8390_NODMA+E8390_PAGE0, ioaddr + E8390_CMD);
		inb_p(ioaddr + EN0_COUNTER0); /* Clear the counter by reading. */
		if (inb_p(ioaddr + EN0_COUNTER0) != 0) {
			outb_p(reg0, ioaddr);
			outb_p(regd, ioaddr+NE_SHIFT(0x0d)); /* Restore the old values. */
			ret = -ENODEV;
			goto err_out;
		}
	}

#ifdef CONFIG_NET_CBUS
	if(ei_debug > 2)
		printk(KERN_DEBUG "ne_probe1(): 8390 verification passed.\n");
#endif

	if (ei_debug  &&  version_printed++ == 0)
		printk(KERN_INFO "%s" KERN_INFO "%s", version1, version2);

	printk(KERN_INFO "NE*000 ethercard probe at %#3x:", ioaddr);

	/* A user with a poor card that fails to ack the reset, or that
	   does not have a valid 0x57,0x57 signature can still use this
	   without having to recompile. Specifying an i/o address along
	   with an otherwise unused dev->mem_end value of "0xBAD" will
	   cause the driver to skip these parts of the probe. */

	bad_card = ((dev->base_addr != 0) && (dev->mem_end == 0xbad));

	/* Reset card. Who knows what dain-bramaged state it was left in. */

	{
		unsigned long reset_start_time = jiffies;

#ifdef CONFIG_NET_CBUS
		/* derived from CNET98EL-patch for bad clones */
		outb_p(E8390_NODMA | E8390_STOP, ioaddr+E8390_CMD);
#endif

		/* DON'T change these to inb_p/outb_p or reset will fail on clones. */
		outb(inb(ioaddr + NE_RESET), ioaddr + NE_RESET);

		while ((inb_p(ioaddr + EN0_ISR) & ENISR_RESET) == 0)
		if (jiffies - reset_start_time > 2*HZ/100) {
			if (bad_card) {
				printk(" (warning: no reset ack)");
				break;
			} else {
				printk(" not found (no reset ack).\n");
				ret = -ENODEV;
				goto err_out;
			}
		}

		outb_p(0xff, ioaddr + EN0_ISR);		/* Ack all intr. */
	}

#ifdef CONFIG_NE2K_CBUS_ICM
#if 0 /* obsoleted */
	if(hw->hwtype == NE2K_CBUS_HARDWARE_TYPE_ICM){
		static const char pat[32] ="AbcdeFghijKlmnoPqrstUvwxyZ789012";
		char buf[sizeof(pat)];
		int maxwait = 200;

		if(ei_debug>2){
			printk(" [ICM-specific initialize...");
		}

		inb(ioaddr + EN0_ISR);
		outb_p(E8390_RXOFF, ioaddr+EN0_RXCR);
		outb_p(0x1|0x40|0x8, ioaddr+EN0_DCFG); /* ENDCFG_WTS|ENDCFG_FT1|ENDCFG_LS */
		outb_p(16384/256, ioaddr+EN0_STARTPG);
		outb_p(32768/256, ioaddr+EN0_STOPPG);
		ne2k_cbus_writemem(dev, ioaddr, 16384, pat, sizeof(pat));
		while((inb(ioaddr+EN0_ISR) & ENISR_RDC) != ENISR_RDC
			  && --maxwait)
			;
		if(ei_debug>2){
			printk("write pat...");
		}
		ne2k_cbus_readmem(dev, ioaddr, 16384, buf, sizeof(pat));
		if(ei_debug>2){
			printk("read pat...");
		}
		if(memcmp(pat, buf, sizeof(pat))) {
			if(ei_debug>2){
				printk("compare failed.)");
			}
			printk(" memory failure\n");
			return ENODEV;
		}
		if(ei_debug>2){
			printk("compare ok...");
		}
		ne2k_cbus_readmem(dev, ioaddr, 0, SA_prom, 32);
		outb(0xff, ioaddr+EN0_ISR);
		printk("done)");
	}
	else
#endif
#endif /* CONFIG_NE2K_CBUS_ICM */
#ifdef CONFIG_NE2K_CBUS_CNET98EL
	if(hw->hwtype == NE2K_CBUS_HARDWARE_TYPE_CNET98EL){
		static const char pat[32] ="AbcdeFghijKlmnoPqrstUvwxyZ789012";
		char buf[32];
		int maxwait = 200;

		if(ei_debug>2){
			printk(" [CNET98EL-specific initialize...");
		}
		outb_p(E8390_NODMA | E8390_STOP, ioaddr+E8390_CMD); /* 0x20|0x1 */
		i=inb(ioaddr);
		if (( i & ~0x2) != (0x20 | 0x01))
			return ENODEV;
		if (( inb(ioaddr + 0x7) & 0x80) != 0x80)
			return ENODEV;
		outb_p(E8390_RXOFF, ioaddr+EN0_RXCR); /* out(ioaddr+0xc, 0x20) */
		/* outb_p(ENDCFG_WTS|ENDCFG_FT1|ENDCFG_LS, ioaddr+EN0_DCFG); */
		outb_p(ENDCFG_WTS|0x48 ,ioaddr+EN0_DCFG); /* 0x49 */
		outb_p(CNET98EL_START_PG,ioaddr+EN0_STARTPG);
		outb_p(CNET98EL_STOP_PG,ioaddr+EN0_STOPPG);
		if(ei_debug>2){
			printk("memory check");
		}
		for(i=0;i<65536; i+=1024){
			if(ei_debug>2){
				printk(" %04x",i);
			}
			ne2k_cbus_writemem(dev,ioaddr, i, pat, 32);
			while(((inb(ioaddr+EN0_ISR)&ENISR_RDC) != ENISR_RDC) && --maxwait)
				;
			ne2k_cbus_readmem(dev,ioaddr, i, buf, 32);
			if(memcmp(pat, buf, 32)) {
				if(ei_debug>2){
					printk(" failed.");
				}
				break;
			}
		}
		if (i != 16384 ){
			if(ei_debug>2){
				printk("] ");
			}
			printk("memory failure at %x\n", i);
			return ENODEV;
		}
		if(ei_debug>2){
			printk(" good...");
		}
		if(!dev->irq){
			if(ei_debug>2){
				printk("] ");
			}
			printk("IRQ must be specified for C-NET(98)E/L. probe failed.\n");
			return ENODEV;
		}
		outb((dev->irq>5)?(dev->irq&4):(dev->irq>>1), ioaddr+(0x2|0x400));
		outb(0x7e, ioaddr+(0x4|0x400));
		ne2k_cbus_readmem(dev, ioaddr, 16384, SA_prom, 32);
		outb(0xff, ioaddr+EN0_ISR);
		if(ei_debug>2){
			printk("done]");
		}
	}else
#endif /* CONFIG_NE2K_CBUS_CNET98EL */
	/* Read the 16 bytes of station address PROM.
	   We must first initialize registers, similar to NS8390_init(eifdev, 0).
	   We can't reliably read the SAPROM address without this.
	   (I learned the hard way!). */
	{
		struct {unsigned char value; unsigned short offset; } program_seq[] = 
		{
			{E8390_NODMA+E8390_PAGE0+E8390_STOP, E8390_CMD}, /* Select page 0*/
#ifndef CONFIG_NET_CBUS
			{0x48,	EN0_DCFG},	/* Set byte-wide (0x48) access. */
#else
			/* NEC PC-9800: some board can only handle word-wide access? */
			{0x48|ENDCFG_WTS,	EN0_DCFG},	/* Set word-wide (0x48) access. */
			{16384/256, EN0_STARTPG},
			{32768/256, EN0_STOPPG},
#endif
			{0x00,	EN0_RCNTLO},	/* Clear the count regs. */
			{0x00,	EN0_RCNTHI},
			{0x00,	EN0_IMR},	/* Mask completion irq. */
			{0xFF,	EN0_ISR},
			{E8390_RXOFF, EN0_RXCR},	/* 0x20  Set to monitor */
			{E8390_TXOFF, EN0_TXCR},	/* 0x02  and loopback mode. */
			{32,	EN0_RCNTLO},
			{0x00,	EN0_RCNTHI},
			{0x00,	EN0_RSARLO},	/* DMA starting at 0x0000. */
			{0x00,	EN0_RSARHI},
			{E8390_RREAD+E8390_START, E8390_CMD},
		};

#ifdef CONFIG_NET_CBUS
		if(ei_debug > 2){
			printk(" [outb_p");
		}
#endif

		for (i = 0; i < sizeof(program_seq)/sizeof(program_seq[0]); i++)
#ifdef CONFIG_NET_CBUS
		{
			if(ei_debug > 2)
				printk("(0x%x,0x%x)",program_seq[i].value, ioaddr + program_seq[i].offset);
#endif
			outb_p(program_seq[i].value, ioaddr + program_seq[i].offset);
#ifdef CONFIG_NET_CBUS
		}
		if(ei_debug > 2)
			printk("]");
#endif
#ifndef CONFIG_NET_CBUS
	for(i = 0; i < 32 /*sizeof(SA_prom)*/; i+=2) {
		SA_prom[i] = inb(ioaddr + NE_DATAPORT);
		SA_prom[i+1] = inb(ioaddr + NE_DATAPORT);
		if (SA_prom[i] != SA_prom[i+1])
			wordlength = 1;
	}
#else
	insw(ioaddr+NE_DATAPORT, SA_prom, 32>>1);
#endif

	}

	if(ei_debug>2){
		printk("[SA_prom[]={ ");
		for(i=0;i<32;i++) printk("%02x ",SA_prom[i]);
		printk("}]");
	}

#ifndef CONFIG_NET_CBUS
	if (wordlength == 2)
	{
		for (i = 0; i < 16; i++)
			SA_prom[i] = SA_prom[i+i];
		/* We must set the 8390 for word mode. */
		outb_p(0x49, ioaddr + EN0_DCFG);
		start_page = NESM_START_PG;
		stop_page = NESM_STOP_PG;
	} else {
		start_page = NE1SM_START_PG;
		stop_page = NE1SM_STOP_PG;
	}
#else
	for (i = 0; i < 16; i++)
		SA_prom[i] = SA_prom[i+i];
#ifdef CONFIG_NE2K_CBUS_CNET98EL
	if(hw->hwtype==NE2K_CBUS_HARDWARE_TYPE_CNET98EL){
		start_page = CNET98EL_START_PG;
		stop_page = CNET98EL_STOP_PG;
	}else{
#endif
		start_page = NESM_START_PG;
		stop_page = NESM_STOP_PG;
#ifdef CONFIG_NE2K_CBUS_CNET98EL
	}
#endif
#endif

	neX000 = (SA_prom[14] == 0x57  &&  SA_prom[15] == 0x57);
#ifndef CONFIG_NET_CBUS
	ctron =  (SA_prom[0] == 0x00 && SA_prom[1] == 0x00 && SA_prom[2] == 0x1d);
	copam =  (SA_prom[14] == 0x49 && SA_prom[15] == 0x00);

	/* Set up the rest of the parameters. */
	if (neX000 || bad_card || copam) {
		name = (wordlength == 2) ? "NE2000" : "NE1000";
	}
	else if (ctron)
	{
		name = (wordlength == 2) ? "Ctron-8" : "Ctron-16";
		start_page = 0x01;
		stop_page = (wordlength == 2) ? 0x40 : 0x20;
	}
#else
	if (neX000){
		name = "NE2000-compat";
	}
#endif
	else
	{
#ifdef SUPPORT_NE_BAD_CLONES
		/* Ack!  Well, there might be a *bad* NE*000 clone there.
		   Check for total bogus addresses. */
		for (i = 0; bad_clone_list[i].name8; i++)
		{
			if (SA_prom[0] == bad_clone_list[i].SAprefix[0] &&
				SA_prom[1] == bad_clone_list[i].SAprefix[1] &&
				SA_prom[2] == bad_clone_list[i].SAprefix[2])
			{
#ifndef CONFIG_NET_CBUS
				if (wordlength == 2)
				{
					name = bad_clone_list[i].name16;
				} else {
					name = bad_clone_list[i].name8;
				}
#else
				name = bad_clone_list[i].name16;
#endif
				break;
			}
		}
		if (bad_clone_list[i].name8 == NULL)
		{
			printk(" not found (invalid signature %2.2x %2.2x).\n",
				SA_prom[14], SA_prom[15]);
#ifdef CONFIG_NET_CBUS
			if(ei_debug>2){
				printk(KERN_DEBUG "PROM prefix is: %2.2x %2.2x %2.2x\n",
					   SA_prom[0], SA_prom[1], SA_prom[2]);
			}
#endif
			ret = -ENXIO;
			goto err_out;
		}
#else
		printk(" not found.\n");
		ret = -ENXIO;
		goto err_out;
#endif
	}

	if (dev->irq < 2)
	{
		unsigned long cookie = probe_irq_on();
		outb_p(0x50, ioaddr + EN0_IMR);	/* Enable one interrupt. */
		outb_p(0x00, ioaddr + EN0_RCNTLO);
		outb_p(0x00, ioaddr + EN0_RCNTHI);
		outb_p(E8390_RREAD+E8390_START, ioaddr); /* Trigger it... */
		mdelay(10);		/* wait 10ms for interrupt to propagate */
		outb_p(0x00, ioaddr + EN0_IMR); 		/* Mask it again. */
		dev->irq = probe_irq_off(cookie);
		if (ei_debug > 2)
			printk(" autoirq is %d\n", dev->irq);
	} else
#ifndef CONFIG_PC9800
	if (dev->irq == 2)
		/* Fixup for users that don't know that IRQ 2 is really IRQ 9,
		   or don't know which one to set. */
		dev->irq = 9;
#else
	if (dev->irq == 7)
		/* Fixup for users that don't know that IRQ 7 is really IRQ 11,
		   or don't know which one to set. */
		dev->irq = 11;
#endif

	if (! dev->irq) {
		printk(" failed to detect IRQ line.\n");
		ret = -EAGAIN;
		goto err_out;
	}

	/* Allocate dev->priv and fill in 8390 specific dev fields. */
	if (ethdev_init(dev))
	{
        	printk (" unable to get memory for dev->priv.\n");
        	ret = -ENOMEM;
		goto err_out;
	}

	/* Snarf the interrupt now.  There's no point in waiting since we cannot
	   share and the board will usually be enabled. */
	ret = request_irq(dev->irq, ei_interrupt, 0, name, dev);
	if (ret) {
		printk (" unable to get IRQ %d (errno=%d).\n", dev->irq, ret);
		goto err_out_kfree;
	}

	dev->base_addr = ioaddr;

	for(i = 0; i < ETHER_ADDR_LEN; i++) {
		printk(" %2.2x", SA_prom[i]);
		dev->dev_addr[i] = SA_prom[i];
	}

#ifndef CONFIG_NET_CBUS
	printk("\n%s: %s found at %#x, using IRQ %d.\n",
		dev->name, name, ioaddr, dev->irq);
#else
	printk("\n%s: %s found at %#x, hardware type %d(%s), using IRQ %d.\n",
		   dev->name, name, ioaddr, hw->hwtype, hw->hwident, dev->irq);
#endif

	ei_status.name = name;
	ei_status.tx_start_page = start_page;
	ei_status.stop_page = stop_page;
#ifndef CONFIG_NET_CBUS
	ei_status.word16 = (wordlength == 2);
#else
	ei_status.word16 = (2 == 2); /* wordlength is always 2 */
#endif

	ei_status.rx_start_page = start_page + TX_PAGES;
#ifdef PACKETBUF_MEMSIZE
	 /* Allow the packet buffer size to be overridden by know-it-alls. */
	ei_status.stop_page = ei_status.tx_start_page + PACKETBUF_MEMSIZE;
#endif

	ei_status.reset_8390 = &ne_reset_8390;
	ei_status.block_input = &ne_block_input;
	ei_status.block_output = &ne_block_output;
	ei_status.get_8390_hdr = &ne_get_8390_hdr;
	ei_status.priv = 0;
	dev->open = &ne_open;
	dev->stop = &ne_close;
	NS8390_init(dev, 0);
	return 0;

err_out_kfree:
#ifndef CONFIG_NET_CBUS
	kfree(dev->priv);
	dev->priv = NULL;
#else
	ne2k_cbus_destroy(dev);
#endif
err_out:
	release_region(ioaddr, NE_IO_EXTENT);
	return ret;
}

static int ne_open(struct net_device *dev)
{
	ei_open(dev);
	return 0;
}

static int ne_close(struct net_device *dev)
{
	if (ei_debug > 1)
		printk(KERN_DEBUG "%s: Shutting down ethercard.\n", dev->name);
	ei_close(dev);
	return 0;
}

/* Hard reset the card.  This used to pause for the same period that a
   8390 reset command required, but that shouldn't be necessary. */

static void ne_reset_8390(struct net_device *dev)
{
	unsigned long reset_start_time = jiffies;
#ifdef CONFIG_NET_CBUS
	struct ei_device *ei_local = (struct ei_device *)(dev->priv);
#endif

	if (ei_debug > 1)
		printk(KERN_DEBUG "resetting the 8390 t=%ld...", jiffies);

#ifdef CONFIG_NET_CBUS
	/* derived from CNET98EL-patch for bad clones... */
	outb_p(E8390_NODMA|E8390_STOP, NE_BASE+E8390_CMD);  /* 0x20 | 0x1 */
#endif

	/* DON'T change these to inb_p/outb_p or reset will fail on clones. */
	outb(inb(NE_BASE + NE_RESET), NE_BASE + NE_RESET);

	ei_status.txing = 0;
	ei_status.dmaing = 0;

	/* This check _should_not_ be necessary, omit eventually. */
	while ((inb_p(NE_BASE+EN0_ISR) & ENISR_RESET) == 0)
		if (jiffies - reset_start_time > 2*HZ/100) {
			printk(KERN_WARNING "%s: ne_reset_8390() did not complete.\n", dev->name);
			break;
		}
	outb_p(ENISR_RESET, NE_BASE + EN0_ISR);	/* Ack intr. */
}

/* Grab the 8390 specific header. Similar to the block_input routine, but
   we don't need to be concerned with ring wrap as the header will be at
   the start of a page, so we optimize accordingly. */

static void ne_get_8390_hdr(struct net_device *dev, struct e8390_pkt_hdr *hdr, int ring_page)
{
	int nic_base = dev->base_addr;
#ifdef CONFIG_NET_CBUS
	struct ei_device *ei_local = (struct ei_device *)(dev->priv);
#endif

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */

	if (ei_status.dmaing)
	{
		printk(KERN_EMERG "%s: DMAing conflict in ne_get_8390_hdr "
			"[DMAstat:%d][irqlock:%d].\n",
			dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}

	ei_status.dmaing |= 0x01;
	outb_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
	outb_p(sizeof(struct e8390_pkt_hdr), nic_base + EN0_RCNTLO);
	outb_p(0, nic_base + EN0_RCNTHI);
	outb_p(0, nic_base + EN0_RSARLO);		/* On page boundary */
	outb_p(ring_page, nic_base + EN0_RSARHI);
	outb_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);

	if (ei_status.word16)
		insw(NE_BASE + NE_DATAPORT, hdr, sizeof(struct e8390_pkt_hdr)>>1);
	else
		insb(NE_BASE + NE_DATAPORT, hdr, sizeof(struct e8390_pkt_hdr));

	outb_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;

	le16_to_cpus(&hdr->count);
}

/* Block input and output, similar to the Crynwr packet driver.  If you
   are porting to a new ethercard, look at the packet driver source for hints.
   The NEx000 doesn't share the on-board packet memory -- you have to put
   the packet out through the "remote DMA" dataport using outb. */

static void ne_block_input(struct net_device *dev, int count, struct sk_buff *skb, int ring_offset)
{
#ifdef NE_SANITY_CHECK
	int xfer_count = count;
#endif
	int nic_base = dev->base_addr;
	char *buf = skb->data;
#ifdef CONFIG_NET_CBUS
	struct ei_device *ei_local = (struct ei_device *)(dev->priv);
#endif

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */
	if (ei_status.dmaing)
	{
		printk(KERN_EMERG "%s: DMAing conflict in ne_block_input "
			"[DMAstat:%d][irqlock:%d].\n",
			dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}
	ei_status.dmaing |= 0x01;

#ifdef CONFIG_NET_CBUS
	/* derived from ICM-patch */
	/* round up count to a word */
	if(count&1) {
	    count++;
	}
#endif

	outb_p(E8390_NODMA+E8390_PAGE0+E8390_START, nic_base+ NE_CMD);
	outb_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_p(count >> 8, nic_base + EN0_RCNTHI);
	outb_p(ring_offset & 0xff, nic_base + EN0_RSARLO);
	outb_p(ring_offset >> 8, nic_base + EN0_RSARHI);
	outb_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
	if (ei_status.word16)
	{
		insw(NE_BASE + NE_DATAPORT,buf,count>>1);
		if (count & 0x01)
		{
			buf[count-1] = inb(NE_BASE + NE_DATAPORT);
#ifdef NE_SANITY_CHECK
			xfer_count++;
#endif
		}
	} else {
		insb(NE_BASE + NE_DATAPORT, buf, count);
	}

#ifdef NE_SANITY_CHECK
	/* This was for the ALPHA version only, but enough people have
	   been encountering problems so it is still here.  If you see
	   this message you either 1) have a slightly incompatible clone
	   or 2) have noise/speed problems with your bus. */

	if (ei_debug > 1)
	{
		/* DMA termination address check... */
		int addr, tries = 20;
		do {
			/* DON'T check for 'inb_p(EN0_ISR) & ENISR_RDC' here
			   -- it's broken for Rx on some cards! */
			int high = inb_p(nic_base + EN0_RSARHI);
			int low = inb_p(nic_base + EN0_RSARLO);
			addr = (high << 8) + low;
			if (((ring_offset + xfer_count) & 0xff) == low)
				break;
		} while (--tries > 0);
	 	if (tries <= 0)
			printk(KERN_WARNING "%s: RX transfer address mismatch,"
				"%#4.4x (expected) vs. %#4.4x (actual).\n",
				dev->name, ring_offset + xfer_count, addr);
	}
#endif
	outb_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;
}

static void ne_block_output(struct net_device *dev, int count,
		const unsigned char *buf, const int start_page)
{
	int nic_base = NE_BASE;
	unsigned long dma_start;
#ifdef NE_SANITY_CHECK
	int retries = 0;
#endif
#ifdef CONFIG_NET_CBUS
	struct ei_device *ei_local = (struct ei_device *)(dev->priv);
#endif

	/* Round the count up for word writes.  Do we need to do this?
	   What effect will an odd byte count have on the 8390?
	   I should check someday. */

	if (ei_status.word16 && (count & 0x01))
		count++;

	/* This *shouldn't* happen. If it does, it's the last thing you'll see */
	if (ei_status.dmaing)
	{
		printk(KERN_EMERG "%s: DMAing conflict in ne_block_output."
			"[DMAstat:%d][irqlock:%d]\n",
			dev->name, ei_status.dmaing, ei_status.irqlock);
		return;
	}
	ei_status.dmaing |= 0x01;
	/* We should already be in page 0, but to be safe... */
	outb_p(E8390_PAGE0+E8390_START+E8390_NODMA, nic_base + NE_CMD);

#ifdef NE_SANITY_CHECK
retry:
#endif

#ifdef NE8390_RW_BUGFIX
	/* Handle the read-before-write bug the same way as the
	   Crynwr packet driver -- the NatSemi method doesn't work.
	   Actually this doesn't always work either, but if you have
	   problems with your NEx000 this is better than nothing! */

	outb_p(0x42, nic_base + EN0_RCNTLO);
	outb_p(0x00,   nic_base + EN0_RCNTHI);
	outb_p(0x42, nic_base + EN0_RSARLO);
	outb_p(0x00, nic_base + EN0_RSARHI);
	outb_p(E8390_RREAD+E8390_START, nic_base + NE_CMD);
	/* Make certain that the dummy read has occurred. */
	udelay(6);
#endif

	outb_p(ENISR_RDC, nic_base + EN0_ISR);

	/* Now the normal output. */
	outb_p(count & 0xff, nic_base + EN0_RCNTLO);
	outb_p(count >> 8,   nic_base + EN0_RCNTHI);
	outb_p(0x00, nic_base + EN0_RSARLO);
	outb_p(start_page, nic_base + EN0_RSARHI);

	outb_p(E8390_RWRITE+E8390_START, nic_base + NE_CMD);
	if (ei_status.word16) {
		outsw(NE_BASE + NE_DATAPORT, buf, count>>1);
	} else {
		outsb(NE_BASE + NE_DATAPORT, buf, count);
	}

	dma_start = jiffies;

#ifdef NE_SANITY_CHECK
	/* This was for the ALPHA version only, but enough people have
	   been encountering problems so it is still here. */

	if (ei_debug > 1)
	{
		/* DMA termination address check... */
		int addr, tries = 20;
		do {
			int high = inb_p(nic_base + EN0_RSARHI);
			int low = inb_p(nic_base + EN0_RSARLO);
			addr = (high << 8) + low;
			if ((start_page << 8) + count == addr)
				break;
		} while (--tries > 0);

		if (tries <= 0)
		{
			printk(KERN_WARNING "%s: Tx packet transfer address mismatch,"
				"%#4.4x (expected) vs. %#4.4x (actual).\n",
				dev->name, (start_page << 8) + count, addr);
			if (retries++ == 0)
				goto retry;
		}
	}
#endif

	while ((inb_p(nic_base + EN0_ISR) & ENISR_RDC) == 0)
		if (jiffies - dma_start > 2*HZ/100) {		/* 20ms */
			printk(KERN_WARNING "%s: timeout waiting for Tx RDC.\n", dev->name);
			ne_reset_8390(dev);
			NS8390_init(dev,1);
			break;
		}

	outb_p(ENISR_RDC, nic_base + EN0_ISR);	/* Ack intr. */
	ei_status.dmaing &= ~0x01;
	return;
}


#ifdef MODULE
#define MAX_NE_CARDS	4	/* Max number of NE cards per module */
static struct net_device dev_ne[MAX_NE_CARDS];
static int io[MAX_NE_CARDS];
static int irq[MAX_NE_CARDS];
static int bad[MAX_NE_CARDS];	/* 0xbad = bad sig or no reset ack */

MODULE_PARM(io, "1-" __MODULE_STRING(MAX_NE_CARDS) "i");
MODULE_PARM(irq, "1-" __MODULE_STRING(MAX_NE_CARDS) "i");
MODULE_PARM(bad, "1-" __MODULE_STRING(MAX_NE_CARDS) "i");
MODULE_PARM_DESC(io, "NEx000 I/O base address(es),required");
MODULE_PARM_DESC(irq, "NEx000 IRQ number(s)");
MODULE_PARM_DESC(bad, "NEx000 accept bad clone(s)");

#ifdef CONFIG_PC9800
static int hwtype[MAX_NE_CARDS] = { 0, }; /* board type */
MODULE_PARM(hwtype, "1-" __MODULE_STRING(MAX_NE_CARDS) "i");
MODULE_PARM_DESC(hwtype, "C-Bus NEx000 hardware type");
#endif

/* This is set up so that no ISA autoprobe takes place. We can't guarantee
that the ne2k probe is the last 8390 based probe to take place (as it
is at boot) and so the probe will get confused by any other 8390 cards.
ISA device autoprobes on a running machine are not recommended anyway. */

int init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < MAX_NE_CARDS; this_dev++) {
		struct net_device *dev = &dev_ne[this_dev];
		dev->irq = irq[this_dev];
		dev->mem_end = bad[this_dev];
		dev->base_addr = io[this_dev];
#ifdef CONFIG_PC9800
		dev->mem_start = hwtype[this_dev];
#endif
		dev->init = ne_probe;
		if (register_netdev(dev) == 0) {
			found++;
			continue;
		}
		if (found != 0) { 	/* Got at least one. */
			return 0;
		}
		if (io[this_dev] != 0)
			printk(KERN_WARNING "ne.c: No NE*000 card found at i/o = %#x\n", io[this_dev]);
		else
			printk(KERN_NOTICE "ne.c: You must supply \"io=0xNNN\" value(s) for ISA cards.\n");
		return -ENXIO;
	}
	return 0;
}

void cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < MAX_NE_CARDS; this_dev++) {
		struct net_device *dev = &dev_ne[this_dev];
		if (dev->priv != NULL) {
#ifndef CONFIG_NET_CBUS
			void *priv = dev->priv;
#endif
			struct pci_dev *idev = (struct pci_dev *)ei_status.priv;
			if (idev)
				idev->deactivate(idev);
			free_irq(dev->irq, dev);
#ifndef CONFIG_NET_CBUS
			release_region(dev->base_addr, NE_IO_EXTENT);
#else /* CONFIG_NET_CBUS */
			{
				const struct ne2k_cbus_hwinfo *hw = ne2k_cbus_get_hwinfo((int)(dev->mem_start & NE2K_CBUS_HARDWARE_TYPE_MASK));
				const struct ne2k_cbus_region *rlist;
				for(rlist = hw->regionlist; rlist->range; rlist++){
					release_region(dev->base_addr + rlist->start, rlist->range);
				}
			}
#endif /* !CONFIG_NET_CBUS */
			unregister_netdev(dev);
#ifndef CONFIG_NET_CBUS
			kfree(priv);
#else
			ne2k_cbus_destroy(dev);
#endif
		}
	}
}
#endif /* MODULE */

/*
 * Local variables:
 *  compile-command: "gcc -DKERNEL -Wall -O6 -fomit-frame-pointer -I/usr/src/linux/net/tcp -c ne.c"
 *  version-control: t
 *  kept-new-versions: 5
 * End:
 */
