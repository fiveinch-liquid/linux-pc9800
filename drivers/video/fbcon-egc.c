/*
 *  linux/drivers/video/fbcon-egc.c -- Low level frame buffer operations
 *					      for EGC 
 *
 * Copyright (C) 1999,2000 Satoshi YAMADA <slakichi@kmc.kyoto-u.ac.jp>
 *
 * Based on fbcon-vga-planes.c (C) 1999 Ben Pfaff <pfaffben@debian.org>
 *				    and Petr Vandrovec <VANDROVE@vc.cvut.cz>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file COPYING in the main directory of this
 * archive for more details.	
 */

#include <linux/module.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <linux/vt_buffer.h>

#include <asm/io.h>

#include <video/fbcon.h>
#include <video/fbcon-egc.h>

#define EGCIO_EGC_R0 0x4a0
#define EGCIO_EGC_R1 0x4a2
#define EGCIO_EGC_R2 0x4a4
#define EGCIO_EGC_R3 0x4a6
#define EGCIO_EGC_R4 0x4a8
#define EGCIO_EGC_R5 0x4aa
#define EGCIO_EGC_R6 0x4ac
#define EGCIO_EGC_R7 0x4ae

#define EGCIO_KANJI_SETMODE	0x68
#define EGCIO_KANJI_CG_LOW	0xa1
#define EGCIO_KANJI_CG_HI	0xa3
#define EGCIO_KANJI_CG_LR	0xa5

#define EGCRAM_KANJI_CG		0xa4000

#define KANJI_ACCESS		0x0b
#define KANJI_NORMAL		0x0a

#define NUM_CACHE		16 /* must be 2,4,8,16... */

typedef struct facecache_t
{
	u8  isenable;
	u16 chardata;
	u8  left[16];
	u8  right[16];
} facecache;

facecache fcache_kanji[NUM_CACHE];
int       fcache_kanji_lastuse = -1;
facecache fcache_ank[256];
u8	  fcache_num;

void grcg_enable(void)
{
	outb(0xcf,0x7c);
	isa_writeb(isa_readb(0x495)|0x80,0x0495);
}

void grcg_disable(void)
{
	outb(0x00,0x7c);
	isa_writeb(isa_readb(0x495)&0x7F,0x0495);
}

void egc_enable(void)
{
	outb(0x07,0x6a);	/* unprotected */
	outb(0x05,0x6a);	/* EGC mode */
	outb(0x06,0x6a);	/* protected */
	outw(0xfff0,EGCIO_EGC_R0);
	outw(0x00ff,EGCIO_EGC_R1);
	outw(0xffff,EGCIO_EGC_R4);
	outw(0x0000,EGCIO_EGC_R6);
	outw(0x000f,EGCIO_EGC_R7);
}

void egc_disable(void)
{
	outb(0x07,0x6a);	/* unprotected */
	outb(0x04,0x6a);	/* GRCG mode */
	outb(0x06,0x6a);	/* protected */
}

void egc_set_mask(int mask)
{
	outw(mask,EGCIO_EGC_R4);
}

void egc_set_ropcode(int rop)
{
	outw(0x2800|(rop&0xff),EGCIO_EGC_R2);
}

void egc_set_fgcolor(int clr)
{
	outw(0x40ff,EGCIO_EGC_R1);
	outw(clr&0xf,EGCIO_EGC_R3);
}

static u8 * egc_load_facedata_ank(struct display *p, u16 data)
{
	int i;
	u16 rdata[16];

	if ( fcache_ank[(unsigned int)(data & 0xff)].isenable ) {
		return fcache_ank[(unsigned int)(data & 0xff)].left;
	}

	outb_p( KANJI_ACCESS, EGCIO_KANJI_SETMODE );
	outb_p( 0, EGCIO_KANJI_CG_LOW );
	outb_p( data & 0xff, EGCIO_KANJI_CG_HI );
	
	/* wait for vsync */
	while (inb (0xa0) & 0x20);
	while (!(inb (0xa0) & 0x20));
	
	memcpy(rdata, phys_to_virt(EGCRAM_KANJI_CG), 32);
	for ( i = 0; i < 16; i++) {
		fcache_ank[(unsigned int)(data & 0xff)].left[i]
			 = (u8)(rdata[i] >> 8);
	}
	fcache_ank[(unsigned int)(data & 0xff)].isenable = 1;
	return fcache_ank[(unsigned int)(data & 0xff)].left;
}

static u8 * egc_load_facedata(struct display *p, u16 data)
{
	int i;
	int t;
	u8 kanji_high = (u8)(data & 0x007f);
	u16 rdata[16];
	if(!(data & 0xff00)) {
//		return p->fontdata + (data & 0xff) * (fontheight(p));
		return egc_load_facedata_ank(p, data);
	}
	for ( i = 0 ; i < NUM_CACHE ; i++ ) {
		if (fcache_kanji[i].isenable
		 && fcache_kanji[i].chardata == (data & 0xff7f)) {
			fcache_kanji_lastuse = i;
			return ((data & 0x80) ? (fcache_kanji[i].right)
					      : (fcache_kanji[i].left));
		}
	}

	do {
		t = fcache_num;
		fcache_num = (fcache_num + 1) & (NUM_CACHE - 1);
	} while ( t == fcache_kanji_lastuse);
	fcache_kanji[t].isenable = 0;
	fcache_kanji[t].chardata = ( data & 0xff7f );

	outb_p(KANJI_ACCESS, EGCIO_KANJI_SETMODE);
	outb_p( ( data >> 8 ) & 0x7f, EGCIO_KANJI_CG_LOW);
	outb_p(kanji_high , EGCIO_KANJI_CG_HI);
	
	if( (kanji_high >=0x0c && kanji_high <= 0x0f) 
	 || kanji_high == 0x56
	 || (kanji_high >=0x59 && kanji_high <= 0x5c)) {
		/* read left */
		outb_p(0x20, EGCIO_KANJI_CG_LR);
		memcpy(rdata, phys_to_virt(EGCRAM_KANJI_CG), 32);
		for ( i = 0; i < 16; i++) {
			fcache_kanji[t].left[i] = (u8)(rdata[i] >> 8);
		}

		/* read right */
		outb_p(KANJI_ACCESS, EGCIO_KANJI_SETMODE);
		outb_p( ( data >> 8 ) & 0x7f, EGCIO_KANJI_CG_LOW);
		outb_p(0x00, EGCIO_KANJI_CG_LR);

		memcpy(rdata, phys_to_virt(EGCRAM_KANJI_CG), 32);
		for ( i = 0; i < 16; i++) {
			fcache_kanji[t].right[i] = (u8)(rdata[i] >> 8);
		}
	} else {
		/* read L/R pattern */
		memcpy(rdata, phys_to_virt(EGCRAM_KANJI_CG), 32);
		for ( i = 0; i < 16; i++) {
			fcache_kanji[t].left[i] = (u8)(rdata[i] & 0x00ff);
			fcache_kanji[t].right[i] = (u8)(rdata[i] >> 8);
		}
	}
	outb_p(KANJI_NORMAL, EGCIO_KANJI_SETMODE);
	fcache_kanji[t].isenable = 1;
	return ((data & 0x80) ? (fcache_kanji[t].right)
			     : (fcache_kanji[t].left));
}

/* ------------------------------ */

void fbcon_egc_setup(struct display *p)
{
	/* Nothing to do. */
}

void fbcon_egc_bmove(struct display *p, int sy, int sx, int dy, int dx,
		   int height, int width)
{
	u16 *src;
	u16 *dest;
	
	int x;
	int word_cnt;

	sy *= fontheight(p);
	dy *= fontheight(p);
	sx *= 8;
	dx *= 8;
	width *= 8;
	height *= fontheight(p);

	grcg_enable();
	egc_enable();
	outw(0x29F0,EGCIO_EGC_R2);

	if (dy < sy || (dy == sy && dx < sx)) {
		/* forward */
		int y;
		u16 *save1;
		u16 *save2;
		src = (u16 *)(((unsigned long)p->screen_base+(sx>>3)+
			       sy*p->line_length) & (~1)) ;
		dest = (u16 *)(((unsigned long)p->screen_base+(dx>>3)+
				dy*p->line_length) & (~1));
		
		outw((sx&0xf)|((dx&0xf)<<4),EGCIO_EGC_R6);
		word_cnt=((dx&0xf)+width+15)>>4;
		if((sx&0xf) != (dx&0xf) && ((sx&0xf) > (dx&0xf) || 
		(((sx&0xf)+width+15)>>4) > (((dx&0xf)+width+15)>>4))) {
			word_cnt++;
			dest=(u16 *)((unsigned long)dest-2);
		}
		outw(width-1,EGCIO_EGC_R7);
		for(y=0;y<height;y++) {
			save1=src;
			save2=dest;
			for(x=0;x<word_cnt;x++) {
				*dest=*src;
				src=(u16 *)((unsigned long)src+2);
				dest=(u16 *)((unsigned long)dest+2);
			}
			src=(u16 *)((unsigned long)save1+80);
			dest=(u16 *)((unsigned long)save2+80);
		}
	} else {
		/* backward */
		int y;
		int sb,db;
		u16 *save1;
		u16 *save2;
		src  = (u16 *)(((unsigned long)p->screen_base +
				((sy+height-1) * p->line_length +
				 ((sx+width-1)>>3))) & (~1));
		dest = (u16 *)(((unsigned long)p->screen_base +
				((dy+height-1) * p->line_length +
				 ((dx+width-1)>>3))) & (~1));
		outw(width-1,EGCIO_EGC_R7);

		sb=((((sx+width)&0xf)-16)*(-1))&0xf;
		db=((((dx+width)&0xf)-16)*(-1))&0xf;

		outw(0x1000|(db<<4)|sb,EGCIO_EGC_R6);
		word_cnt=(db+width+15)>>4;

		if(sb>db || (sb<db &&
		   (sb+width+15)>>4 > (db+width+15)>>4)) {
			word_cnt++;
			dest=(u16 *)((unsigned long)dest+2);
		}

		for(y=0;y<height;y++) {
			save1=src;
			save2=dest;
			for(x=0;x<word_cnt;x++) {
				*dest=*src;
				src=(u16 *)((unsigned long)src-2);
				dest=(u16 *)((unsigned long)dest-2);
			}
			src=(u16 *)((unsigned long)save1-80);
			dest=(u16 *)((unsigned long)save2-80);
		}
	}
	egc_disable();
	grcg_disable();
}

void fbcon_egc_clear(struct vc_data *conp, struct display *p,
			    int sy, int sx, int height, int width)
{
	u16 *data;
	int x;

	grcg_enable();
	egc_enable();

	egc_set_fgcolor((conp)?(conp->vc_video_erase_attr>>4):(0));
	outw(0x2cac,EGCIO_EGC_R2);
	
	sy *= fontheight(p);
	height *= fontheight(p);

	
	data = (u16 *)((unsigned long)p->screen_base + 0x8000 + sx +
		       sy * p->line_length);
	data = (u16 *)((unsigned long)data & (~1));
	while (height--) {
		u16 *save=data;
		/* first 8pixels */
		if(sx&1) {
			*data=0xff00;
			data++;
		}
		for (x = 0; x < (width&(~1)) -1; x+=2) {
			*data=0xffff;
			data++;
		}
		/* last 8pixels */
		if((sx+width)&1) {
			*data=0x00ff;
			data++;
		}
		/* to next line */
		data = (u16 *)((unsigned long)save + p->line_length);
	}
	egc_disable();
	grcg_disable();
}


void fbcon_egc_putc(struct vc_data *conp, struct display *p, int ch, int yy, int xx)
{
	u16 *data = (u16 *)((unsigned long)p->screen_base + xx +
			    yy * p->line_length * fontheight(p));
	
	u8 *fdata_s = egc_load_facedata(p, ch);
	int fg = conp->vc_pc98_addbuf & 0x0f;
	int bg = (conp->vc_pc98_addbuf >> 4) & 0x0f;
	int isbold = conp->vc_pc98_addbuf & 0x100;
	int isuline = conp->vc_pc98_addbuf & 0x200;
	int y;
	
	data = (u16 *)((unsigned long)data & (~1));
	grcg_enable();
	egc_enable();
	outw(0x2cac,EGCIO_EGC_R2);
	/* write one character */
	for (y = 0; y < fontheight(p); y++) {
		u16 mask=(*(fdata_s+y))<<((xx&1)<<3);
		if (isbold)
			mask |= (mask << 1);
		if ( y == fontheight(p) - 1 && isuline)
			mask = (0xff) << ((xx&1)<<3) ;
		egc_set_fgcolor(fg);
		*data = mask;
		if(isbold && !(xx&2) && xx!=0 && *(fdata_s+y)&0x80) {
			*(data-1) = 0x0100;
		}
		egc_set_fgcolor(bg);
		mask ^= 0xff<<((xx&1)<<3);
		*data = mask;
		data = (u16 *)((unsigned long)data + p->line_length);
	}
	egc_disable();
	grcg_disable();
}

void fbcon_egc_putcs(struct vc_data *conp, struct display *p,
			    const unsigned short *s,
			    int count, int yy, int xx)
{
	u16 *data = (u16 *)((unsigned long)p->screen_base +
			    xx + yy * p->line_length * fontheight(p));
	
#ifdef CONFIG_PC9800
	int attr = scr_readw((u16 *)((unsigned long)s
		 +conp->vc_screenbuf_size));
	int fg = attr & 0x0f;
	int bg = ( attr >> 4) & 0x0f;
	int isbold = attr & 0x100;
	int isuline = attr & 0x200;
#else
	int fg = scr_readw(s);
	int bg = scr_readw(s)>>4;
#endif
	int n,y;
	data = (u16 *)((unsigned long)data & ~1);
	
	grcg_enable();
	egc_enable();
	outw(0x2cac,EGCIO_EGC_R2);
	
	if(xx & 1) {
		/* write one character */
		int c = scr_readw(s++);
		u8 *fdata_s = egc_load_facedata(p, c);
		for (y = 0; y < fontheight(p); y++) {
			u16 mask=(*(fdata_s+y))<<8;
			if (isbold)
				mask |= (mask << 1);
			if ( y == fontheight(p) - 1 && isuline)
				mask = 0xff00 ;
			egc_set_fgcolor(fg);
			*data = mask;
			egc_set_fgcolor(bg);
			mask ^= 0xff00;
			*data = mask;
			data = (u16 *)((unsigned long)data+p->line_length);
		}
		data = (u16 *)((unsigned long)data + 2 -
			       p->line_length * fontheight(p));
		xx++;
		count--;
	}

	for (n = 0; n < (count&(~1)) - 1 ; n += 2 ) {
		int c,i;
		u8 *fdata_s[2];
		for(i=0;i<2;i++) {
			c = scr_readw(s++);
			fdata_s[i] = egc_load_facedata(p, c);
		}
		for (y = 0; y < fontheight(p); y++) {
			u16 mask=((*(fdata_s[1]+y))<<8)|(*(fdata_s[0]+y));
			if (isbold)
				mask |= (((mask << 1) & 0xfeff)|(mask >> 15));
			if ( y == fontheight(p) - 1 && isuline)
				mask = 0xffff ;
			egc_set_fgcolor(fg);
			*data = mask;
			if (isbold && !(xx&2) && xx!=0
			&&  *(fdata_s[0]+y)&0x80) {
				*(data-1) = 0x0100;
			}
			egc_set_fgcolor(bg);
			mask ^= 0xffff;
			*data = mask;
			data = (u16 *)((unsigned long)data + p->line_length);
		}
		data = (u16 *)((unsigned long)data + 2 -
			       p->line_length * fontheight(p));
	}
	if (count &1) {
		/* write one character */
		int c = scr_readw(s++);
		u8 *fdata_s = egc_load_facedata(p, c);
		for (y = 0; y < fontheight(p); y++) {
			u16 mask=(*(fdata_s+y));
			if (isbold)
				mask |= (mask << 1);
			if ( y == fontheight(p) - 1 && isuline)
				mask = 0x00ff ;
			mask &= 0x00ff;
			egc_set_fgcolor(fg);
			if(isbold && !(xx&2) && xx!=0 && *(fdata_s+y)&0x80) {
				*(data-1) = 0x0100;
			}
			*data = mask;
			egc_set_fgcolor(bg);
			mask ^= 0xff;
			*data = mask;
			data = (u16 *)((unsigned long)data + p->line_length);
		}
	}
	egc_disable();
	grcg_disable();
}

void fbcon_egc_revc(struct display *p, int xx, int yy)
{
	u16 *data = (u16 *)((unsigned long)p->screen_base + xx + 0x8000 +
			    yy * p->line_length * fontheight(p));
	int y;
	data = (u16 *)((unsigned long)data & (~1));
	grcg_enable();
	egc_enable();
	egc_set_ropcode(0x33);
	egc_set_mask( 0xFF<< ((xx&1)<<3) );
	
	for (y = 0; y < fontheight(p); y++) {
		*data = 0xAAAA;
		data = (u16 *)((unsigned long)data +p->line_length);
	}
	egc_disable();
	grcg_disable();
}

struct display_switch fbcon_egc = {
    fbcon_egc_setup, fbcon_egc_bmove, fbcon_egc_clear,
    fbcon_egc_putc, fbcon_egc_putcs, fbcon_egc_revc,
    NULL, NULL, NULL, FONTWIDTH(8)
};

#ifdef MODULE
int init_module(void)
{
    return 0;
}

void cleanup_module(void)
{
	/* Nothing to do. */
}
#endif /* MODULE */


    /*
     *	Visible symbols for modules
     */

EXPORT_SYMBOL(fbcon_egc);
EXPORT_SYMBOL(fbcon_egc_setup);
EXPORT_SYMBOL(fbcon_egc_bmove);
EXPORT_SYMBOL(fbcon_egc_clear);
EXPORT_SYMBOL(fbcon_egc_putc);
EXPORT_SYMBOL(fbcon_egc_putcs);
EXPORT_SYMBOL(fbcon_egc_revc);

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

