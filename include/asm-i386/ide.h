/*
 *  linux/include/asm-i386/ide.h
 *
 *  Copyright (C) 1994-1996  Linus Torvalds & authors
 */

/*
 *  This file contains the i386 architecture specific IDE code.
 */

#ifndef __ASMi386_IDE_H
#define __ASMi386_IDE_H

#ifdef __KERNEL__

#include <linux/config.h>

#ifndef MAX_HWIFS
# ifdef CONFIG_BLK_DEV_IDEPCI
#define MAX_HWIFS	10
# else
#define MAX_HWIFS	6
# endif
#endif

#define ide__sti()	__sti()

static __inline__ int ide_default_irq(ide_ioreg_t base)
{
	switch (base) {
#ifdef CONFIG_PC9800
		case 0x640: return 9;
#endif /* !CONFIG_PC9800 */
		case 0x1f0: return 14;
		case 0x170: return 15;
		case 0x1e8: return 11;
		case 0x168: return 10;
		case 0x1e0: return 8;
		case 0x160: return 12;
		default:
			return 0;
	}
}

static __inline__ ide_ioreg_t ide_default_io_base(int index)
{
	switch (index) {
#ifdef CONFIG_PC9800
		/* 98MEMO: PC-98x1 alternate IO-ports for primary/secondary
		   are port 430h or 432h */
		case 0: return 0x640;
		case 1: return 0x640;
#else /* !CONFIG_PC9800 */
		case 0:	return 0x1f0;
		case 1:	return 0x170;
		case 2: return 0x1e8;
		case 3: return 0x168;
		case 4: return 0x1e0;
		case 5: return 0x160;
#endif /* CONFIG_PC9800 */
		default:
			return 0;
	}
}

static __inline__ void ide_init_hwif_ports(hw_regs_t *hw, ide_ioreg_t data_port, ide_ioreg_t ctrl_port, int *irq)
{
	ide_ioreg_t reg = data_port;
	int i;
#ifdef CONFIG_PC9800
	ide_ioreg_t increment = data_port == 0x640 ? 2 : 1;
#endif

	for (i = IDE_DATA_OFFSET; i <= IDE_STATUS_OFFSET; i++) {
		hw->io_ports[i] = reg;
#ifndef CONFIG_PC9800
		reg += 1;
#else
		reg += increment;
#endif
	}
	if (ctrl_port) {
		hw->io_ports[IDE_CONTROL_OFFSET] = ctrl_port;
#ifdef CONFIG_PC9800
	} else if (data_port == 0x640) {
		hw->io_ports[IDE_CONTROL_OFFSET] = 0x74c;
#endif
	} else {
		hw->io_ports[IDE_CONTROL_OFFSET] = hw->io_ports[IDE_DATA_OFFSET] + 0x206;
	}
	if (irq != NULL)
		*irq = 0;
	hw->io_ports[IDE_IRQ_OFFSET] = 0;
}

static __inline__ void ide_init_default_hwifs(void)
{
#ifndef CONFIG_BLK_DEV_IDEPCI
	hw_regs_t hw;
	int index;

	for(index = 0; index < MAX_HWIFS; index++) {
		ide_init_hwif_ports(&hw, ide_default_io_base(index), 0, NULL);
		hw.irq = ide_default_irq(ide_default_io_base(index));
		ide_register_hw(&hw, NULL);
	}
#endif /* CONFIG_BLK_DEV_IDEPCI */
}

typedef union {
	unsigned all			: 8;	/* all of the bits together */
	struct {
		unsigned head		: 4;	/* always zeros here */
		unsigned unit		: 1;	/* drive select number, 0 or 1 */
		unsigned bit5		: 1;	/* always 1 */
		unsigned lba		: 1;	/* using LBA instead of CHS */
		unsigned bit7		: 1;	/* always 1 */
	} b;
	} select_t;

#define ide_request_irq(irq,hand,flg,dev,id)	request_irq((irq),(hand),(flg),(dev),(id))
#define ide_free_irq(irq,dev_id)		free_irq((irq), (dev_id))

#define ide_check_region(from,extent)		check_region((from), (extent))
#define ide_request_region(from,extent,name)	request_region((from), (extent), (name))
#define ide_release_region(from,extent)		release_region((from), (extent))

/*
 * The following are not needed for the non-m68k ports
 */
#define ide_ack_intr(hwif)		(1)
#define ide_fix_driveid(id)		do {} while (0)
#define ide_release_lock(lock)		do {} while (0)
#define ide_get_lock(lock, hdlr, data)	do {} while (0)

#ifdef CONFIG_PC9800_NOTDEF

#define IDE_PC9800_MUTEX_LOCK	/* Use kernel mutex rather than original one */

# ifndef IDE_PC9800_MUTEX_LOCK
int pc9800_select_idebank(int index);
void pc9800_unselect_idebank(int index);
# else

#include <asm/semaphore.h>
#include <asm/io.h>

extern struct semaphore ide_pc9800_mutex;
extern byte ide_pc9800_current_bank;

static __inline__ void pc9800_select_idebank (int index)
{
	if (index < 2) {
		down (&ide_pc9800_mutex);
		if (ide_pc9800_current_bank != index) {
			OUT_BYTE(index, 0x432);
			ide_pc9800_current_bank = index;
		}
	}
}

static inline void
pc9800_unselect_idebank (int index)
{
	if (index < 2)
		up (&ide_pc9800_mutex);
}

# endif
#endif

#endif /* __KERNEL__ */

#endif /* __ASMi386_IDE_H */
