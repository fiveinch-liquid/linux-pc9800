/*
 *  include/asm-s390/s390-gdbregs.h
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *
 *  used both by the linux kernel for remote debugging & gdb 
 */

#ifndef _S390_GDBREGS_H
#define _S390_GDBREGS_H

#ifdef __KERNEL__
#include <asm/s390-regs-common.h>
#else
#include <s390x/s390-regs-common.h>
#endif
#define S390_MAX_INSTR_SIZE 6
#define NUM_REGS      (2+NUM_GPRS+NUM_ACRS+NUM_CRS+1+NUM_FPRS)
#define FIRST_ACR     (2+NUM_GPRS)
#define LAST_ACR      (FIRST_ACR+NUM_ACRS-1)
#define FIRST_CR      (FIRST_ACR+NUM_ACRS)
#define LAST_CR       (FIRST_CR+NUM_CRS-1)

#define PSWM_REGNUM    0
#define PC_REGNUM      1
#define	GP0_REGNUM     2		    /* GPR register 0 */ 
#define GP_LAST_REGNUM (GP0_REGNUM+NUM_GPRS-1)
#define RETADDR_REGNUM (GP0_REGNUM+14)                   /* Usually return address */
#define SP_REGNUM      (GP0_REGNUM+15)	    /* Contains address of top of stack */
#define FP_REGNUM     SP_REGNUM /* needed in findvar.c still */
#define FRAME_REGNUM  (GP0_REGNUM+11)
#define FPC_REGNUM    (GP0_REGNUM+NUM_GPRS+NUM_ACRS+NUM_CRS)
#define FP0_REGNUM    (FPC_REGNUM+1) /* FPR (Floating point) register 0 */
#define FPLAST_REGNUM (FP0_REGNUM+NUM_FPRS-1)	/* Last floating point register */

/* The top of this structure is as similar as possible to a pt_regs structure to */
/* simplify code */
typedef struct
{
	S390_REGS_COMMON
	__u32         crs[NUM_CRS];
	s390_fp_regs  fp_regs;
} s390_gdb_regs __attribute__((packed));

#define REGISTER_NAMES                                           \
{                                                                \
"pswm","pswa",                                                   \
"gpr0","gpr1","gpr2","gpr3","gpr4","gpr5","gpr6","gpr7",         \
"gpr8","gpr9","gpr10","gpr11","gpr12","gpr13","gpr14","gpr15",   \
"acr0","acr1","acr2","acr3","acr4","acr5","acr6","acr7",         \
"acr8","acr9","acr10","acr11","acr12","acr13","acr14","acr15",   \
"cr0","cr1","cr2","cr3","cr4","cr5","cr6","cr7",                 \
"cr8","cr9","cr10","cr11","cr12","cr13","cr14","cr15",           \
"fpc",                                                           \
"fpr0","fpr1","fpr2","fpr3","fpr4","fpr5","fpr6","fpr7",         \
"fpr8","fpr9","fpr10","fpr11","fpr12","fpr13","fpr14","fpr15"    \
}

/* Index within `registers' of the first byte of the space for
   register N.  */

#define ACR0_OFFSET ((PSW_MASK_SIZE+PSW_ADDR_SIZE)+(GPR_SIZE*NUM_GPRS))
#define CR0_OFFSET (ACR0_OFFSET+(ACR_SIZE+NUM_ACRS))
#define FPC_OFFSET (CR0_OFFSET+(CR_SIZE*NUM_CRS))
#define FP0_OFFSET (FPC_OFFSET+(FPC_SIZE+FPC_PAD_SIZE))

#define REGISTER_BYTES    \
((FP0_OFFSET)+(FPR_SIZE*NUM_FPRS))

#define REGISTER_BYTE(N)  ((N)<=GP_LAST_REGNUM ? (N)*8: \
(N) <= LAST_ACR ? (ACR0_OFFSET+(((N)-FIRST_ACR)*ACR_SIZE)): \
(N) <= LAST_CR ? (CR0_OFFSET+(((N)-FIRST_CR)*CR_SIZE)): \
(N) == FPC_REGNUM ? FPC_OFFSET:(FP0_OFFSET+(((N)-FP0_REGNUM)*FPR_SIZE)))

#endif











