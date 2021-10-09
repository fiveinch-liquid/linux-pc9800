/*
 * linux/drivers/video/egcfb.c -- EGC/GRCG framebuffer
 *
 * Copyright 1999 Satoshi YAMADA <slakichi@kmc.kyoto-u.ac.jp>
 *
 * Based on VGA framebuffer (C) 1999 Ben Pfaff <pfaffben@debian.org> ,
 *				     Petr Vandrovec <VANDROVE@vc.cvut.cz>
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file COPYING in the main directory of this
 * archive for more details. 
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/malloc.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/console.h>
#include <linux/selection.h>
#include <linux/ioport.h>
#include <linux/init.h>

#include <asm/io.h>

#include <video/fbcon.h>
#include <video/fbcon-egc.h>

/* plane 0,1,2 */
#define EGC_RGB_FB_PHYS 0xA8000
#define EGC_RGB_FB_PHYS_LEN 0x18000
/* plane 3 */
#define EGC_E_FB_PHYS 0xE0000
#define EGC_E_FB_PHYS_LEN 0x8000
/* total (fix-info returns these value, but these are INVALID value!) */
#define EGC_FB_PHYS 0xA8000
#define EGC_FB_PHYS_LEN 0x20000

#define EGCIO_GDC_CMD		0xa2
#define EGCIO_GDC_PARAM		0xa0
#define EGCIO_PL_NUM		0xa8
#define EGCIO_PL_GREEN		0xaa
#define EGCIO_PL_RED		0xac
#define EGCIO_PL_BLUE		0xae
#define EGCIO_SYNCCTRL		0x9a2

#define EGCMEM_ISEGC		0x54d

/* --------------------------------------------------------------------- */

/*
 * card parameters
 */

static struct egcfb_info {
	struct fb_info	fb_info;	/* framebuffer info */
	char *video_vbase[2];		/* VRAM base address in virtual */
	int hasEGC;			/* This matchine has EGC?(1/0) */
	int palette_blanked;		/* Blanked by Palette control */
	int sync_blanked;		/* Blanked by APM control */
} egcfb;


/* --------------------------------------------------------------------- */

/* default GDC parameters */
#define EGC_FB_PIXCLOCK	 39708	/* 1/((margins+hsync_len+xres[pix])*(hsync[Hz])) */
#define EGC_FB_MARGIN_LE 32	/* SYNC HBP 512>= ([P5(b5-b0)]+1)*8 >= 24 */
#define EGC_FB_MARGIN_R	 32	/* SYNC HFP 512>= ([P4(b7-b2)]+1)*8 >= 16 */
#define EGC_FB_MARGIN_U	 37	/* SYNC VBP 63 >= P8(b7-b2) >= 1 */
#define EGC_FB_MARGIN_LO 2	/* SYNC VFP 63 >= P6(b5-b0) >= 1 */
#define EGC_FB_HSYNC	 96	/* SYNC HS  512>= ([P3(b5-b0)]+1)*8 ,>=32 */
#define EGC_FB_VSYNC	 2	/* SYNC VS  31 >= [P4(b1-b0)P3(b7-b5)] , >=1 */

/*

4+80+4+12+2=98*8=784pixels,70Hz

37+400+2+2=441lines,31.48Hz

*/

static struct fb_var_screeninfo egcfb_defined = {
	640,400,640,400,/* W,H, W, H (virtual) load xres,xres_virtual*/
	0,0,		/* virtual -> visible no offset */
	4,		/* depth -> load bits_per_pixel */
	0,		/* greyscale ? */
	{0,0,0},	/* R */
	{0,0,0},	/* G */
	{0,0,0},	/* B */
	{0,0,0},	/* transparency */
	0,		/* standard pixel format */
	FB_ACTIVATE_NOW,
	-1,-1,
	0,
	EGC_FB_PIXCLOCK,
	EGC_FB_MARGIN_LE, EGC_FB_MARGIN_R,
	EGC_FB_MARGIN_U, EGC_FB_MARGIN_LO,
	EGC_FB_HSYNC, EGC_FB_VSYNC,
	0,	/* No sync info */
	FB_VMODE_NONINTERLACED,
	{0,0,0,0,0,0}
};


static struct display default_display;
static struct {
	u_short blue, green, red, transp;
} palette[16];

static int current_console = 0;

/* --------------------------------------------------------------------- */

static int egcfb_apply_varinfo(int con, struct fb_info *info)
{
	/* virtual console operation - but EGC/GRCG can't do so,
	   so nothing to do. */
	return 0;
}

static int egcfb_get_fixinfo(struct fb_fix_screeninfo *fix, int con_num,
			     struct fb_info *info)
{
#if 0
	/* vga16fb op. I think these are unused operation ... */
	struct display *current_display;

	if (con_num < 0)
		current_display = &default_display;
	else
		current_display = fb_display + con_num;
#endif
	memset(fix, 0, sizeof(struct fb_fix_screeninfo));
	strcpy(fix->id,"egc");

	/* again, these value is INVALID; do not use these value! */
	fix->smem_start = EGC_FB_PHYS;
	fix->smem_len = EGC_FB_PHYS_LEN;
	
	fix->type = FB_TYPE_VGA_PLANES;
	fix->visual = FB_VISUAL_PSEUDOCOLOR;
	fix->xpanstep  = 8;
	fix->ypanstep  = 1;
	fix->ywrapstep = 0;
	fix->line_length = 80;
	return 0;
}

static int egcfb_get_varinfo(struct fb_var_screeninfo *var, int con_num,
			     struct fb_info *info)
{
	if(con_num < 0)
		memcpy(var, &egcfb_defined, sizeof(struct fb_var_screeninfo));
	else
		*var=fb_display[con_num].var;
	return 0;
}

static void egcfb_set_display(int con_num, struct egcfb_info *info)
{
	struct fb_fix_screeninfo fix;
	struct display *display;

	if (con_num < 0)
		display = &default_display;
	else
		display = fb_display + con_num;

	egcfb_get_fixinfo(&fix, con_num, &info->fb_info);

	display->screen_base = info->video_vbase[0];
	display->visual = fix.visual;
	display->type = fix.type;
	display->type_aux = fix.type_aux;
	display->ypanstep = fix.ypanstep;
	display->ywrapstep = fix.ywrapstep;
	display->line_length = fix.line_length;
	display->next_line = fix.line_length;
	display->can_soft_blank = 1;
	display->inverse = 0;

	display->dispsw = &fbcon_egc;

	display->scrollmode = SCROLL_YREDRAW;
}

#if 0
static void vga16fb_encode_var(struct fb_var_screeninfo *var,
			       const struct vga16fb_par *par,
			       const struct vga16fb_info *info)
{
	*var = par->var;
}
#endif

static int egcfb_check_var(const struct fb_var_screeninfo *var,
			   const struct egcfb_info *info)
{
	if(var->xres != 640 || var->xres_virtual != 640)
		return -EINVAL;
	if(var->yres != 400 || var->yres_virtual != 400)
		return -EINVAL;
	if(var->xoffset != 0 || var->yoffset != 0)
		return -EINVAL;
	if(var->bits_per_pixel != 4)
		return -EINVAL;
	return 0;
}

static void egcfb_set_defaultvar(struct fb_var_screeninfo *var)
{
	var->xres=var->xres_virtual=640;
	var->yres=var->yres_virtual=400;
	var->xoffset=var->yoffset=0;
	var->bits_per_pixel=4;
	var->grayscale=0;
	var->red.length=var->green.length=var->blue.length=4;
	var->red.offset=var->green.offset=var->blue.offset=0;
	var->transp.length=var->transp.offset=0;
	var->nonstd=0;
	var->height=var->width=-1;
	var->accel_flags=0;
	var->left_margin=EGC_FB_MARGIN_LE;
	var->right_margin=EGC_FB_MARGIN_R;
	var->upper_margin=EGC_FB_MARGIN_U;
	var->lower_margin=EGC_FB_MARGIN_LO;
	var->hsync_len=EGC_FB_HSYNC;
	var->vsync_len=EGC_FB_VSYNC;
	var->sync=0;
	var->vmode=FB_VMODE_NONINTERLACED;

	cli();
	outb_p(0x47,0xa2);
	outb_p(80,0xa0);	/* pitch command */
	sti();
	outb_p(0x01,0x6a);
	outb_p(0x00,0xa4);	/* show bank:0 */
	outb_p(0x00,0xa6);	/* write bank:0 */
	outb_p(0x0d,0xa2); /* Show Graphics */
	outb_p(0x0c,0x62); /* Hide Text */
}

static int egcfb_set_varinfo(struct fb_var_screeninfo *var, int con_num,
			       struct fb_info *fb)
{
	struct egcfb_info *info = (struct egcfb_info*)fb;
#if 0
	struct display *display;
#endif
	int retval;

#if 0
	if (con < 0)
		display = fb->disp;
	else
		display = fb_display + con;
#endif
	retval = egcfb_check_var(var, info);
	if (retval != 0)
		return retval;
	egcfb_set_defaultvar(var);
	
	if ((var->activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_TEST)
		return 0;

#if 0
	if ((var->activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW) {
	/* Nothing to do. */
	}
#endif

	return 0;
}

static int egcfb_get_palette(unsigned regno, unsigned *red, unsigned *green,
			   unsigned *blue, unsigned *transp,
			   struct fb_info *fb_info)
{
	/*
	 *  Read a single color register and split it into colors/transparent.
	 *  Return != 0 for invalid regno.
	 */

	if (regno >= 16)
		return 1;

	*red   = palette[regno].red;
	*green = palette[regno].green;
	*blue  = palette[regno].blue;
	*transp = 0;
	return 0;
}

static void egcfb_do_set_palette(int regno, unsigned red, unsigned green,
				 unsigned blue)
{
	outb_p(regno,EGCIO_PL_NUM);
	outb_p(green >>12,EGCIO_PL_GREEN);
	outb_p(red   >>12,EGCIO_PL_RED);
	outb_p(blue  >>12,EGCIO_PL_BLUE);
}



static int egcfb_set_palette(unsigned regno, unsigned red, unsigned green,
			  unsigned blue, unsigned transp,
			  struct fb_info *fb_info)
{
	int gray;

	if (regno >= 16)
		return 1;

	palette[regno].red   = red;
	palette[regno].green = green;
	palette[regno].blue  = blue;
	
	if (current_console < 0)
		gray = default_display.var.grayscale;
	else
		gray = fb_display[current_console].var.grayscale;
	if (gray) {
		red = green = blue = (red * 77 + green * 151 + blue * 28) >> 8;
	}
	egcfb_do_set_palette(regno,red,green,blue);
	
	return 0;
}

static void egcfb_set_palette_all(int con_num, struct fb_info *info)
{
	if (con_num != current_console)
		return;
	if (fb_display[con_num].cmap.len)
		fb_set_cmap(&fb_display[con_num].cmap, 1, egcfb_set_palette,
			    info);
	else
		fb_set_cmap(fb_default_cmap(16), 1, egcfb_set_palette, info);
}


static int egcfb_get_colormap(struct fb_cmap *cmap, int kspc, int con_num,
			    struct fb_info *info)
{
	if (con_num != current_console)
		return fb_get_cmap(cmap, kspc, egcfb_get_palette, info);
	else if (fb_display[con_num].cmap.len)
		fb_copy_cmap(&fb_display[con_num].cmap, cmap, kspc ? 0 : 2);
	else
		fb_copy_cmap(fb_default_cmap(16), cmap, kspc ? 0 : 2);
	return 0;
}

static int egcfb_set_colormap(struct fb_cmap *cmap, int kspc, int con_num,
			   struct fb_info *info)
{
	int err;

	if (!fb_display[con_num].cmap.len) {	/* no colormap allocated? */
		err = fb_alloc_cmap(&fb_display[con_num].cmap,16,0);
		if (err)
			return err;
	}
	if (con_num == current_console)
		{
		int retval= fb_set_cmap(cmap, kspc, egcfb_set_palette, info);
		//if( retval == 0 )
		//	fb_copy_cmap(cmap, &fb_display[current_console].cmap, kspc ? 0 : 1);
		return retval;
		}
	else
		fb_copy_cmap(cmap, &fb_display[con_num].cmap, kspc ? 0 : 1);
	return 0;
}

static int egcfb_pan_display(struct fb_var_screeninfo *var, int con_num,
			       struct fb_info *info) 
{
	if (var->xoffset + fb_display[con_num].var.xres > fb_display[con_num].var.xres_virtual ||
	    var->yoffset + fb_display[con_num].var.yres > fb_display[con_num].var.yres_virtual)
		return -EINVAL;
	/* must be xoffset=yoffset=0, so nothing to do. */
	fb_display[con_num].var.xoffset = var->xoffset;
	fb_display[con_num].var.yoffset = var->yoffset;
	fb_display[con_num].var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
}

#if 0
static int egcfb_ioctl(struct inode *inode, struct file *file,
		       unsigned int cmd, unsigned long arg, int con,
		       struct fb_info *info)
{
	/* :-P */
	return -EINVAL;
}
#else
#define egcfb_ioctl	NULL
#endif

static int egcfb_mmap(struct fb_info *info, struct file *file,
		      struct vm_area_struct * vma)
{
	/* based on fbmem.c - Copyright (C) 1994 Martin Schaller */
	unsigned long start,len,off;
	unsigned long vm_end, vm_start;
	pgprot_t vm_prot;

	off	 = vma->vm_pgoff << PAGE_SHIFT;
	vm_start = vma->vm_start;
	vm_end	 = vma->vm_end;

	start = EGC_FB_PHYS;
	len   = (start & ~PAGE_MASK)+EGC_FB_PHYS_LEN;
	start &= PAGE_MASK;
	len = (len+~PAGE_MASK) & PAGE_MASK;

	if(vm_end - vm_start + off > len)
		return -EINVAL;

	if (boot_cpu_data.x86 > 3)
		pgprot_val(vma->vm_page_prot) |= _PAGE_PCD;
	vm_prot = vma->vm_page_prot;

	/* This is fake ;-) */
	vma->vm_pgoff = ( off + start ) >> PAGE_SHIFT;

	/* Plane 0,1,2 */
	
	start = EGC_RGB_FB_PHYS;
	len   = (start & ~PAGE_MASK)+EGC_RGB_FB_PHYS_LEN;
	start &= PAGE_MASK;
	len = (len+~PAGE_MASK) & PAGE_MASK;

	if(off < len) {
		if ((vm_end - vm_start + off) > len)
			vm_end = (len - off) + vm_start;
		if (io_remap_page_range(vm_start, start + off,
				     vm_end - vm_start, vma->vm_page_prot))
			return -EAGAIN;
		/* restore */
		vm_end = vma->vm_end;
		}
	
	/* Plane 3 (Extended) */
	
	start = EGC_E_FB_PHYS;
	len   = (start & ~PAGE_MASK)+EGC_E_FB_PHYS_LEN;
	start &= PAGE_MASK;
	len = (len+~PAGE_MASK) & PAGE_MASK;
	
	if(vm_end - vm_start + off > EGC_RGB_FB_PHYS_LEN) {
		if(off < EGC_RGB_FB_PHYS_LEN) {
			vm_start += (EGC_RGB_FB_PHYS_LEN - off);
			off = 0;
			}
		else
			off -= EGC_RGB_FB_PHYS_LEN;
		if ((vm_end - vm_start + off) > len)
			/* mustn't be occured ... */
			return -EINVAL;
		if (io_remap_page_range(vm_start, start + off,
				     vm_end - vm_start, vma->vm_page_prot))
			/* FIXME: don't re-remapped planes even if failed. */
			return -EAGAIN;
		}

	return 0;

}

static struct fb_ops egcfb_ops = {
	owner:		THIS_MODULE,
	fb_get_fix:	egcfb_get_fixinfo,
	fb_get_var:	egcfb_get_varinfo,
	fb_set_var:	egcfb_set_varinfo,
	fb_get_cmap:	egcfb_get_colormap,
	fb_set_cmap:	egcfb_set_colormap,
	fb_pan_display:	egcfb_pan_display,
	fb_ioctl:	egcfb_ioctl,
	fb_mmap:	egcfb_mmap
};

int egcfb_setup(char *options)
{
	char *this_opt;
	
	egcfb.fb_info.fontname[0] = '\0';
	
	if (!options || !*options)
		return 0;
	
	for (this_opt = strtok (options, ",");
	     this_opt;
	     this_opt = strtok (NULL, ",")) {
		if (!strncmp(this_opt, "font:", 5))
			strcpy(egcfb.fb_info.fontname, this_opt+5);
	}
	return 0;
}

/* on switching console ... */
static int egcfb_switch(int con_num, struct fb_info *fb)
{
	struct egcfb_info * info = (struct egcfb_info *)fb;

	/* Do we have to save the colormap? */
	if (fb_display[current_console].cmap.len) {
		fb_get_cmap(&fb_display[current_console].cmap, 1,
			    egcfb_get_palette, fb);
	}
	
	current_console = con_num;
	egcfb_set_defaultvar(&fb_display[con_num].var);
	egcfb_set_display(con_num, info);
	egcfb_set_palette_all(con_num, fb);
	return 1;
}

static void egcfb_sync_blank(struct egcfb_info *info, int mode)
{
	int sendcmd = 0;
	if ((mode & VESA_VSYNC_SUSPEND) ||
	   egcfb.sync_blanked & VESA_VSYNC_SUSPEND)
		{
		egcfb.sync_blanked |= VESA_VSYNC_SUSPEND;
		sendcmd |= 0x80;
		}
	if ((mode & VESA_HSYNC_SUSPEND) ||
	   egcfb.sync_blanked & VESA_HSYNC_SUSPEND)
		{
		egcfb.sync_blanked |= VESA_HSYNC_SUSPEND;
		sendcmd |= 0x40;
		}
	outb_p(sendcmd, EGCIO_SYNCCTRL);
}

static void egcfb_sync_unblank(struct egcfb_info *info)
{
	outb_p(0, EGCIO_SYNCCTRL);
	info->sync_blanked = 0;
}

static void egcfb_pallete_blank(void)
{
	int i;

	for (i=0; i<16; i++) {
		outb_p (i, EGCIO_PL_NUM) ;
		outb_p (0, EGCIO_PL_GREEN) ;
		outb_p (0, EGCIO_PL_RED) ;
		outb_p (0, EGCIO_PL_BLUE) ;
	}
}

/* 0 unblank, 1 blank, 2 no vsync, 3 no hsync, 4 off */
static void egcfb_blank(int blank, struct fb_info *fb_info)
{
	struct egcfb_info *info = (struct egcfb_info*)fb_info;

	switch (blank) {
	case 0:				/* Unblank */
		if (info->sync_blanked) {
			egcfb_sync_unblank(info);
		}
		if (info->palette_blanked) {
			egcfb_set_palette_all(current_console, fb_info);
			info->palette_blanked = 0;
		}
		break;
	case 1:				/* blank */
		egcfb_pallete_blank();
		info->palette_blanked = 1;
		break;
	default:			/* VESA blanking */
		egcfb_sync_blank(info, blank-1);
		break;
	}
}

int __init egc_init(void)
{
	int i,j;
	printk(KERN_DEBUG "egcfb: initializing\n");

	if (!__request_region(&ioport_resource, 0xa2, 1, "egcfb")) {
		printk(KERN_ERR "egcfb: unable to reserve io port, exiting\n");
		return -1;
		}

	if(!(isa_readb(0x054c)&(1<<1)))
		{
		printk(KERN_ERR "egcfb: this machine does not have GRCG, exiting\n");
		return -EINVAL;
		}
	if(!(isa_readb(0x054c)&(1<<2)))
		{
		printk(KERN_ERR "egcfb: not 16-colors mode, exiting\n");
		return -EINVAL;
		}
	if(!(isa_readb(0x054d)&(1<<6)))
		{
		printk(KERN_ERR "egcfb: this machine does not have EGC, exiting\n");
		return -EINVAL;
		}
	

	egcfb.video_vbase[0] = ioremap(EGC_RGB_FB_PHYS, EGC_RGB_FB_PHYS_LEN);
	egcfb.video_vbase[1] = ioremap(EGC_E_FB_PHYS, EGC_E_FB_PHYS_LEN);
	printk(KERN_INFO "egcfb: mapped to 0x%p and 0x%p\n",
	       egcfb.video_vbase[0], egcfb.video_vbase[1]);

	egcfb.hasEGC = (isa_readb(EGCMEM_ISEGC)&0x40) ? 1 : 0;
	egcfb.palette_blanked = 0;
	egcfb.sync_blanked = 0;

	egcfb_defined.red.length   = 4;
	egcfb_defined.green.length = 4;
	egcfb_defined.blue.length  = 4;
	for(i = 0; i < 16; i++) {
		j = color_table[i];
		palette[i].red	 = default_red[j];
		palette[i].green = default_grn[j];
		palette[i].blue	 = default_blu[j];
	}

	default_display.var = egcfb_defined;

	strcpy(egcfb.fb_info.modename, "EGC GRCG");
	egcfb.fb_info.changevar = NULL;
	egcfb.fb_info.node = -1;
	egcfb.fb_info.fbops = &egcfb_ops;
	egcfb.fb_info.disp=&default_display;
	egcfb.fb_info.switch_con=&egcfb_switch;
	egcfb.fb_info.updatevar=&egcfb_apply_varinfo;
	egcfb.fb_info.blank=&egcfb_blank;
	egcfb.fb_info.flags=FBINFO_FLAG_DEFAULT;
	egcfb_set_display(-1, &egcfb);

	isa_writeb(isa_readb(0x054C)|0x80,0x054C);

	if (register_framebuffer(&egcfb.fb_info)<0)
		return -EINVAL;

	printk(KERN_INFO "fb%d: %s frame buffer device\n",
	       GET_FB_IDX(egcfb.fb_info.node), egcfb.fb_info.modename);

	return 0;
}

#ifndef MODULE
int __init egcfb_init(void)
{
    return egc_init();
}

#else /* MODULE */

int init_module(void)
{
	return egc_init();
}

void cleanup_module(void)
{
	unregister_framebuffer(&egcfb.fb_info);
	iounmap(egcfb.video_vbase[0]);
	iounmap(egcfb.video_vbase[1]);
	__release_region(&ioport_resource,0xa2,1);
}

#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */

