/*
 * linux/drivers/video/gdccon.c
 * Low level GDC based console driver for NEC PC-9800 series
 *
 * Created 24 Dec 1998 by Linux/98 project
 *
 * based on:
 * linux/drivers/video/vgacon.c in Linux 2.1.131 by Geert Uytterhoeven
 * linux/char/gdc.c in Linux/98 2.1.57 by Linux/98 project
 * linux/char/console.c in Linux/98 2.1.57 by Linux/98 project
 */

/*
#define VRAM_OVERRUN_DEBUG
*/

#ifdef VRAM_OVERRUN_DEBUG
# define NEED_UNMAP_PHYSPAGE
#endif

#include <linux/config.h>
#include <linux/types.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/console_struct.h>
#include <linux/string.h>
#include <linux/kd.h>
#include <linux/slab.h>
#include <linux/vt_kern.h>
#include <linux/selection.h>
#include <linux/ioport.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/pc9800.h>

#ifdef VRAM_OVERRUN_DEBUG
# include <asm/pc9800_debug.h>
#endif

#define BLANK 0x0020
#define BLANK_ATTR 0x00e1

/* GDC/GGDC port# */
#define GDC_COMMAND 0x62
#define GDC_PARAM 0x60
#define GDC_STAT 0x60
#define GDC_DATA 0x62

#define MODE_FF1	(0x0068)	/* mode F/F register 1 */

#define  MODE_FF1_ATR_SEL	(0x00)	/* 0: vertical line 1: 8001 graphic */
#define  MODE_FF1_GRAPHIC_MODE	(0x02)	/* 0: color 1: mono */
#define  MODE_FF1_COLUMN_WIDTH	(0x04)	/* 0: 80col 1: 40col */
#define  MODE_FF1_FONT_SEL	(0x06)	/* 0: 6x8 1: 7x13 */
#define  MODE_FF1_GRP_MODE	(0x08)	/* 0: display odd-y raster 1: not */
#define  MODE_FF1_KAC_MODE	(0x0A)	/* 0: code access 1: dot access */
#define  MODE_FF1_NVMW_PERMIT	(0x0C)	/* 0: protect 1: permit */
#define  MODE_FF1_DISP_ENABLE	(0x0E)	/* 0: enable 1: disable */

#define GGDC_COMMAND 0xA2
#define GGDC_PARAM 0xA0
#define GGDC_STAT 0xA0
#define GGDC_DATA 0xA2

/* GDC status */
#define GDC_DATA_READY		(1 << 0)
#define GDC_FIFO_FULL		(1 << 1)
#define GDC_FIFO_EMPTY		(1 << 2)
#define GGDC_FIFO_EMPTY		GDC_FIFO_EMPTY
#define GDC_DRAWING		(1 << 3)
#define GDC_DMA_EXECUTE		(1 << 4)	/* nonsense on 98 */
#define GDC_VERTICAL_SYNC	(1 << 5)
#define GDC_HORIZONTAL_BLANK	(1 << 6)
#define GDC_LIGHTPEN_DETECT	(1 << 7)	/* nonsense on 98 */

#define ATTR_G		(1U << 7)
#define ATTR_R		(1U << 6)
#define ATTR_B		(1U << 5)
#define ATTR_GRAPHIC	(1U << 4)
#define ATTR_VERTBAR	ATTR_GRAPHIC	/* vertical bar */
#define ATTR_UNDERLINE	(1U << 3)
#define ATTR_REVERSE	(1U << 2)
#define ATTR_BLINK	(1U << 1)
#define ATTR_NOSECRET	(1U << 0)
#define AMASK_NOCOLOR	(ATTR_GRAPHIC | ATTR_UNDERLINE | ATTR_REVERSE \
			 | ATTR_BLINK | ATTR_NOSECRET)

#define CMD_STOP	(0x0C)
#define CMD_START	(0x0D)

/*
 *  Interface used by the world
 */
static const char *gdccon_startup(void);
static void gdccon_init(struct vc_data *c, int init);
static void gdccon_deinit(struct vc_data *c);
static void gdccon_cursor(struct vc_data *c, int mode);
static int gdccon_switch(struct vc_data *c);
static int gdccon_blank(struct vc_data *c, int blank);
static int gdccon_scrolldelta(struct vc_data *c, int lines);
static int gdccon_set_origin(struct vc_data *c);
static void gdccon_save_screen(struct vc_data *c);
static int gdccon_scroll(struct vc_data *c, int t, int b, int dir, int lines);
static u8 gdccon_build_attr(struct vc_data *c, u8 color, u8 intensity, u8 blink, u8 underline, u8 reverse);
static void gdccon_invert_region(struct vc_data *c, u16 *p, int count);
static unsigned long gdccon_uni_pagedir[2];

/* Description of the hardware situation */
static unsigned long   gdc_vram_base;		/* Base of video memory */
static unsigned long   gdc_vram_end;		/* End of video memory */
static unsigned int    gdc_video_num_columns;
						/* Number of text columns */
static unsigned int    gdc_video_num_lines;
						/* Number of text lines */
#if 0
static int	       gdc_can_do_color = 0;	/* Do we support colors? */
#else
static int	       gdc_can_do_color = 1;	/* Do we support colors? */
#endif
static unsigned char   gdc_video_type;		/* Card type */
static unsigned char   gdc_hardscroll_enabled;
static unsigned char   gdc_hardscroll_user_enable = 1;
static int	       gdc_vesa_blanked;
static unsigned int    gdc_rolled_over = 0;

#define DISP_FREQ_AUTO	0
#define DISP_FREQ_25k	1
#define DISP_FREQ_31k	2
static unsigned int	gdc_disp_freq __initdata = DISP_FREQ_AUTO;

static int fifo_data = 0; /* GDC FIFO capacity is 16 bytes */

static inline void
gdc_stupid_wait_vsync (void)
{
	while (inb (GDC_STAT) & GDC_VERTICAL_SYNC)
		;
	while (!(inb (GDC_STAT) & GDC_VERTICAL_SYNC))
		;
}

#define gdc_attr_offset(x) ((typeof(x))((unsigned long)(x)+0x2000))

void no_scroll(char *str, int *ints)
{
	/*
	 * Disabling scrollback is required for the Braillex ib80-piezo
	 * Braille reader made by F.H. Papenmeier (Germany).
	 * Use the "no-scroll" bootflag.
	 */
	gdc_hardscroll_user_enable = gdc_hardscroll_enabled = 0;
}

#define	gdc_outb(val, port)	outb_p ((val), (port))
#define	gdc_inb(port)		inb_p (port)

#define __gdc_write_command(cmd)	gdc_outb ((cmd), GDC_COMMAND)
#define __gdc_write_param(param)	gdc_outb ((param), GDC_PARAM)

static inline unsigned long gdc_wait_empty (void)
{
	unsigned long flags;

	while (1) {
		save_flags (flags);
		cli ();
		if (gdc_inb (GDC_STAT) & GDC_FIFO_EMPTY)
			break;
		restore_flags (flags);
	}
	return flags;
}

static inline void write_gdc_cmd (unsigned char cmd)
{
	unsigned long flags;

	/*
	 * ddprintk might set the console position from interrupt
	 * handlers, thus the write has to be IRQ-atomic.
	 */
	save_flags(flags);
	cli();

	if (fifo_data > 15){
		fifo_data = 0;
		while ((inb(GDC_STAT) & GDC_FIFO_EMPTY) == 0){;}
	}
	outb_p(cmd, GDC_COMMAND);
	fifo_data ++;

	restore_flags(flags);
}	

static inline void write_gdc_prm (unsigned char prm)
{
	unsigned long flags;

	/*
	 * ddprintk might set the console position from interrupt
	 * handlers, thus the write has to be IRQ-atomic.
	 */
	save_flags(flags);
	cli();

	if (fifo_data > 15){
		fifo_data = 0;
		while ((inb(GDC_STAT) & GDC_FIFO_EMPTY) == 0){;}
	}
	outb_p(prm, GDC_PARAM);
	fifo_data ++;

	restore_flags(flags);
}

static inline void write_ggdc_cmd (unsigned char cmd)
{
	unsigned long flags;

	/*
	 * ddprintk might set the console position from interrupt
	 * handlers, thus the write has to be IRQ-atomic.
	 */
	save_flags(flags);
	cli();

	if (fifo_data > 15){
		fifo_data = 0;
		while ((inb(GGDC_STAT) & GGDC_FIFO_EMPTY) == 0)
			;
	}
	outb_p(cmd, GGDC_COMMAND);
	fifo_data ++;

	restore_flags(flags);
}	

static inline void write_ggdc_prm (unsigned char prm)
{
	unsigned long flags;

	/*
	 * ddprintk might set the console position from interrupt
	 * handlers, thus the write has to be IRQ-atomic.
	 */
	save_flags(flags);
	cli();

	if (fifo_data > 15){
		fifo_data = 0;
		while ((inb(GGDC_STAT) & GGDC_FIFO_EMPTY) == 0){;}
	}
	outb_p(prm, GGDC_PARAM);
	fifo_data ++;

	restore_flags(flags);
}

static int gdccon_25k_mode(unsigned long lines)
{
	unsigned long hdots = lines * 16;

	outb_p(0x00, 0x9a8);   /* 24.83 KHz */

	write_gdc_cmd (0x0E);  /* SYNC, DE deny */
	write_gdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
	write_gdc_prm (0x4E);  /* C/R = 78 (80 chars) */
	write_gdc_prm (0x07);  /* VSL = 0(3) ; HS = 7 */
	write_gdc_prm (0x25);  /* HFP = 9    ; VSH = 1(VS=8) */
	write_gdc_prm (0x07);  /* DS, PH = 0 ; HBP = 7 */
	write_gdc_prm (0x07);  /* VH, VL = 0 ; VFP = 7 */
	write_gdc_prm (hdots & 0xff);  /* LFL */
	write_gdc_prm (0x64 | ((hdots >> 8) & 0x03));  /* VBP = 25   ; LFH */

	write_gdc_cmd (0x47);  /* PITCH */
	write_gdc_prm (0x50);

	write_gdc_cmd (0x70);  /* SCROLL */
	write_gdc_prm (0x00);
	write_gdc_prm (0x00);
	write_gdc_prm ((hdots << 4) & 0xf0);  /* SL1=592 (0x250) */
	write_gdc_prm ((hdots >> 4) & 0x3f);

	write_ggdc_cmd (0x0E);  /* SYNC */
	write_ggdc_prm (0x00);
	write_ggdc_prm (0x4E);
	write_ggdc_prm (0x07);
	write_ggdc_prm (0x25);
	write_ggdc_prm (0x07);
	write_ggdc_prm (0x07);
	write_ggdc_prm (hdots & 0xff);  /* LFL */
	write_ggdc_prm (0x64 | ((hdots >> 8) & 0x03));  /* VBP = 25   ; LFH */

	write_ggdc_cmd (0x47);  /* PITCH */ 
	write_ggdc_prm (0x28);

	return 0;
}

static int gdccon_31k_mode(unsigned long lines)
{
	unsigned long hdots = lines * 16;

	outb_p(0x01, 0x9a8);   /* 31.47KHz */

	write_gdc_cmd (0x0E);  /* SYNC, DE deny */
	write_gdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
	write_gdc_prm (0x4E);  /* C/R = 78 (80 chars) */
	write_gdc_prm (0x4B);  /* VSL = 2(3) ; HS = 11 */
	write_gdc_prm (0x0C);  /* HFP = 3    ; VSH = 0(VS=2) */
	write_gdc_prm (0x03);  /* DS, PH = 0 ; HBP = 3 */
	write_gdc_prm (0x06);  /* VH, VL = 0 ; VFP = 6 */
	write_gdc_prm (hdots & 0xff);  /* LFL */
	write_gdc_prm (0x94 | ((hdots >> 8) & 0x03));  /* VBP = 37   ; LFH */

	write_gdc_cmd (0x47);  /* PITCH */
	write_gdc_prm (0x50);

	write_gdc_cmd (0x70);  /* SCROLL */
	write_gdc_prm (0x00);
	write_gdc_prm (0x00);
	write_gdc_prm ((hdots << 4) & 0xf0);  /* SL1=592 (0x250) */
	write_gdc_prm ((hdots >> 4) & 0x3f);

	write_ggdc_cmd (0x0E);  /* SYNC, DE deny */
	write_ggdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
	write_ggdc_prm (0x4E);  /* C/R = 78 (80 chars) */
	write_ggdc_prm (0x4B);  /* VSL = 2(3) ; HS = 11 */
	write_ggdc_prm (0x0C);  /* HFP = 3    ; VSH = 0(VS=2) */
	write_ggdc_prm (0x03);  /* DS, PH = 0 ; HBP = 3 */
	write_ggdc_prm (0x06);  /* VH, VL = 0 ; VFP = 6 */
	write_ggdc_prm (hdots & 0xff);  /* LFL */
	write_ggdc_prm (0x94 | ((hdots >> 8) & 0x03));  /* VBP = 37   ; LFH */

	write_ggdc_cmd (0x47);  /* PITCH */ 
	write_ggdc_prm (0x28);

	return 0;
}

static const char * __init gdccon_startup(void)
{
	const char *display_desc = NULL;
	int defaultp = !gdc_disp_freq && !gdc_video_num_lines;

	if (!gdc_disp_freq)
		/* Use BIOS value.  */
		gdc_disp_freq = ORIG_VIDEO_MODE & 0x04
			? DISP_FREQ_31k : DISP_FREQ_25k;
	if (!gdc_video_num_lines)
		gdc_video_num_lines = ORIG_VIDEO_LINES;
	if (!gdc_video_num_columns)
		gdc_video_num_columns = ORIG_VIDEO_COLS;

	if (!defaultp) {
		write_gdc_cmd (CMD_STOP);
		write_ggdc_cmd (CMD_STOP);

		switch (gdc_disp_freq) {
		case DISP_FREQ_25k:
			gdccon_25k_mode (gdc_video_num_lines);
			break;
		case DISP_FREQ_31k:
			gdccon_31k_mode (gdc_video_num_lines);
			break;
		}

		write_gdc_cmd (CMD_START);
		write_ggdc_cmd (CMD_START);
	}

	gdc_vram_base = GDC_MAP_MEM(0xa0000);
	/* Last few bytes of text VRAM area are for NVRAM. */
	gdc_vram_end = GDC_MAP_MEM(0xa0000 + 0x1fe0);

	if (!PC9800_HIGHRESO_P ()) {
		gdc_video_type = VIDEO_TYPE_98NORMAL;
		display_desc = "NEC PC-9800 Normal";
	} else {
		gdc_video_type = VIDEO_TYPE_98HIRESO;
		display_desc = "NEC PC-9800 High Resolution";
	}

	gdc_hardscroll_enabled = gdc_hardscroll_user_enable;

	return display_desc;
}

#ifdef VRAM_OVERRUN_DEBUG
static int __init
gdccon_setup_trap (void)
{
	/*
	 * Trap scr_mem{move,set,...} overrun by unmapping memory page.
	 * If kernel hits these pages, page fault are triggered and
	 * then kernel forces oops.
	 */
	unmap_physpage (GDC_MAP_MEM (0x9f000));
	unmap_physpage (GDC_MAP_MEM (0xa6000));

	printk (KERN_DEBUG "gdccon: overrun trap code activated\n");
	return 0;
}

/*
 * Call gdccon_setup_trap() while normal driver setup, as gdccon_startup()
 * may be called while bootup temporary page table is in use. (Is this true?)
 */
__initcall (gdccon_setup_trap);
#endif

static void gdccon_init(struct vc_data *c, int init)
{
	unsigned long p;
	
	/* We cannot be loaded as a module, therefore init is always 1 */
	c->vc_can_do_color = gdc_can_do_color;
	c->vc_cols = gdc_video_num_columns;
	c->vc_rows = gdc_video_num_lines;
#ifdef CONFIG_PC9800
	c->vc_complement_mask = ATTR_REVERSE;
#else
	c->vc_complement_mask = 0x7700;
#endif
	p = *c->vc_uni_pagedir_loc;
	if (c->vc_uni_pagedir_loc == &c->vc_uni_pagedir ||
	    !--c->vc_uni_pagedir_loc[1])
		con_free_unimap(c->vc_num);
	c->vc_uni_pagedir_loc = gdccon_uni_pagedir;
#ifdef PC9800_GDCCON_DEBUG
	printk (KERN_DEBUG __FUNCTION__	": #%u: %scolor, %ux%u, uni %p\n",
		c->vc_num, "!" + !!c->vc_can_do_color,
		c->vc_cols, c->vc_rows, c->vc_uni_pagedir_loc);
#endif
	gdccon_uni_pagedir[1]++;
	if (!gdccon_uni_pagedir[0] && p)
		con_set_default_unimap(c->vc_num);
}

static inline void gdc_set_mem_top(struct vc_data *c)
{
	unsigned long origin = (c->vc_visible_origin - gdc_vram_base) / 2;
#if 1
	unsigned long flags;

	flags = gdc_wait_empty ();
	__gdc_write_command (0x70);			/* SCROLL */
	__gdc_write_param (origin);			/* SAD1 (L) */
	__gdc_write_param ((origin >> 8) & 0x1f);	/* SAD1 (H) */
	restore_flags (flags);
#else
	write_gdc_cmd(0x70);			/* SCROLL */
	write_gdc_prm(origin);			/* SAD1 Low */
	write_gdc_prm((origin >> 8) & 0x1f);	/* SAD1 High */
#endif
}

static void gdccon_deinit(struct vc_data *c)
{
	/* When closing the last console, reset video origin */
	if (!--gdccon_uni_pagedir[1]) {
		c->vc_visible_origin = gdc_vram_base;
		gdc_set_mem_top(c);
		con_free_unimap(c->vc_num);
	}
	c->vc_uni_pagedir_loc = &c->vc_uni_pagedir;
	con_set_default_unimap(c->vc_num);
}

#if 0
/* Translate ANSI terminal color code to GDC color code.  */
#define BGR_TO_GRB(bgr)	((((bgr) & 4) >> 2) | (((bgr) & 3) << 1))
#else
#define RGB_TO_GRB(rgb)	((((rgb) & 4) >> 1) | (((rgb) & 2) << 1) | ((rgb) & 1))
#endif

static const u8 gdccon_color_table[] = {
#define C(color)	((RGB_TO_GRB (color) << 5) | ATTR_NOSECRET)
	C(0), C(1), C(2), C(3), C(4), C(5), C(6), C(7)
#undef C
};

static u8 gdccon_build_attr(struct vc_data *c, u8 color, u8 intensity, u8 blink, u8 underline, u8 reverse)
{
	u8 attr = gdccon_color_table [color & 0x07];

	if (!gdc_can_do_color)
		attr = (intensity == 0 ? 0x61
			: intensity == 2 ? 0xE1 : 0xA1);
	if (underline)
		attr |= 0x08;

	/* ignore intensity */
#if 0
	if(intensity == 0)
		;
	else if (intensity == 2)
		attr |= 0x10; /* virtical line */
#else
	if (intensity == 0) {
		if (attr == c->vc_def_attr)
			attr = c->vc_half_attr;
		else
			attr |= c->vc_half_attr & AMASK_NOCOLOR;
	}
	else if (intensity == 2) {
		if (attr == c->vc_def_attr)
			attr = c->vc_bold_attr;
		else
			attr |= c->vc_bold_attr & AMASK_NOCOLOR;
	}
#endif
	if (reverse)
		attr |= ATTR_REVERSE;
	if ((color & 0x07) == 0) {	/* foreground color == black */
		/* Fake background color by reversed character
		   as GDC cannot set background color.  */
		attr |= gdccon_color_table[(color >> 4) & 0x07];
		attr ^= ATTR_REVERSE;
	}
	if (blink)
		attr |= ATTR_BLINK;

	return attr;
}

static void gdccon_invert_region(struct vc_data *c, u16 *p, int count)
{
	while (count--) {
		u16 a = scr_readw(gdc_attr_offset(p));

		a ^= ATTR_REVERSE;
		scr_writew(a, gdc_attr_offset(p));
		p++;
	}
}

static u8 gdc_csrform_lr = 15;			/* Lines/Row */
static u16 gdc_csrform_bl_bd = ((12 << 6)	/* BLinking Rate */
				| (0 << 5));	/* Blinking Disable */

static inline void
gdc_hide_cursor (void)
{
	__gdc_write_command (0x4b);		/* CSRFORM */
	__gdc_write_param (gdc_csrform_lr);	/* CS = 0, CE = 0, L/R = ? */
}

static inline void
gdc_show_cursor (int cursor_start, int cursor_finish)
{
	__gdc_write_command (0x4b);		/* CSRFORM */
	__gdc_write_param (0x80 | gdc_csrform_lr);		/* CS = 1 */
	__gdc_write_param (cursor_start | gdc_csrform_bl_bd);
	__gdc_write_param ((cursor_finish << 3) | (gdc_csrform_bl_bd >> 8));
}

static void gdccon_cursor(struct vc_data *c, int mode)
{
	unsigned long flags;
	u16 ead;

	if (c->vc_origin != c->vc_visible_origin)
		gdccon_scrolldelta(c, 0);

	flags = gdc_wait_empty ();
	switch (mode) {
	case CM_ERASE:
		gdc_hide_cursor ();
		break;

	case CM_MOVE:
	case CM_DRAW:
		switch (c->vc_cursor_type & 0x0f) {
		case CUR_UNDERLINE:
			gdc_show_cursor (14, 15);	/* XXX font height */
			break;
		case CUR_TWO_THIRDS:
			gdc_show_cursor (5, 15);	/* XXX */
			break;
		case CUR_LOWER_THIRD:
			gdc_show_cursor (11, 15);	/* XXX */
			break;
		case CUR_LOWER_HALF:
			gdc_show_cursor (8, 15);	/* XXX */
			break;
		case CUR_NONE:
			gdc_hide_cursor ();
			break;
          	default:
			gdc_show_cursor(0, 15);		/* XXX */
			break;
		}

		__gdc_write_command (0x49);		/* CSRW */
		ead = (c->vc_pos - gdc_vram_base) >> 1;
		__gdc_write_param (ead);
		__gdc_write_param ((ead >> 8) & 0x1f);
		break;
	}
	restore_flags (flags);
}

static int gdccon_switch(struct vc_data *c)
{
	/*
	 * We need to save screen size here as it's the only way
	 * we can spot the screen has been resized and we need to
	 * set size of freshly allocated screens ourselves.
	 */
	gdc_video_num_columns = c->vc_cols;
	gdc_video_num_lines = c->vc_rows;
#ifndef CONFIG_PC9800
	if (!vga_is_gfx)
		scr_memcpyw_to((u16 *) c->vc_origin, (u16 *) c->vc_screenbuf, c->vc_screenbuf_size);
#else
#if 0
	printk (KERN_DEBUG
		"%s: c=%p {origin=%#x, screenbuf=%#x, screenbuf_size=%u\n",
		__FUNCTION__, c,
		c->vc_origin, c->vc_screenbuf, c->vc_screenbuf_size);
#endif
	if (c->vc_origin != (unsigned long) c->vc_screenbuf
	    && gdc_vram_base <= c->vc_origin && c->vc_origin < gdc_vram_end) {
		scr_memcpyw_to ((u16 *) c->vc_origin,
				(u16 *) c->vc_screenbuf,
				c->vc_screenbuf_size);
		scr_memcpyw_to ((u16 *) gdc_attr_offset(c->vc_origin),
				(u16 *) ((char *) c->vc_screenbuf
					 + c->vc_screenbuf_size),
				c->vc_screenbuf_size);
	}
	else
		printk (KERN_WARNING
			"gdccon: switch (vc #%d) called on origin=%#lx\n",
			c->vc_num, c->vc_origin);
#endif
	return 0;	/* Redrawing not needed */
}

#ifndef CONFIG_PC9800
static void vga_set_palette(struct vc_data *c, unsigned char *table)
{
	int i, j ;

	for (i=j=0; i<16; i++) {
		outb_p (table[i], dac_reg) ;
		outb_p (c->vc_palette[j++]>>2, dac_val) ;
		outb_p (c->vc_palette[j++]>>2, dac_val) ;
		outb_p (c->vc_palette[j++]>>2, dac_val) ;
	}
}
#endif /* !CONFIG_PC9800 */

static int gdccon_set_palette(struct vc_data *c, unsigned char *table)
{
#if !defined(CONFIG_PC9800) && defined(CAN_LOAD_PALETTE)
	if (vga_video_type != VIDEO_TYPE_VGAC || vga_palette_blanked || !CON_IS_VISIBLE(c))
		return -EINVAL;
	vga_set_palette(c, table);
	return 0;
#else
	return -EINVAL;
#endif
}

#define RELAY0		0x01
#define RELAY0_GDC	0x00
#define RELAY0_ACCEL	0x01
#define RELAY1		0x02
#define RELAY1_INTERNAL	0x00
#define RELAY1_EXTERNAL	0x02
#define IO_RELAY	0x0fac
#define IO_DPMS		0x09a2
static unsigned char relay_mode = RELAY0_GDC | RELAY1_INTERNAL;

static void gdc_vesa_blank(int mode)
{
    unsigned long flags;
    unsigned char stat;

    save_flags(flags);
    cli();

    relay_mode = inb_p(IO_RELAY);
    if ((relay_mode & (RELAY0 | RELAY1)) != (RELAY0_GDC | RELAY1_INTERNAL)){
#ifdef CONFIG_DONTTOUCHRELAY
	restore_flags(flags);
	return;
#else
	outb_p((relay_mode & ~(RELAY0 | RELAY1)) |
	       RELAY0_GDC | RELAY1_INTERNAL , IO_RELAY);
#endif
    }
    if (mode & VESA_VSYNC_SUSPEND) {
	stat = inb_p(IO_DPMS);
	outb_p(stat | 0x80, IO_DPMS);
    }
    if (mode & VESA_HSYNC_SUSPEND) {
	stat = inb_p(IO_DPMS);
	outb_p(stat | 0x40, IO_DPMS);
    }

    restore_flags(flags);
}

static void gdc_vesa_unblank(void)
{
    unsigned long flags;
    unsigned char stat;

    save_flags(flags);
    cli();

#ifdef CONFIG_DONTTOUCHRELAY
    if (relay_mode & (RELAY0 | RELAY1))
	return;
#endif
    stat = inb_p(0x09a2);
    outb_p(stat & ~0xc0, IO_DPMS);
    if (relay_mode & (RELAY0 | RELAY1)){
	outb_p(relay_mode, IO_RELAY);
    }

    restore_flags(flags);
}

#ifndef CONFIG_PC9800
static void vga_pal_blank(void)
{
	int i;

	for (i=0; i<16; i++) {
		outb_p (i, dac_reg) ;
		outb_p (0, dac_val) ;
		outb_p (0, dac_val) ;
		outb_p (0, dac_val) ;
	}
}
#endif /* !CONFIG_PC9800 */

static int gdccon_blank(struct vc_data *c, int blank)
{
	switch (blank) {
	case 0:				/* Unblank */
		if (gdc_vesa_blanked) {
			gdc_vesa_unblank();
			gdc_vesa_blanked = 0;
		}
		outb (MODE_FF1_DISP_ENABLE | 1, MODE_FF1);

		/* Tell console.c that it need not to restore the screen */
		return 0;

	case 1:				/* Normal blanking */
#ifndef CONFIG_PC9800
		if (vga_video_type == VIDEO_TYPE_VGAC) {
			vga_pal_blank();
			vga_palette_blanked = 1;
			return 0;
		}
#endif /* !CONFIG_PC9800 */

		/* Disable displaying */
		outb (MODE_FF1_DISP_ENABLE | 0, MODE_FF1);

		/* Tell console.c that it need not to reset origin */
		return 0;

	case -1:			/* Entering graphic mode */
#ifndef CONFIG_PC9800
		scr_memsetw((void *)vga_vram_base, BLANK, c->vc_screenbuf_size);
		vga_is_gfx = 1;
#endif /* !CONFIG_PC9800 */
		return 1;
	default:			/* VESA blanking */
		if (gdc_video_type == VIDEO_TYPE_98NORMAL ||
		    gdc_video_type == VIDEO_TYPE_9840 ||
		    gdc_video_type == VIDEO_TYPE_98HIRESO){
			gdc_vesa_blank(blank-1);
			gdc_vesa_blanked = blank;
		}
		return 0;
	}
}

/*
 * PIO_FONT support.
 *
 * The font loading code goes back to the codepage package by
 * Joel Hoffman (joel@wam.umd.edu). (He reports that the original
 * reference is: "From: p. 307 of _Programmer's Guide to PC & PS/2
 * Video Systems_ by Richard Wilton. 1987.  Microsoft Press".)
 *
 * Change for certain monochrome monitors by Yury Shevchuck
 * (sizif@botik.yaroslavl.su).
 */

#if !defined(CONFIG_PC9800) && defined(CAN_LOAD_EGA_FONTS)

#define colourmap 0xa0000
/* Pauline Middelink <middelin@polyware.iaf.nl> reports that we
   should use 0xA0000 for the bwmap as well.. */
#define blackwmap 0xa0000
#define cmapsz 8192

static int
vgacon_do_font_op(char *arg, int set, int ch512)
{
	int i;
	char *charmap;
	int beg;
	unsigned short video_port_status = vga_video_port_reg + 6;
	int font_select = 0x00;

	if (vga_video_type != VIDEO_TYPE_EGAM) {
		charmap = (char *)VGA_MAP_MEM(colourmap);
		beg = 0x0e;
#ifdef VGA_CAN_DO_64KB
		if (vga_video_type == VIDEO_TYPE_VGAC)
			beg = 0x06;
#endif
	} else {
		charmap = (char *)VGA_MAP_MEM(blackwmap);
		beg = 0x0a;
	}
	
#ifdef BROKEN_GRAPHICS_PROGRAMS
	/*
	 * All fonts are loaded in slot 0 (0:1 for 512 ch)
	 */

	if (!arg)
		return -EINVAL;		/* Return to default font not supported */

	vga_font_is_default = 0;
	font_select = ch512 ? 0x04 : 0x00;
#else	
	/*
	 * The default font is kept in slot 0 and is never touched.
	 * A custom font is loaded in slot 2 (256 ch) or 2:3 (512 ch)
	 */

	if (set) {
		vga_font_is_default = !arg;
		if (!arg)
			ch512 = 0;		/* Default font is always 256 */
		font_select = arg ? (ch512 ? 0x0e : 0x0a) : 0x00;
	}

	if ( !vga_font_is_default )
		charmap += 4*cmapsz;
#endif

	cli();
	outb_p( 0x00, seq_port_reg );   /* First, the sequencer */
	outb_p( 0x01, seq_port_val );   /* Synchronous reset */
	outb_p( 0x02, seq_port_reg );
	outb_p( 0x04, seq_port_val );   /* CPU writes only to map 2 */
	outb_p( 0x04, seq_port_reg );
	outb_p( 0x07, seq_port_val );   /* Sequential addressing */
	outb_p( 0x00, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* Clear synchronous reset */

	outb_p( 0x04, gr_port_reg );    /* Now, the graphics controller */
	outb_p( 0x02, gr_port_val );    /* select map 2 */
	outb_p( 0x05, gr_port_reg );
	outb_p( 0x00, gr_port_val );    /* disable odd-even addressing */
	outb_p( 0x06, gr_port_reg );
	outb_p( 0x00, gr_port_val );    /* map start at A000:0000 */
	sti();
	
	if (arg) {
		if (set)
			for (i=0; i<cmapsz ; i++)
				vga_writeb(arg[i], charmap + i);
		else
			for (i=0; i<cmapsz ; i++)
				arg[i] = vga_readb(charmap + i);

		/*
		 * In 512-character mode, the character map is not contiguous if
		 * we want to remain EGA compatible -- which we do
		 */

		if (ch512) {
			charmap += 2*cmapsz;
			arg += cmapsz;
			if (set)
				for (i=0; i<cmapsz ; i++)
					vga_writeb(arg[i], charmap+i);
			else
				for (i=0; i<cmapsz ; i++)
					arg[i] = vga_readb(charmap+i);
		}
	}
	
	cli();
	outb_p( 0x00, seq_port_reg );   /* First, the sequencer */
	outb_p( 0x01, seq_port_val );   /* Synchronous reset */
	outb_p( 0x02, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* CPU writes to maps 0 and 1 */
	outb_p( 0x04, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* odd-even addressing */
	if (set) {
		outb_p( 0x03, seq_port_reg ); /* Character Map Select */
		outb_p( font_select, seq_port_val );
	}
	outb_p( 0x00, seq_port_reg );
	outb_p( 0x03, seq_port_val );   /* clear synchronous reset */

	outb_p( 0x04, gr_port_reg );    /* Now, the graphics controller */
	outb_p( 0x00, gr_port_val );    /* select map 0 for CPU */
	outb_p( 0x05, gr_port_reg );
	outb_p( 0x10, gr_port_val );    /* enable even-odd addressing */
	outb_p( 0x06, gr_port_reg );
	outb_p( beg, gr_port_val );     /* map starts at b800:0 or b000:0 */

	/* if 512 char mode is already enabled don't re-enable it. */
	if ((set)&&(ch512!=vga_512_chars)) {	/* attribute controller */
		int i;
		for(i=0; i<MAX_NR_CONSOLES; i++) {
			struct vc_data *c = vc_cons[i].d;
			if (c && c->vc_sw == &vga_con)
				c->vc_hi_font_mask = ch512 ? 0x0800 : 0;
		}
		vga_512_chars=ch512;
		/* 256-char: enable intensity bit
		   512-char: disable intensity bit */
		inb_p( video_port_status );	/* clear address flip-flop */
		outb_p ( 0x12, attrib_port ); /* color plane enable register */
		outb_p ( ch512 ? 0x07 : 0x0f, attrib_port );
		/* Wilton (1987) mentions the following; I don't know what
		   it means, but it works, and it appears necessary */
		inb_p( video_port_status );
		outb_p ( 0x20, attrib_port );
	}
	sti();

	return 0;
}

/*
 * Adjust the screen to fit a font of a certain height
 */
static int
vgacon_adjust_height(unsigned fontheight)
{
	int rows, maxscan;
	unsigned char ovr, vde, fsr;

	if (fontheight == vga_video_font_height)
		return 0;

	vga_video_font_height = video_font_height = fontheight;

	rows = video_scan_lines/fontheight;	/* Number of video rows we end up with */
	maxscan = rows*fontheight - 1;		/* Scan lines to actually display-1 */

	/* Reprogram the CRTC for the new font size
	   Note: the attempt to read the overflow register will fail
	   on an EGA, but using 0xff for the previous value appears to
	   be OK for EGA text modes in the range 257-512 scan lines, so I
	   guess we don't need to worry about it.

	   The same applies for the spill bits in the font size and cursor
	   registers; they are write-only on EGA, but it appears that they
	   are all don't care bits on EGA, so I guess it doesn't matter. */

	cli();
	outb_p( 0x07, vga_video_port_reg );		/* CRTC overflow register */
	ovr = inb_p(vga_video_port_val);
	outb_p( 0x09, vga_video_port_reg );		/* Font size register */
	fsr = inb_p(vga_video_port_val);
	sti();

	vde = maxscan & 0xff;			/* Vertical display end reg */
	ovr = (ovr & 0xbd) +			/* Overflow register */
	      ((maxscan & 0x100) >> 7) +
	      ((maxscan & 0x200) >> 3);
	fsr = (fsr & 0xe0) + (fontheight-1);    /*  Font size register */

	cli();
	outb_p( 0x07, vga_video_port_reg );		/* CRTC overflow register */
	outb_p( ovr, vga_video_port_val );
	outb_p( 0x09, vga_video_port_reg );		/* Font size */
	outb_p( fsr, vga_video_port_val );
	outb_p( 0x12, vga_video_port_reg );		/* Vertical display limit */
	outb_p( vde, vga_video_port_val );
	sti();

	vc_resize_all(rows, 0);			/* Adjust console size */
	return 0;
}

static int vgacon_font_op(struct vc_data *c, struct console_font_op *op)
{
	int rc;

	if (vga_video_type < VIDEO_TYPE_EGAM)
		return -EINVAL;

	if (op->op == KD_FONT_OP_SET) {
		if (op->width != 8 || (op->charcount != 256 && op->charcount != 512))
			return -EINVAL;
		rc = vgacon_do_font_op(op->data, 1, op->charcount == 512);
		if (!rc && !(op->flags & KD_FONT_FLAG_DONT_RECALC))
			rc = vgacon_adjust_height(op->height);
	} else if (op->op == KD_FONT_OP_GET) {
		op->width = 8;
		op->height = vga_video_font_height;
		op->charcount = vga_512_chars ? 512 : 256;
		if (!op->data) return 0;
		rc = vgacon_do_font_op(op->data, 0, 0);
	} else
		rc = -ENOSYS;
	return rc;
}

#else /* CONFIG_PC9800 || !CAN_LOAD_EGA_FONTS */

#if 0 /* not used? */
#ifdef CONFIG_PC9800
static int gdccon_adjust_height(unsigned fontheight)
{
   unsigned long lines = video_num_lines * 16;

   write_gdc_cmd (0x0C);  /* STOP */ 
   write_ggdc_cmd (0x0C);  /* STOP */ 

#if 0
   write_gdc_cmd (0x0E);  /* SYNC, DE deny */
   write_gdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
   write_gdc_prm (0x4E);  /* C/R = 78 (80 chars) */
   write_gdc_prm (0x07);  /* VSL = 0(3) ; HS = 7 */
   write_gdc_prm (0x25);  /* HFP = 9    ; VSH = 1(VS=8) */
   write_gdc_prm (0x07);  /* DS, PH = 0 ; HBP = 7 */
   write_gdc_prm (0x07);  /* VH, VL = 0 ; VFP = 7 */
   write_gdc_prm (lines & 0xff);  /* LFL */
   write_gdc_prm (0x64 | ((lines >> 8) & 0x03));  /* VBP = 25   ; LFH */
#else
   write_gdc_cmd (0x0E);  /* SYNC, DE deny */
   write_gdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
   write_gdc_prm (0x4E);  /* C/R = 78 (80 chars) */
   write_gdc_prm (0x4B);  /* VSL = 2(3) ; HS = 11 */
   write_gdc_prm (0x0C);  /* HFP = 3    ; VSH = 0(VS=2) */
   write_gdc_prm (0x03);  /* DS, PH = 0 ; HBP = 3 */
   write_gdc_prm (0x06);  /* VH, VL = 0 ; VFP = 6 */
   write_gdc_prm (lines & 0xff);  /* LFL */
   write_gdc_prm (0x94 | ((lines >> 8) & 0x03));  /* VBP = 37   ; LFH */
#endif
   
   write_gdc_cmd (0x47);  /* PITCH */
   write_gdc_prm (0x50);

   write_gdc_cmd (0x70);  /* SCROLL */
   write_gdc_prm (0x00);
   write_gdc_prm (0x00);
   write_gdc_prm ((lines << 4) & 0xf0);  /* SL1=592 (0x250) */
   write_gdc_prm ((lines >> 4) & 0x3f);

#if 0
   write_ggdc_cmd (0x0E);  /* SYNC */
   write_ggdc_prm (0x06);
   write_ggdc_prm (0x4E);
   write_ggdc_prm (0x07);
   write_ggdc_prm (0x11);
   write_ggdc_prm (0x03);
   write_ggdc_prm (0x07);
   write_ggdc_prm (lines & 0xff);  /* LFL */
   write_ggdc_prm (0x64 | ((lines >> 8) & 0x03));  /* VBP = 25   ; LFH */
#else
   write_ggdc_cmd (0x0E);  /* SYNC, DE deny */
   write_ggdc_prm (0x00);  /* CHR, F, I, D, G, S = 0 */
   write_ggdc_prm (0x4E);  /* C/R = 78 (80 chars) */
   write_ggdc_prm (0x4B);  /* VSL = 2(3) ; HS = 11 */
   write_ggdc_prm (0x0C);  /* HFP = 3    ; VSH = 0(VS=2) */
   write_ggdc_prm (0x03);  /* DS, PH = 0 ; HBP = 3 */
   write_ggdc_prm (0x06);  /* VH, VL = 0 ; VFP = 6 */
   write_ggdc_prm (lines & 0xff);  /* LFL */
   write_ggdc_prm (0x94 | ((lines >> 8) & 0x03));  /* VBP = 37   ; LFH */
#endif

   write_ggdc_cmd (0x47);  /* PITCH */ 
   write_ggdc_prm (0x28);

   write_gdc_cmd (0x0D);  /* START */ 
#if 0
   write_ggdc_cmd (0x0D);  /* START */
#endif

   return 0;
}


#endif /* CONFIG_PC9800 */
#endif /* 0 */

static int gdccon_font_op(struct vc_data *c, struct console_font_op *op)
{
	return -ENOSYS;
}

#endif /* !CONFIG_PC9800 && CAN_LOAD_EGA_FONTS */

/*
#define PC9800_GDCCON_DEBUG 1
*/

static int gdccon_scrolldelta(struct vc_data *c, int lines)
{
#ifdef PC9800_GDCCON_DEBUG
	printk("gdccon_scrolldelta: lines=%d", lines);
#endif

	if (!lines)			/* Turn scrollback off */
		c->vc_visible_origin = c->vc_origin;
	else {
		int vram_size = gdc_vram_end - gdc_vram_base;
		int margin = c->vc_size_row /* * 4 */;
		int ul, we, p, st;

#ifdef PC9800_GDCCON_DEBUG
		printk(", gdc_vram_base=0x%lx, gdc_vram_end=0x%lx", __pa(gdc_vram_base), __pa(gdc_vram_end));
		printk(", vram_size=%d, margin=%d", vram_size, margin);
		printk(", c->vc_origin=0x%lx, c->vc_visible_origin=0x%lx, c->vc_scr_end=0x%lx", __pa(c->vc_origin), __pa(c->vc_visible_origin), __pa(c->vc_scr_end));
		printk(", c->vc_size_row=%u, gdc_rolled_over=%u", c->vc_size_row, gdc_rolled_over);
#endif
		if (gdc_rolled_over > (c->vc_scr_end - gdc_vram_base) + margin) {
			ul = c->vc_scr_end - gdc_vram_base;
			we = gdc_rolled_over + c->vc_size_row;
		} else {
			ul = 0;
			we = vram_size;
		}
		p = (c->vc_visible_origin - gdc_vram_base - ul + we) % we + lines * c->vc_size_row;
		st = (c->vc_origin - gdc_vram_base - ul + we) % we;
#ifdef PC9800_GDCCON_DEBUG
		printk(", ul=%d, we=%d", ul, we);
		printk(", p=%d, st=%d", p, st);
#endif
		if (p < margin)
			p = 0;
		if (p > st - margin)
			p = st;
#ifdef PC9800_GDCCON_DEBUG
		printk(", p(new)=%d", p);
#endif
		c->vc_visible_origin = gdc_vram_base + (p + ul) % we;
	}
	gdc_set_mem_top(c);
#ifdef PC9800_GDCCON_DEBUG
	printk(", c->vc_visible_origin(new)=0x%lx, done.\n", __pa(c->vc_visible_origin));
#endif
	return 1;
}

#undef PC9800_GDCCON_DEBUG

static int gdccon_set_origin(struct vc_data *c)
{
#ifdef PC9800_GDCCON_DEBUG
	printk (KERN_DEBUG "%s: c=%p, console_blanked=%d\n",
		__FUNCTION__, c, console_blanked);
#endif
#if 0 /* It is now Ok to write to blanked screens,
	 since output from video controller is cut off */
	if (console_blanked)	/* We are writing to blanked screens */
		return 0;
#endif

	c->vc_origin = c->vc_visible_origin = gdc_vram_base;
	gdc_set_mem_top(c);
	gdc_rolled_over = 0;
	return 1;
}

static void gdccon_save_screen(struct vc_data *c)
{
	static int gdc_bootup_console = 0;

	if (!gdc_bootup_console) {
		/* This is a gross hack, but here is the only place we can
		 * set bootup console parameters without messing up generic
		 * console initialization routines.
		 */
		gdc_bootup_console = 1;
		c->vc_x = ORIG_X;
		c->vc_y = ORIG_Y;
	}

	scr_memcpyw_from((u16 *) c->vc_screenbuf, (u16 *) c->vc_origin, c->vc_screenbuf_size);
	scr_memcpyw_from((u16 *) ((char *) c->vc_screenbuf + c->vc_screenbuf_size), (u16 *) gdc_attr_offset(c->vc_origin), c->vc_screenbuf_size);
}

static int gdccon_scroll(struct vc_data *c, int t, int b, int dir, int lines)
{
	unsigned long oldo;
	unsigned int delta;

	if (t || b != c->vc_rows /* || vga_is_gfx*/)
		return 0;

	if (c->vc_origin != c->vc_visible_origin)
		gdccon_scrolldelta(c, 0);

	if (!gdc_hardscroll_enabled || lines >= c->vc_rows/2)
		return 0;

	oldo = c->vc_origin;
	delta = lines * c->vc_size_row;
	if (dir == SM_UP) {
		if (c->vc_scr_end + delta >= gdc_vram_end) {
			scr_memcpyw((u16 *)gdc_vram_base,
				    (u16 *)(oldo + delta),
				    c->vc_screenbuf_size - delta);
			scr_memcpyw((u16 *)gdc_attr_offset(gdc_vram_base),
				    (u16 *)gdc_attr_offset(oldo + delta),
				    c->vc_screenbuf_size - delta);
			c->vc_origin = gdc_vram_base;
			gdc_rolled_over = oldo - gdc_vram_base;
		} else
			c->vc_origin += delta;
		scr_memsetw((u16 *)(c->vc_origin + c->vc_screenbuf_size - delta), c->vc_video_erase_char, delta);
		scr_memsetw((u16 *)gdc_attr_offset(c->vc_origin + c->vc_screenbuf_size - delta), c->vc_video_erase_attr, delta);
	} else {
		if (oldo - delta < gdc_vram_base) {
#if 0
			printk (KERN_DEBUG
				"gdc_vram_base = %#lx, gdc_vram_end = %#lx\n",
				gdc_vram_base, gdc_vram_end);
#endif
			scr_memmovew((u16 *)(gdc_vram_end - c->vc_screenbuf_size + delta),
				     (u16 *)oldo,
				     c->vc_screenbuf_size - delta);
			scr_memmovew((u16 *)gdc_attr_offset(gdc_vram_end - c->vc_screenbuf_size + delta),
				     (u16 *)gdc_attr_offset(oldo),
				     c->vc_screenbuf_size - delta);
			c->vc_origin = gdc_vram_end - c->vc_screenbuf_size;
			gdc_rolled_over = 0;
		} else
			c->vc_origin -= delta;
		c->vc_scr_end = c->vc_origin + c->vc_screenbuf_size;
#if 0
		printk (KERN_DEBUG "scr_memsetw(%p, %#x, %u)\n",
			c->vc_origin, c->vc_video_erase_char, delta);
#endif
		scr_memsetw((u16 *)(c->vc_origin), c->vc_video_erase_char, delta);
#if 0
		printk (KERN_DEBUG "scr_memsetw(%p, %#x, %u)\n",
			gdc_attr_offset (c->vc_origin), c->vc_video_erase_attr,
			delta);
#endif
		scr_memsetw((u16 *)gdc_attr_offset(c->vc_origin), c->vc_video_erase_attr, delta);
	}
	c->vc_scr_end = c->vc_origin + c->vc_screenbuf_size;
	c->vc_visible_origin = c->vc_origin;
	gdc_set_mem_top(c);
	c->vc_pos = (c->vc_pos - oldo) + c->vc_origin;
	return 1;
}

#if 0	/* not yet complete... */

static DECLARE_WAIT_QUEUE_HEAD (gdc_wait);

static void gdc_interrupt (int irq, void *dev_id, struct pt_regs *regs)
{
	wake_up_interrutible (&gdc_wait);
}

void gdc_wait_vsync (void)
{
	DECLARE_WAITQUEUE (wait, current);

	add_wait_queue (&gdc_wait, &wait);
	current->state = TASK_INTERRUPTIBLE;
	schedule ();

	current->state = TASK_RUNNING;
	remove_wait_queue (&gdc_wait, &wait);
}
#endif

static int
gdccon_setterm_command (struct vc_data *c)
{
	switch (c->vc_par[0]) {
	case 1: /* set attr for underline mode */
		if (c->vc_npar < 2) {
			if (c->vc_par[1] < 16)
				c->vc_ul_attr = gdccon_color_table
					[color_table[c->vc_par[1]] & 7];
		}
		else {
			if (c->vc_par[2] < 256)
				c->vc_ul_attr = c->vc_par[2];
		}
		if (c->vc_underline)
			goto update_attr;
		return 1;
	case 2:	/* set attr for half intensity mode */
		if (c->vc_npar < 2) {
			if (c->vc_par[1] < 16)
				c->vc_half_attr = gdccon_color_table
					[color_table[c->vc_par[1]] & 7];
		}
		else {
			if (c->vc_par[2] < 256)
				c->vc_half_attr = c->vc_par[2];
		}
		if (c->vc_intensity == 0)
			goto update_attr;
		return 1;

	case 3: /* set color for bold mode */
		if (c->vc_npar < 2) {
			if (c->vc_par[1] < 16)
				c->vc_bold_attr = gdccon_color_table
					[color_table[c->vc_par[1]] & 7];
		}
		else {
			if (c->vc_par[2] < 256)
				c->vc_bold_attr = c->vc_par[2];
		}
		if (c->vc_intensity == 2)
			goto update_attr;
		return 1;
	}
	return 0;

update_attr:
	c->vc_attr = gdccon_build_attr (c,
					c->vc_color, c->vc_intensity,
					c->vc_blink, c->vc_underline,
					c->vc_reverse);
	return 1;
}

/*
 *  The console `switch' structure for the GDC based console
 */

static int gdccon_dummy(struct vc_data *c)
{
	return 0;
}

#define DUMMY (void *) gdccon_dummy

const struct consw gdc_con = {
	con_startup:	gdccon_startup,
	con_init:	gdccon_init,
	con_deinit:	gdccon_deinit,
	con_clear:	DUMMY,
	con_putc:	DUMMY,
	con_putcs:	DUMMY,
	con_cursor:	gdccon_cursor,
	con_scroll:	gdccon_scroll,
	con_bmove:	DUMMY,
	con_switch:	gdccon_switch,
	con_blank:	gdccon_blank,
	con_font_op:	gdccon_font_op,
	con_set_palette: gdccon_set_palette,
	con_scrolldelta: gdccon_scrolldelta,
	con_set_origin:	gdccon_set_origin,
	con_save_screen: gdccon_save_screen,
	con_build_attr:	gdccon_build_attr,
	con_invert_region: gdccon_invert_region,
	con_setterm_command:	gdccon_setterm_command,
};

#ifdef GDCCON_DEBUG_MEMFUNCS

extern void show_stack (unsigned long *esp); /* arch/i386/kernel/traps.c */

void gdccon_check_address (const void *start, size_t len,
			   const char *name, const char *function,
			   const char *file, unsigned int lineno)
{
	int i;
	static int inhibit;

	if (inhibit)
		return;

	if (gdc_vram_base <= (unsigned long) start
	    && (unsigned long) start + len <= gdc_vram_end + 0x2000)
		return;

	for (i = 0; i < MAX_NR_CONSOLES; i++)
		if (vc_cons[i].d
		    && (void *) vc_cons[i].d->vc_screenbuf <= start
		    && start + len <= ((void *) vc_cons[i].d->vc_screenbuf
				       + vc_cons[i].d->vc_screenbuf_size * 2))
			return;

	inhibit = 1;
	if (function)
		printk (KERN_WARNING "gdccon: In function `%s'\n", function);
	if (file)
		printk (KERN_WARNING "gdccon: %s:%u", file, lineno);
	else
		printk (KERN_WARNING "gdccon");
	printk (": %s address out of range (%p+%u)\nStack: ",
		name, start, len);
	show_stack (NULL);
	printk ("\n");
	inhibit = 0;
}

#endif

static int __init gdc_setup(char *str)
{
	unsigned long tmp_ulong;
	char *opt, *orig_opt, *endp;

	while ((opt = strsep (&str, ",")) != NULL) {
		int force = 0;

		orig_opt = opt;
		if (!strncmp (opt, "force", 5)) {
			force = 1;
			opt += 5;
		}
		if (!strcmp (opt, "mono"))
			gdc_can_do_color = 0;
		else if ((tmp_ulong = simple_strtoul (opt, &endp, 0)) > 0) {
			if (!strcmp (endp, "lines")
			    || (!strcmp (endp, "linesforce")
				&& (force = 1))) {
				if (!force
				    && (tmp_ulong < 20
					|| (!PC9800_9821_P ()
					    && 25 < tmp_ulong)
					|| 37 < tmp_ulong))
					printk (KERN_ERR
						"gdccon: %d is out of bound"
						" for number of lines\n",
						(int) tmp_ulong);
				else
					gdc_video_num_lines = tmp_ulong;
			}
			else if (!strcmp (endp, "kHz")) {
				if (tmp_ulong == 24 || tmp_ulong == 25)
					gdc_disp_freq = DISP_FREQ_25k;
				else if (PC9800_9821_P () && tmp_ulong == 31)
					gdc_disp_freq = DISP_FREQ_31k;
				else {
					printk (KERN_ERR "gdccon: `%s' ignored\n",
						orig_opt);
				}
			}
			else
				printk (KERN_ERR "gdccon: unknown option `%s'\n",
					orig_opt);
		}
		else
			printk (KERN_ERR "gdccon: unknown option `%s'\n",
				orig_opt);
	}

	return 1; 
}

__setup("gdccon=", gdc_setup);

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
