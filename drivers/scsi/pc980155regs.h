#ifndef __PC980155REGS_H
#define __PC980155REGS_H

#include "wd33c93.h"

#define REG_ADDRST (base_io+0)
#define REG_CONTRL (base_io+2)
#define REG_CWRITE (base_io+4)
#define REG_STATRD (base_io+4)

#define WD_MEMORYBANK 0x30
#define WD_RESETINT   0x33

#if 0
#define WAIT() outb(0x00,0x5f)
#else
#define WAIT() do{}while(0)
#endif

static inline uchar read_wd33c93(wd33c93_regs *regp,uchar reg_num)
{
  uchar data;
  outb (reg_num, regp->SASR);
  WAIT();
  data = inb(regp->SCMD);
  WAIT();
  return data;
}

static inline uchar read_aux_stat(wd33c93_regs* regp){
  uchar result;
  result = inb(regp->SASR);
  WAIT();
  /*  printk("PC-9801-55: regp->SASR(%x) = %x\n", regp->SASR, result); */
  return result;
}
#define READ_AUX_STAT() read_aux_stat(regp)

static inline void write_wd33c93(wd33c93_regs *regp,uchar reg_num, uchar value)
{
  outb (reg_num, regp->SASR);
  WAIT();
  outb(value, regp->SCMD);
  WAIT();
}


#define write_wd33c93_cmd(regp,cmd) write_wd33c93(regp,WD_COMMAND,cmd)

static inline void write_wd33c93_count(wd33c93_regs *regp,unsigned long value)
{
   outb (WD_TRANSFER_COUNT_MSB, regp->SASR);
   WAIT();
   outb ((value >> 16) & 0xff, regp->SCMD);
   WAIT();
   outb ((value >> 8)  & 0xff, regp->SCMD);
   WAIT();
   outb ( value        & 0xff, regp->SCMD);
   WAIT();
}


static inline unsigned long read_wd33c93_count(wd33c93_regs *regp)
{
unsigned long value;

   outb (WD_TRANSFER_COUNT_MSB, regp->SASR);
   value = inb(regp->SCMD) << 16;
   value |= inb(regp->SCMD) << 8;
   value |= inb(regp->SCMD);
   return value;
}

static inline void write_wd33c93_cdb(wd33c93_regs *regp, unsigned int len, unsigned char cmnd[]){
  int i;
  outb (WD_CDB_1, regp->SASR);
  for (i=0; i<len; i++)
    outb (cmnd[i], regp->SCMD);
}

#define pc980155_int_enable(regp)  write_wd33c93(regp, WD_MEMORYBANK, read_wd33c93(regp, WD_MEMORYBANK) | 0x04)
#define pc980155_int_disable(regp) write_wd33c93(regp, WD_MEMORYBANK, read_wd33c93(regp, WD_MEMORYBANK) & ~0x04)

#endif
