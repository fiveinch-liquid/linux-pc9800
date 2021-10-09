#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/blk.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/ioport.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/irq.h>
#include <asm/dma.h>
#include <linux/module.h>

#include "scsi.h"
#include "hosts.h"
#include "wd33c93.h"
#include "pc980155.h"
#include "pc980155regs.h"

#define DEBUG

#include<linux/stat.h>

static inline void __print_debug_info(unsigned int);
static inline void __print_debug_info(unsigned int a){}
#define print_debug_info() __print_debug_info(base_io);

#define NR_BASE_IOS 4
static int nr_base_ios = NR_BASE_IOS;
static unsigned int base_ios[NR_BASE_IOS] = {0xcc0, 0xcd0, 0xce0, 0xcf0};
static wd33c93_regs regp;

static struct Scsi_Host *pc980155_host = NULL;

static void pc980155_intr_handle(int irq, void *dev_id, struct pt_regs *regs);

inline void pc980155_dma_enable(unsigned int base_io){
  outb (0x01, REG_CWRITE);
  WAIT();
}
inline void pc980155_dma_disable(unsigned int base_io){
  outb (0x02, REG_CWRITE);
  WAIT();
}


static void pc980155_intr_handle(int irq, void *dev_id, struct pt_regs *regs){
  wd33c93_intr (pc980155_host);
}

static int dma_setup (Scsi_Cmnd *sc, int dir_in){
  /*
   * sc->SCp.this_residual : transfer count
   * sc->SCp.ptr : distination address (virtual address)
   * dir_in : data direction (DATA_OUT_DIR:0 or DATA_IN_DIR:1)
   *
   * if success return 0
   */

   /*
    * DMA WRITE MODE
    * bit 7,6 01b single mode (this mode only)
    * bit 5   inc/dec (default:0 = inc)
    * bit 4   auto initialize (normaly:0 = off)
    * bit 3,2 01b memory -> io
    *         10b io -> memory
    *         00b verify
    * bit 1,0 channel
    */
  disable_dma(sc->host->dma_channel);
  set_dma_mode(sc->host->dma_channel, 0x40 | (dir_in ? 0x04 : 0x08));
  clear_dma_ff(sc->host->dma_channel);
  set_dma_addr(sc->host->dma_channel, virt_to_phys(sc->SCp.ptr));
  set_dma_count(sc->host->dma_channel, sc->SCp.this_residual);
#if 0
#ifdef DEBUG
  printk ("D%d(%x)D", sc->SCp.this_residual);
#endif
#endif
  enable_dma(sc->host->dma_channel);

  pc980155_dma_enable(sc->host->io_port);

  return 0;
}

static void dma_stop (struct Scsi_Host *instance, Scsi_Cmnd *sc, int status){
  /*
   * instance: Hostadapter's instance
   * sc: scsi command
   * status: True if success
   */

  pc980155_dma_disable(sc->host->io_port);

  disable_dma(sc->host->dma_channel);
}  

/* return non-zero on detection */
static inline int pc980155_test_port (wd33c93_regs* regp)
{
	/* Quick and dirty test for presence of the card. */
	if (READ_AUX_STAT() == 0xff)
		return 0;
	return 1;
}

static inline int
pc980155_getconfig (unsigned int base_io, wd33c93_regs* regp,
		    unsigned char* irq, unsigned char* dma,
		    unsigned char* scsi_id)
{
	static unsigned char irqs[] = { 3, 5, 6, 9, 12, 13 };
	unsigned char result;
  
	printk (KERN_DEBUG "PC-9801-55: base_io=%x SASR=%x SCMD=%x\n",
		base_io, regp->SASR, regp->SCMD);
	result = read_wd33c93(regp, WD_RESETINT);
	printk (KERN_DEBUG "PC-9801-55: getting config (%x)\n", result);
	*scsi_id = result & 0x07;
	*irq = (result >> 3) & 0x07;
	if (*irq > 5) {
		printk (KERN_ERR "PC-9801-55 (base %#x): impossible IRQ (%d)"
			" - other device here?\n", base_io, *irq);
		return 0;
	}

	*irq = irqs[*irq];
	result = inb(REG_STATRD);
	WAIT();
	*dma = result & 0x03;
	if (*dma == 1) {
		printk (KERN_ERR
			"PC-9801-55 (base %#x): impossible DMA channl (%d)"
			" - other device here?\n", base_io, *dma);
		return 0;
	}
#ifdef DEBUG
	printk ("PC-9801-55: end of getconfig\n");
#endif
	return 1;
}

/* return non-zero on detection */
int scsi_pc980155_detect(Scsi_Host_Template* tpnt)
{
	unsigned int base_io;
	unsigned char irq, dma, scsi_id;
	int i;
#ifdef DEBUG
	unsigned char debug;
#endif
  
	for (i = 0; i < nr_base_ios; i++) {
		base_io = base_ios[i];
		regp.SASR = REG_ADDRST;
		regp.SCMD = REG_CONTRL;

    /*    printk ("PC-9801-55: regp.SASR(%x = %x)\n", regp.SASR, REG_ADDRST); */
		if (check_region (base_io, 6))
			continue;
		if (! pc980155_test_port (&regp))
			continue;

		if (!pc980155_getconfig (base_io, &regp, &irq, &dma, &scsi_id))
			continue;
#ifdef DEBUG
		printk ("PC-9801-55: config: base io = %x, irq = %d, dma channel = %d, scsi id = %d\n",
			base_io, irq, dma, scsi_id);
#endif
		if (request_irq (irq, pc980155_intr_handle, 0, "PC-9801-55",
				 NULL)) {
			printk (KERN_ERR
				"PC-9801-55: unable to allocate IRQ %d\n",
				irq);
			continue;
		}
		if (request_dma (dma, "PC-9801-55")) {
			printk (KERN_ERR "PC-9801-55: "
				"unable to allocate DMA channel %d\n", dma);
			free_irq (irq, NULL);
			continue;
		}

		request_region (base_io, 6, "PC-9801-55");
		pc980155_host = scsi_register(tpnt, sizeof(struct WD33C93_hostdata));
		pc980155_host->this_id = scsi_id;
		pc980155_host->io_port = base_io;
		pc980155_host->n_io_port = 6;
		pc980155_host->irq = irq;
		pc980155_host->dma_channel = dma;

#ifdef DEBUG
		printk ("PC-9801-55: scsi host found at %x irq = %d, use dma channel %d.\n", base_io, irq, dma);
		debug = read_aux_stat(&regp);
		printk ("PC-9801-55: aux: %x ", debug);
		debug = read_wd33c93(&regp, 0x17);
		printk ("status: %x\n", debug);
#endif

		pc980155_int_enable(&regp);
  
		wd33c93_init (pc980155_host, &regp, dma_setup, dma_stop,
			      WD33C93_FS_12_15);
    
		return 1;
	}

	printk ("PC-9801-55: not found\n");
	return 0;
}

int pc980155_proc_info (char *buf, char **start, off_t off, int len,
			int hostno, int in)
{
	/* NOT SUPPORTED YET! */

	if (in) {
		return -EPERM;
	}
	*start = buf;
	return sprintf (buf, "Sorry, not supported yet.\n");
}

int pc980155_setup (char *str)
{
next:
  if (!strncmp(str, "io:", 3)){
    base_ios[0] = simple_strtoul(str+3,NULL,0);
    nr_base_ios = 1;
    while (*str > ' ' && *str != ',')
      str++;
    if (*str == ','){
      str++;
      goto next;
    }
  }
  return 0;
}

__setup("pc980155", pc980155_setup);

#ifdef MODULE

Scsi_Host_Template driver_template = SCSI_PC980155;

#include "scsi_module.c"
#endif
