/* Linux driver for Philips webcam 
   Various miscellaneous functions and tables.
   (C) 1999-2001 Nemosoft Unv. (webcam@smcc.demon.nl)

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/  

#include <linux/slab.h>

#include "pwc.h"

struct pwc_coord pwc_image_sizes[PSZ_MAX] = 
{
	{ 128,  96, 0 },
	{ 160, 120, 0 },
	{ 176, 144, 0 },
	{ 320, 240, 0 },
	{ 352, 288, 0 },
	{ 640, 480, 0 },
};

/* x,y -> PSZ_ */
int pwc_decode_size(struct pwc_device *pdev, int width, int height)
{
	int i, find;

	/* Make sure we don't go beyond our max size */
	if (width > pdev->view_max.x || height > pdev->view_max.y)
		return -1;
	/* Find the largest size supported by the camera that fits into the
	   requested size. 
	 */
	find = -1;
	for (i = 0; i < PSZ_MAX; i++) {
		if (pdev->image_mask & (1 << i)) {
			if (pwc_image_sizes[i].x <= width && pwc_image_sizes[i].y <= height)
				find = i;
		}
	}
	return find;
}

/* initialize variables depending on type */
void pwc_construct(struct pwc_device *pdev)
{
	switch(pdev->type) {
	case 645:
	case 646:
		pdev->view_min.x = 128;
		pdev->view_min.y =  96;
		pdev->view_max.x = 352;
		pdev->view_max.y = 288;
		pdev->image_mask = 1 << PSZ_SQCIF | 1 << PSZ_QCIF | 1 << PSZ_CIF;
		pdev->vcinterface = 2;
		pdev->vendpoint = 4;
		pdev->frame_header_size = 0;
		pdev->frame_trailer_size = 0;
		break;
	case 675:
	case 680:
	case 690:
		pdev->view_min.x = 128;
		pdev->view_min.y =  96;
		pdev->view_max.x = 640;
		pdev->view_max.y = 480;
		pdev->image_mask = 1 << PSZ_SQCIF | 1 << PSZ_QSIF | 1 << PSZ_QCIF | 1 << PSZ_SIF | 1 << PSZ_CIF | 1 << PSZ_VGA;
		pdev->vcinterface = 3;
		pdev->vendpoint = 4;
		pdev->frame_header_size = 0;
		pdev->frame_trailer_size = 0;
		break;
	case 730:
	case 740:
		pdev->view_min.x = 160;
		pdev->view_min.y = 120;
		pdev->view_max.x = 640;
		pdev->view_max.y = 480;
		pdev->image_mask = 1 << PSZ_QSIF | 1 << PSZ_SIF | 1 << PSZ_VGA;
		pdev->vcinterface = 3;
		pdev->vendpoint = 5;
		pdev->frame_header_size = TOUCAM_HEADER_SIZE;
		pdev->frame_trailer_size = TOUCAM_TRAILER_SIZE;
		break;
	}
	pdev->view_min.size = pdev->view_min.x * pdev->view_min.y;
	pdev->view_max.size = pdev->view_max.x * pdev->view_max.y;
}


