/*
 *  ide_pc9800.c
 *
 *  Copyright (C) 1997-2000  Linux/98 project,
 *			     Kyoto University Microcomputer Club.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/ide.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/pc9800.h>

#define PC9800_IDE_BANKSELECT	0x432

#define DEBUG

static void
pc9800_select (ide_drive_t *drive)
{
#ifdef DEBUG
	byte old;

	/* Too noisy: */
	/* printk (KERN_DEBUG "pc9800_select (%s)\n", drive->name); */

	OUT_BYTE (0x80, PC9800_IDE_BANKSELECT);
	old = IN_BYTE (PC9800_IDE_BANKSELECT);
	if (old != HWIF (drive)->index)
		printk (KERN_DEBUG "ide-pc9800: switching bank #%d -> #%d\n",
			old, HWIF (drive)->index);
#endif
	OUT_BYTE (HWIF (drive)->index, PC9800_IDE_BANKSELECT);
}

void __init
ide_probe_for_pc9800 (void)
{
	byte tmp;

	if (!PC9800_9821_P () /* || !PC9821_IDEIF_DOUBLE_P () */)
		return;

	if (check_region (PC9800_IDE_BANKSELECT, 1)) {
		printk (KERN_ERR
			"ide: bank select port (%#x) is already occupied!\n",
			PC9800_IDE_BANKSELECT);
		return;
	}

	/* Do actual probing. */
	if ((tmp = IN_BYTE (PC9800_IDE_BANKSELECT)) == (byte) ~0
	    || (OUT_BYTE (tmp ^ 1, PC9800_IDE_BANKSELECT),
		/* Next OUT_BYTE is dummy for reading status. */
		OUT_BYTE (0x80, PC9800_IDE_BANKSELECT),
		IN_BYTE (PC9800_IDE_BANKSELECT) != (tmp ^ 1))) {
		printk (KERN_INFO
			"ide: pc9800 type bank selecting port not found\n");
		return;
	}
	/* Restore original value, just in case. */
	OUT_BYTE (tmp, PC9800_IDE_BANKSELECT);

	request_region (PC9800_IDE_BANKSELECT, 1, "ide0/1 bank");

	/* These ports are probably used by IDE I/F.  */
	request_region (0x430, 1, "ide");
	request_region (0x435, 1, "ide");

	if (ide_hwifs[0].io_ports[IDE_DATA_OFFSET] == HD_DATA
	    && ide_hwifs[1].io_ports[IDE_DATA_OFFSET] == HD_DATA) {
		ide_hwifs[0].chipset = ide_pc9800;
		ide_hwifs[0].mate = &ide_hwifs[1];
		ide_hwifs[0].selectproc = pc9800_select;
		ide_hwifs[1].chipset = ide_pc9800;
		ide_hwifs[1].mate = &ide_hwifs[0];
		ide_hwifs[1].selectproc = pc9800_select;
	}
}
