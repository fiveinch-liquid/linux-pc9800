/*
 *  PC-9801-55 SCSI host adapter driver
 *
 *  Copyright (C) 1997-2000  Kyoto University Microcomputer Club
 *			     (Linux/98 project)
 */

#ifndef _SCSI_PC9801_55_H
#define _SCSI_PC9801_55_H

#include <linux/types.h>
#include <linux/kdev_t.h>
#include <scsi/scsicam.h>

int wd33c93_queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int wd33c93_abort(Scsi_Cmnd *);
int wd33c93_reset(Scsi_Cmnd *, unsigned int);
int scsi_pc980155_detect(Scsi_Host_Template *);
int pc980155_proc_info(char *, char **, off_t, int, int, int);

#ifndef CMD_PER_LUN
#define CMD_PER_LUN 2
#endif

#ifndef CAN_QUEUE
#define CAN_QUEUE 16
#endif

#define SCSI_PC980155 {	proc_name:		"PC-9801-55",		\
  			name:			"SCSI PC-9801-55",	\
			proc_info:		pc980155_proc_info,	\
			detect:			scsi_pc980155_detect,	\
			/* command: use queue command */		\
			queuecommand:		wd33c93_queuecommand,	\
			abort:			wd33c93_abort,		\
			reset:			wd33c93_reset,		\
			bios_param:		pc9800_scsi_bios_param,	\
			can_queue:		CAN_QUEUE,		\
			this_id:		7,			\
			sg_tablesize:		SG_ALL,			 \
			cmd_per_lun:		CMD_PER_LUN, /* dont use link command */ \
			unchecked_isa_dma:	1, /* use dma **XXXX***/ \
			use_clustering:		ENABLE_CLUSTERING }

#endif /* _SCSI_PC9801_55_H */
