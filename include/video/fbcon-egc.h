/*
 *	FBcon low-level driver for EGC
 */

#ifndef _VIDEO_FBCON_EGC_H
#define _VIDEO_FBCON_EGC_H

#include <linux/config.h>

#ifdef MODULE
#if defined(CONFIG_FBCON_EGC) || defined(CONFIG_FBCON_EGC_MODULE)
#define FBCON_HAS_EGC
#endif
#else
#if defined(CONFIG_FBCON_EGC)
#define FBCON_HAS_EGC
#endif
#endif

extern struct display_switch fbcon_egc;
extern void fbcon_egc_setup(struct display *p);
extern void fbcon_egc_bmove(struct display *p, int sy, int sx, int dy, int dx,
				   int height, int width);
extern void fbcon_egc_clear(struct vc_data *conp, struct display *p, int sy,
				   int sx, int height, int width);
extern void fbcon_egc_putc(struct vc_data *conp, struct display *p, int c,
				  int yy, int xx);
extern void fbcon_egc_putcs(struct vc_data *conp, struct display *p,
				   const unsigned short *s, int count, int yy, int xx);
extern void fbcon_egc_revc(struct display *p, int xx, int yy);

#endif /* _VIDEO_FBCON_EGC_H */
