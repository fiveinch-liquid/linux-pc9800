/*
 *  NEC PC-9800 series partition supports
 *
 *  Copyright (C) 1998-2000	Kyoto University Microcomputer Club
 */

#define NEC98_PTABLE_MAGIC	0xAA55

extern int nec98_partition (struct gendisk *, kdev_t, unsigned long, int);
