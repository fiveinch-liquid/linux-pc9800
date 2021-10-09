#ifndef _I386_SCATTERLIST_H
#define _I386_SCATTERLIST_H

#include <linux/config.h>

struct scatterlist {
    char *  address;    /* Location data is to be transferred to */
    char * alt_address; /* Location of actual if address is a 
			 * dma indirect buffer.  NULL otherwise */
    unsigned int length;
};

#ifdef CONFIG_PC9800
#define ISA_DMA_THRESHOLD (0xffffffff)
#else
#define ISA_DMA_THRESHOLD (0x00ffffff)
#endif

#endif /* !(_I386_SCATTERLIST_H) */
