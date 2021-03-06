
/* 
 * File...........: linux/drivers/s390/block/dasd_fba.c
 * Author(s)......: Holger Smolinski <Holger.Smolinski@de.ibm.com>
 * Bugreports.to..: <Linux390@de.ibm.com>
 * (C) IBM Corporation, IBM Deutschland Entwicklung GmbH, 1999,2000
 *          fixed partition handling and HDIO_GETGEO
 */

#include <linux/config.h>
#include <linux/stddef.h>
#include <linux/kernel.h>
#include <asm/debug.h>

#include <linux/malloc.h>
#include <linux/hdreg.h>	/* HDIO_GETGEO                      */
#include <linux/blk.h>
#include <asm/ccwcache.h>
#include <asm/idals.h>
#include <asm/dasd.h>

#include <asm/ebcdic.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/s390dyn.h>

#include "dasd_fba.h"

#ifdef PRINTK_HEADER
#undef PRINTK_HEADER
#endif				/* PRINTK_HEADER */
#define PRINTK_HEADER DASD_NAME"(fba):"

#define DASD_FBA_CCW_WRITE 0x41
#define DASD_FBA_CCW_READ 0x42
#define DASD_FBA_CCW_LOCATE 0x43
#define DASD_FBA_CCW_DEFINE_EXTENT 0x63

dasd_discipline_t dasd_fba_discipline;

typedef struct
dasd_fba_private_t {
	dasd_fba_characteristics_t rdc_data;
} dasd_fba_private_t;

#ifdef CONFIG_DASD_DYNAMIC
static
devreg_t dasd_fba_known_devices[] =
{
	{
		ci:
		{hc:
		 {ctype:0x6310,
                  dtype:0x9336}},
		flag:(DEVREG_MATCH_CU_TYPE |
                      DEVREG_MATCH_DEV_TYPE| 
                      DEVREG_TYPE_DEVCHARS),
		oper_func:dasd_oper_handler
	},
	{
		ci:
		{hc:
		 {ctype:0x3880,
                  dtype:0x3370}},
		flag:(DEVREG_MATCH_CU_TYPE |
                      DEVREG_MATCH_DEV_TYPE| 
                      DEVREG_TYPE_DEVCHARS),
		oper_func:dasd_oper_handler
	}
};
#endif
static inline void
define_extent (ccw1_t * ccw, DE_fba_data_t * DE_data, int rw,
	       int blksize, int beg, int nr)
{
	memset (DE_data, 0, sizeof (DE_fba_data_t));
	ccw->cmd_code = DASD_FBA_CCW_DEFINE_EXTENT;
	ccw->count = 16;
	set_normalized_cda (ccw, __pa (DE_data));
	if (rw == WRITE)
		(DE_data->mask).perm = 0x0;
	else if (rw == READ)
		(DE_data->mask).perm = 0x1;
	else
		DE_data->mask.perm = 0x2;
	DE_data->blk_size = blksize;
	DE_data->ext_loc = beg;
	DE_data->ext_end = nr - 1;
}

static inline void
locate_record (ccw1_t * ccw, LO_fba_data_t * LO_data, int rw, int block_nr,
	       int block_ct)
{
	memset (LO_data, 0, sizeof (LO_fba_data_t));
	ccw->cmd_code = DASD_FBA_CCW_LOCATE;
	ccw->count = 8;
	set_normalized_cda (ccw, __pa (LO_data));
	if (rw == WRITE)
		LO_data->operation.cmd = 0x5;
	else if (rw == READ)
		LO_data->operation.cmd = 0x6;
	else
		LO_data->operation.cmd = 0x8;
	LO_data->blk_nr = block_nr;
	LO_data->blk_ct = block_ct;
}

static int
dasd_fba_id_check (s390_dev_info_t * info)
{
        if (info->sid_data.cu_type == 0x3880)
                if (info->sid_data.dev_type == 0x3370)
                        return 0;
	if (info->sid_data.cu_type == 0x6310)
		if (info->sid_data.dev_type == 0x9336)
			return 0;
	return -ENODEV;
}

static int
dasd_fba_check_characteristics (struct dasd_device_t *device)
{
	int rc = -ENODEV;
	void *rdc_data;
	dasd_fba_private_t *private;

	if (device == NULL) {
		printk (KERN_WARNING PRINTK_HEADER
		   "Null device pointer passed to characteristics checker\n");
		return -ENODEV;
	}
        if ( device->private != NULL ) {
                kfree(device->private);
        }
        device->private = kmalloc (sizeof (dasd_fba_private_t), GFP_KERNEL);
	if (device->private == NULL) {
                printk (KERN_WARNING PRINTK_HEADER
                        "memory allocation failed for private data\n");
                return -ENOMEM;
	}
	private = (dasd_fba_private_t *) device->private;
	rdc_data = (void *) &(private->rdc_data);
	rc = read_dev_chars (device->devinfo.irq, &rdc_data, 32);
	if (rc) {
            printk (KERN_WARNING PRINTK_HEADER
                    "Read device characteristics returned error %d\n", rc);
            kfree(private);
            device->private=NULL;
            return rc;
	}
	printk (KERN_INFO PRINTK_HEADER
		"%04X on sch %d: %04X/%02X(CU:%04X/%02X) %dMB at(%d B/blk)\n",
		device->devinfo.devno, device->devinfo.irq,
                device->devinfo.sid_data.dev_type, device->devinfo.sid_data.dev_model,
                device->devinfo.sid_data.cu_type, device->devinfo.sid_data.cu_model,
		(private->rdc_data.blk_bdsa *
		 (private->rdc_data.blk_size >> 9)) >> 11,
		private->rdc_data.blk_size);
	return 0;
}

static int
dasd_fba_do_analysis (struct dasd_device_t *device)
{
	int rc = 0;
	int sb;
	dasd_fba_private_t *private = (dasd_fba_private_t *) device->private;
	int bs = private->rdc_data.blk_size;

	memset (&(device->sizes), 0, sizeof (dasd_sizes_t));
	switch (bs) {
	case 512:
	case 1024:
	case 2048:
	case 4096:
		device->sizes.bp_block = bs;
		break;
	default:
		printk (KERN_INFO PRINTK_HEADER
			"/dev/%s (%04X): unknown blocksize %d\n",
			device->name, device->devinfo.devno, bs);
		return -EMEDIUMTYPE;
	}
	device->sizes.s2b_shift = 0;	/* bits to shift 512 to get a block */
	for (sb = 512; sb < bs; sb = sb << 1)
		device->sizes.s2b_shift++;

	device->sizes.blocks = (private->rdc_data.blk_bdsa);
        device->sizes.pt_block = 1;

	return rc;
}

static int
dasd_fba_fill_geometry (struct dasd_device_t *device, struct hd_geometry *geo)
{
	int rc = 0;
	unsigned long sectors = device->sizes.blocks << device->sizes.s2b_shift;
	unsigned long tracks = sectors >> 6;
	unsigned long cyls = tracks >> 4;

	switch (device->sizes.bp_block) {
	case 512:
	case 1024:
	case 2048:
	case 4096:
		break;
	default:
		return -EINVAL;
	}
	geo->cylinders = cyls;
	geo->heads = 16;
	geo->sectors = 128 >> device->sizes.s2b_shift;
	return rc;
}

static dasd_era_t
dasd_fba_examine_error (ccw_req_t * cqr, devstat_t * stat)
{
	dasd_device_t *device = (dasd_device_t *) cqr->device;
	if (stat->cstat == 0x00 &&
	    stat->dstat == (DEV_STAT_CHN_END | DEV_STAT_DEV_END))
		return dasd_era_none;

	switch (device->devinfo.sid_data.dev_model) {
	case 0x3370:
		return dasd_3370_erp_examine (cqr, stat);
	case 0x9336:
		return dasd_9336_erp_examine (cqr, stat);
	default:
		return dasd_era_recover;
	}
}

static dasd_erp_action_fn_t
dasd_fba_erp_action (ccw_req_t * cqr)
{
	return default_erp_action;
}

static dasd_erp_postaction_fn_t
dasd_fba_erp_postaction (ccw_req_t * cqr)
{
	if (cqr->function == default_erp_action)
		return default_erp_postaction;
	printk (KERN_WARNING PRINTK_HEADER
		"unknown ERP action %p\n",
		cqr->function);
	return NULL;
}

static ccw_req_t *
dasd_fba_build_cp_from_req (dasd_device_t * device, struct request *req)
{
	ccw_req_t *rw_cp = NULL;
	int rw_cmd;
	int bhct, i;
	long size;
	ccw1_t *ccw;
	DE_fba_data_t *DE_data;
	LO_fba_data_t *LO_data;
	struct buffer_head *bh;
	dasd_fba_private_t *private = (dasd_fba_private_t *) device->private;
	int byt_per_blk = device->sizes.bp_block;

	if (req->cmd == READ) {
		rw_cmd = DASD_FBA_CCW_READ;
	} else if (req->cmd == WRITE) {
		rw_cmd = DASD_FBA_CCW_WRITE;
	} else {
		PRINT_ERR ("Unknown command %d\n", req->cmd);
		return NULL;
	}
	/* Build the request */
	/* count bhs to prevent errors, when bh smaller than block */
	bhct = 0;
	for (bh = req->bh; bh; bh = bh->b_reqnext) {
		if (bh->b_size > byt_per_blk)
			for (size = 0; size < bh->b_size; size += byt_per_blk)
				bhct++;
		else
			bhct++;
	}

	rw_cp = dasd_alloc_request (dasd_fba_discipline.name,
				    1 + 2*bhct,
				    sizeof (DE_fba_data_t) +
				    bhct*sizeof (LO_fba_data_t));
	if (!rw_cp) {
		return NULL;
	}
	DE_data = rw_cp->data;
	LO_data = rw_cp->data + sizeof (DE_fba_data_t);
	ccw = rw_cp->cpaddr;

	define_extent (ccw, DE_data, req->cmd, byt_per_blk,
		       req->sector, req->nr_sectors);
	ccw->flags |= CCW_FLAG_CC;

	for (i = 0, bh = req->bh; bh;) {
		if (bh->b_size > byt_per_blk) {
			for (size = 0; size < bh->b_size; size += byt_per_blk) {
                                ccw++;
                                locate_record (ccw, LO_data, req->cmd, i, 1);
                                ccw->flags |= CCW_FLAG_CC;
				ccw++;
                                ccw->flags |= CCW_FLAG_CC|CCW_FLAG_SLI;
				ccw->cmd_code = rw_cmd;
				ccw->count = byt_per_blk;
				set_normalized_cda (ccw, __pa (bh->b_data + size));
                                i++;
                                LO_data++;
			}
			bh = bh->b_reqnext;
		} else {	/* group N bhs to fit into byt_per_blk */
			for (size = 0; bh != NULL && size < byt_per_blk;) {
                                ccw++;
                                locate_record (ccw, LO_data, req->cmd, i, 1);
                                ccw->flags |= CCW_FLAG_CC;
				ccw++;
				if (private->rdc_data.mode.bits.data_chain) {
					ccw->flags |= CCW_FLAG_DC|CCW_FLAG_SLI;
				} else {
					PRINT_WARN ("Cannot chain chunks smaller than one block\n");
					ccw_free_request (rw_cp);
					return NULL;
				}
				ccw->cmd_code = rw_cmd;
				ccw->count = bh->b_size;
				set_normalized_cda (ccw, __pa (bh->b_data));
				size += bh->b_size;
				bh = bh->b_reqnext;
                                i++;
                                LO_data++;
			}
			ccw->flags &= ~CCW_FLAG_DC;
			ccw->flags |= CCW_FLAG_CC|CCW_FLAG_SLI;
			if (size != byt_per_blk) {
				PRINT_WARN ("Cannot fulfill request smaller than block\n");
				ccw_free_request (rw_cp);
				return NULL;
			}
		}
	}
	ccw->flags &= ~(CCW_FLAG_DC | CCW_FLAG_CC);

	rw_cp->device = device;
	rw_cp->expires = 5 * TOD_MIN;		/* 5 minutes */
	rw_cp->req = req;
	check_then_set (&rw_cp->status, CQR_STATUS_EMPTY, CQR_STATUS_FILLED);
	return rw_cp;
}

static char *
dasd_fba_dump_sense (struct dasd_device_t *device, ccw_req_t * req)
{
	char *page = (char *) get_free_page (GFP_KERNEL);
	int len;
	if (page == NULL) {
		return NULL;
	}
	len = sprintf (page, KERN_WARNING PRINTK_HEADER
		       "device %04X on irq %d: I/O status report:\n",
		       device->devinfo.devno, device->devinfo.irq);

	return page;
}

dasd_discipline_t dasd_fba_discipline =
{
	name:"FBA ",
	ebcname:"FBA ",
	max_blocks:((PAGE_SIZE >> 1)/sizeof(ccw1_t)-1),
	id_check:dasd_fba_id_check,
	check_characteristics:dasd_fba_check_characteristics,
	do_analysis:dasd_fba_do_analysis,
	fill_geometry:dasd_fba_fill_geometry,
	start_IO:dasd_start_IO,
	examine_error:dasd_fba_examine_error,
	erp_action:dasd_fba_erp_action,
	erp_postaction:dasd_fba_erp_postaction,
	build_cp_from_req:dasd_fba_build_cp_from_req,
	dump_sense:dasd_fba_dump_sense,
	int_handler:dasd_int_handler
};


int
dasd_fba_init (void)
{
	int rc = 0;
	printk (KERN_INFO PRINTK_HEADER
		"%s discipline initializing\n", dasd_fba_discipline.name);
	ASCEBC (dasd_fba_discipline.ebcname, 4);
	dasd_discipline_enq (&dasd_fba_discipline);
#ifdef CONFIG_DASD_DYNAMIC 
        {
            int i;
            for (i = 0; i < sizeof (dasd_fba_known_devices) / sizeof (devreg_t); i++) {
		printk (KERN_INFO PRINTK_HEADER
			"We are interested in: CU %04X/%02x\n",
			dasd_fba_known_devices[i].ci.hc.ctype,
			dasd_fba_known_devices[i].ci.hc.cmode);
		s390_device_register (&dasd_fba_known_devices[i]);
            }
        }
#endif				/* CONFIG_DASD_DYNAMIC */
        return rc;
}

void
dasd_fba_cleanup( void ) {
        printk ( KERN_INFO PRINTK_HEADER
                 "%s discipline cleaning up\n", dasd_fba_discipline.name);
#ifdef CONFIG_DASD_DYNAMIC
        {
	int i;
        for ( i=0; i<sizeof(dasd_fba_known_devices)/sizeof(devreg_t); i++) {
                printk (KERN_INFO PRINTK_HEADER
                        "We were interested in: CU %04X/%02x\n",
                        dasd_fba_known_devices[i].ci.hc.ctype,
                        dasd_fba_known_devices[i].ci.hc.cmode);
                s390_device_unregister(&dasd_fba_known_devices[i]);
        }
        }
#endif /* CONFIG_DASD_DYNAMIC */
        dasd_discipline_deq(&dasd_fba_discipline);
}
/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-indent-level: 4 
 * c-brace-imaginary-offset: 0
 * c-brace-offset: -4
 * c-argdecl-indent: 4
 * c-label-offset: -4
 * c-continued-statement-offset: 4
 * c-continued-brace-offset: 0
 * indent-tabs-mode: nil
 * tab-width: 8
 * End:
 */
