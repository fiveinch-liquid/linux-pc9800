/*
 *  fs/partitions/x68000.h
 */

int x68k_partition(struct gendisk *hd, kdev_t dev,
		   unsigned long first_sector, int first_minor);
