/*
 *  fs/partitions/x68000.c
 *
 *  X68000 style partition table handler.
 *
 *  Copyright (C) 1999-2000  TAKAI Kousuke  <tak@kmc.kyoto-u.ac.jp>
 *			Linux/98 project, Kyoto Univ. Microcomputer Club
 */

/*
 * Commentary:
 *  X68000/X68030 are MC68000/MC68EC030-based personal computers
 *  designed by Sharp Corporation and used to be sold mainly in Japan.
 *
 *  We had never heard that Linux runs on them, but here is the partition
 *  table handling routine for fun. :-)
 */

#include <linux/fs.h>

#include "check.h"

struct x68k_partition {
	s8	name[8];
	u32	start;
	u32	length;
} __attribute__((__packed__));

struct x68k_partition_table {
	u32	magic;		/* 'X68K' */
	u32	max;
	u32	spare;
	u32	shipping;
	struct x68k_partition partition[15];
} __attribute__((__packed__));

#define _X68K_MAGIC	(('X' << 24) | ('6' << 16) | ('8' << 8) | 'K')
#define X68K_MAGIC	(be32_to_cpu (_X68K_MAGIC))

int x68k_partition (struct gendisk *hd, kdev_t dev,
		    unsigned long first_sector, int first_minor)
{
	int part;
	int sectors_per_record;
	unsigned long max;
	struct x68k_partition_table *pt;
	struct buffer_head *bh;

	if (hardsect_size[MAJOR (dev)] == NULL
	    || (hardsect_size[MAJOR (dev)][MINOR (dev)] != 256
		&& hardsect_size[MAJOR (dev)][MINOR (dev)] != 512
		&& hardsect_size[MAJOR (dev)][MINOR (dev)] != 1024))
		return 0;

	bh = bread (dev, (4 * hardsect_size[MAJOR (dev)][MINOR (dev)]
			  / blksize_size[MAJOR (dev)][MINOR (dev)]),
		    blksize_size[MAJOR (dev)][MINOR (dev)]);
	if (bh == NULL)
		return -1;

	pt = ((struct x68k_partition_table *)
	      ((void *)bh->b_data
	       + (4 * hardsect_size[MAJOR (dev)][MINOR (dev)]
		  % blksize_size[MAJOR (dev)][MINOR (dev)])));
	if (pt->magic != X68K_MAGIC) {
		brelse (bh);
		return 0;
	}
	max = be32_to_cpu (pt->max);

	sectors_per_record = 1024 / hardsect_size[MAJOR (dev)][MINOR (dev)];

	for (part = 0; part < 15; part++) {
		unsigned int start, length;

		if (*(u8 *)&pt->partition[part].start)
			break;	/* end of partition table */
		start  = be32_to_cpu (pt->partition[part].start );
		length = be32_to_cpu (pt->partition[part].length);
		if (start >= max || (start + length) > max)
			break;
		if (!part)
			printk (" (X68k partition table)\n");
		add_gd_partition (hd, first_minor,
				  start  * sectors_per_record,
				  length * sectors_per_record);
		printk (" <%-8.8s> %8u+%u\n",
			pt->partition[part].name, start, length);
		first_minor++;
	}
	brelse (bh);

	return part ? 1 : 0;
}
