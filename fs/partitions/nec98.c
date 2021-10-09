/*
 *  NEC PC-9800 series partition supports
 *
 *  Copyright (C) 1999	Kyoto University Microcomputer Club
 */

#include <linux/config.h>

#ifdef CONFIG_NEC98_BSD_DISKLABEL
#define CONFIG_BSD_DISKLABEL
#endif

#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/kernel.h>
#include <linux/blk.h>
#include <linux/major.h>

#include "check.h"
#include "nec98.h"

/* #ifdef CONFIG_BLK_DEV_IDEDISK */
#include <linux/ide.h>
/* #endif */

/* #ifdef CONFIG_BLK_DEV_SD */
#include "../../drivers/scsi/scsi.h"
#include "../../drivers/scsi/sd.h"
#include "../../drivers/scsi/hosts.h"
#include <scsi/scsicam.h>
/* #endif */

struct nec98_partition {
	__u8	mid;		/* 0x80 - active */
	__u8	sid;		/* 0x80 - bootable */
	__u16	pad1;		/* dummy for padding */
	__u8	ipl_sector;	/* IPL sector	*/
	__u8	ipl_head;	/* IPL head	*/
	__u16	ipl_cyl;	/* IPL cylinder	*/
	__u8	sector;		/* starting sector	*/
	__u8	head;		/* starting head	*/
	__u16	cyl;		/* starting cylinder	*/
	__u8	end_sector;	/* end sector	*/
	__u8	end_head;	/* end head	*/
	__u16	end_cyl;	/* end cylinder	*/
	unsigned char name[16];
} __attribute__((__packed__));

#define NEC98_BSD_PARTITION_MID 0x14
#define NEC98_BSD_PARTITION_SID 0x44
#define MID_SID_16(mid, sid)	(((mid) & 0xFF) | (((sid) & 0xFF) << 8))
#define NEC98_BSD_PARTITION_MID_SID	\
	MID_SID_16 (NEC98_BSD_PARTITION_MID, NEC98_BSD_PARTITION_SID)
#define NEC98_VALID_PTABLE_ENTRY(P) \
	(!(P)->pad1 && (P)->cyl <= (P)->end_cyl)

static inline int
is_valid_nec98_partition_table (const struct nec98_partition *ptable,
				__u8 nsectors, __u8 nheads)
{
	int i;
	int valid = 0;

	for (i = 0; i < 16; i++) {
		if (!*(__u16 *)&ptable[i])
			continue;	/* empty slot */
		if (ptable[i].pad1	/* `pad1' contains junk */
		    || ptable[i].ipl_sector	>= nsectors
		    || ptable[i].sector		>= nsectors
		    || ptable[i].end_sector	>= nsectors
		    || ptable[i].ipl_head	>= nheads
		    || ptable[i].head		>= nheads
		    || ptable[i].end_head	>= nheads
		    || ptable[i].cyl > ptable[i].end_cyl)
			return 0;
		valid = 1;	/* We have a valid partition.  */
	}
	/* If no valid PC-9800-style partitions found,
	   the disk may have other type of partition table.  */
	return valid;
}

#ifdef CONFIG_NEC98_BSD_DISKLABEL
extern int current_minor;
extern void bsd_disklabel_partition(struct gendisk *hd, int minor, int type)
#endif

int nec98_partition(struct gendisk *hd, kdev_t dev,
		    unsigned long first_sector, int first_minor)
{
	unsigned int nr;
	int g_head, g_sect;
	struct buffer_head *bh;
	const struct nec98_partition *part;
	int sector_size = get_hardsect_size(dev);
#ifdef CONFIG_NEC98_BSD_DISKLABEL
	int i;
	/* no bsd disklabel as a default */
	kdev_t bsd_minors[16];
	unsigned int nr_bsd_minors = 0;
#endif

	switch (MAJOR (dev)) {
#if defined CONFIG_BLK_DEV_HD_ONLY
	case HD_MAJOR:
	{
		extern unsigned int hd_info[2][6];

		g_head = hd_info[first_minor >> hd->minor_shift][0];
		g_sect = hd_info[first_minor >> hd->minor_shift][1];
		break;
	}
#endif /* CONFIG_BLK_DEV_HD_ONLY */
#if defined CONFIG_BLK_DEV_IDEDISK || defined CONFIG_BLK_DEV_IDEDISK_MODULE
	case IDE0_MAJOR:
	case IDE1_MAJOR:
	case IDE2_MAJOR:
	case IDE3_MAJOR:
	case IDE4_MAJOR:
	case IDE5_MAJOR:
	case IDE6_MAJOR:
	case IDE7_MAJOR:
	case IDE8_MAJOR:
	case IDE9_MAJOR:
	{
		ide_drive_t *drive = (((ide_hwif_t *) hd->real_devices)->drives
				      + (first_minor >> hd->minor_shift));
		g_head = drive->head;
		g_sect = drive->sect;
		break;
	}
#endif /* CONFIG_BLK_DEV_IDEDISK(_MODULE) */
#if defined CONFIG_BLK_DEV_SD || defined CONFIG_BLK_DEV_SD_MODULE
	case SCSI_DISK0_MAJOR:
	case SCSI_DISK1_MAJOR:
	case SCSI_DISK2_MAJOR:
	case SCSI_DISK3_MAJOR:
	case SCSI_DISK4_MAJOR:
	case SCSI_DISK5_MAJOR:
	case SCSI_DISK6_MAJOR:
	case SCSI_DISK7_MAJOR:
	{
		Scsi_Disk *disk	= ((Scsi_Disk *) hd->real_devices
				   + (first_minor >> hd->minor_shift));
		struct Scsi_Host *host = disk->device->host;
		int diskinfo[3] = { 0, 0, 0 };

		if(host->hostt->bios_param)
			host->hostt->bios_param(disk, dev, diskinfo);
		else
			scsicam_bios_param(disk, dev, diskinfo);

		if ((g_head = diskinfo[0]) <= 0)
			g_head = 8;
		if ((g_sect = diskinfo[1]) <= 0)
			g_sect = 17;
		break;
	}
#endif /* CONFIG_BLK_DEV_SD(_MODULE) */
	default:
		printk(" unsupported disk (major = %u)\n", MAJOR(dev));
		return 0;
	}

	if (!(bh = bread(dev, 0, sector_size * 2))) {
		if (warn_no_part)
			printk(" unable to read partition table\n");
		return -1;
	}

	/* magic(?) check */
	if (*(__u16 *) (bh->b_data + sector_size - 2)
	    != cpu_to_le16 (NEC98_PTABLE_MAGIC)) {
		bforget (bh);
		return 0;
	}

	if (!is_valid_nec98_partition_table ((struct nec98_partition *)
					     (bh->b_data + sector_size),
					     g_sect, g_head)) {
#if 0
		if (warn_no_part)
			printk (" partition table consistency check failed"
				" (not PC-9800 disk?)\n");
#endif
		bforget (bh);
		return 0;
	}

	part = (const struct nec98_partition *)(bh->b_data + sector_size);
	for (nr = 0; nr < 16; nr++, part++) {
		unsigned int start_sect, end_sect;

		if (part->mid == 0 || part->sid == 0)
			continue;

		if (nr)
			printk("     ");

		{	/* Print partition name. Fdisk98 might put NUL
			   characters in partition name... */

			int j;
			unsigned char *p;
			unsigned char buf[sizeof (part->name) * 2 + 1];

			for (p = buf, j = 0; j < sizeof (part->name); j++, p++)
				if ((*p = part->name[j]) < ' ') {
					*p++ = '^';
					*p = part->name[j] + '@';
				}

			*p = 0;
			printk(" <%s>", buf);
		}
		start_sect = (part->cyl * g_head + part->head) * g_sect
			+ part->sector;
		end_sect = (part->end_cyl + 1) * g_head * g_sect;
		if (end_sect <= start_sect) {
			printk(" (invalid partition info)\n");
			continue;
		}
		add_gd_partition(hd, first_minor,
				 start_sect, end_sect - start_sect);
#ifdef CONFIG_NEC98_BSD_DISKLABEL
		if ((*(__u16 *)&part->mid & 0x7F7F)
		    == NEC98_BSD_PARTITION_MID_SID) {
			printk("!");
			bsd_minors[nr_bsd_minors++] = first_minor;
		}
#endif
		{	/* Pretty size printing. */
			/* XXX sector size? */
			unsigned int psize = (end_sect - start_sect) / 2;
			int unit_char = 'K';

			if (psize > 99999) {
				psize >>= 10;
				unit_char = 'M';
			}
			printk(" %5d%cB (%5d-%5d)\n", 
			       psize, unit_char, part->cyl, part->end_cyl);
		}
		first_minor++;
	}
#ifdef CONFIG_NEC98_BSD_DISKLABEL
	current_minor = first_minor;
	for (i = 0; i < nr_bsd_minors; i++) {
		/* NEC98_BSD_PARTITION_MID_SID is not valid SYSIND for
		   IBM PC's MS-DOS partition table, so we simply pass
		   it to bsd_disklabel_partition;
		   it will just print `<bsd: ... >'. */
		bsd_disklabel_partition(hd, bsd_minors[i],
					*(__u16 *)&part->mid & 0x7F7F);
	}
#endif
	/* brelse(bh); */
	bforget(bh); /* XXX */

	return nr ? 1 : 0;
}

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
