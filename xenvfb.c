/**
  This implements an Android compatible virtual framebuffer device driver,
  and it's mostly inspired from vfb.c.

  Author: Claudiu Perta
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/init.h>

#include "fb_draw.h"


/* Android */
#define XENVFB_WIDTH 320

/* Android */
#define XENVFB_HEIGHT 240

/* Android */
#define XENVFB_SCREEN_BASE 0

/* Android */
#define ANDROID_NUMBER_OF_BUFFERS 2

/* Android */
#define ANDROID_BYTES_PER_PIXEL 2


static const u32 cfb_tab8_be[] = {
    0x00000000,0x000000ff,0x0000ff00,0x0000ffff,
    0x00ff0000,0x00ff00ff,0x00ffff00,0x00ffffff,
    0xff000000,0xff0000ff,0xff00ff00,0xff00ffff,
    0xffff0000,0xffff00ff,0xffffff00,0xffffffff
};

static const u32 cfb_tab8_le[] = {
    0x00000000,0xff000000,0x00ff0000,0xffff0000,
    0x0000ff00,0xff00ff00,0x00ffff00,0xffffff00,
    0x000000ff,0xff0000ff,0x00ff00ff,0xffff00ff,
    0x0000ffff,0xff00ffff,0x00ffffff,0xffffffff
};

static const u32 cfb_tab16_be[] = {
    0x00000000, 0x0000ffff, 0xffff0000, 0xffffffff
};

static const u32 cfb_tab16_le[] = {
    0x00000000, 0xffff0000, 0x0000ffff, 0xffffffff
};

static const u32 cfb_tab32[] = {
	0x00000000, 0xffffffff
};

static void *videomemory;
static u_long videomemorysize = XENVFB_WIDTH * XENVFB_HEIGHT *
                                ANDROID_NUMBER_OF_BUFFERS *
                                ANDROID_BYTES_PER_PIXEL;
module_param(videomemorysize, ulong, 0);

static void *rvmalloc(unsigned long size)
{
	void *mem;
	unsigned long adr;

	size = PAGE_ALIGN(size);
	mem = vmalloc_32(size);
	if (!mem)
		return NULL;

	memset(mem, 0, size); /* Clear the ram out, no junk to the user */
	adr = (unsigned long) mem;
	while (size > 0) {
		SetPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}

	return mem;
}

static void rvfree(void *mem, unsigned long size)
{
	unsigned long adr;

	if (!mem)
		return;

	adr = (unsigned long) mem;
	while ((long) size > 0) {
		ClearPageReserved(vmalloc_to_page((void *)adr));
		adr += PAGE_SIZE;
		size -= PAGE_SIZE;
	}
	vfree(mem);
}

/*
It is used to describe the features of a video card you normally can set.
With fb_var_screeninfo, you can define such things as depth and the resolution
you want.
*/
static struct fb_var_screeninfo xenvfb_default __devinitdata = {
	.xres =		XENVFB_WIDTH,
	.yres =		XENVFB_HEIGHT,
	.xres_virtual =	XENVFB_WIDTH,
	.yres_virtual =	XENVFB_HEIGHT * ANDROID_NUMBER_OF_BUFFERS,
	.bits_per_pixel = 16, /* Android requirements  */
  .red = {11, 5, 0},
  .green = {5, 6, 0},
  .blue = {0, 5, 0},
  .transp = {0, 0, 0},
  .activate =	FB_ACTIVATE_NOW,
  .height =	XENVFB_HEIGHT,
  .width =	XENVFB_WIDTH,
  .left_margin =	0,
  .right_margin =	0,
  .upper_margin =	0,
  .lower_margin =	0,
  .vmode =	FB_VMODE_INTERLACED,
};

static struct fb_fix_screeninfo xenvfb_fix __devinitdata = {
	.id =		"XENVFB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.xpanstep =	0,
	.ypanstep =	0,
	.accel =	FB_ACCEL_NONE,
};

static int xenvfb_enable __initdata = 1;	/* enabled by default */
module_param(xenvfb_enable, bool, 0);

static int
xenvfb_check_var(struct fb_var_screeninfo *var,
			           struct fb_info *info);
static int
xenvfb_set_par(struct fb_info *info);

static int
xenvfb_setcolreg(u_int regno,
              u_int red,
              u_int green,
              u_int blue,
			        u_int transp,
              struct fb_info *info);

static int
xenvfb_pan_display(struct fb_var_screeninfo *var,
			             struct fb_info *info);

static int
xenvfb_mmap(struct fb_info *info,
		        struct vm_area_struct *vma);


static void
xenvfb_copyarea(struct fb_info *p,
                const struct fb_copyarea *area);

static ssize_t
xenvfb_read(struct fb_info *info,
            char __user *buf,
            size_t count,
            loff_t *ppos);

static ssize_t
xenvfb_write(struct fb_info *info,
             const char __user *buf,
		         size_t count, loff_t *ppos);

static void
xenvfb_imageblit(struct fb_info *p,
                 const struct fb_image *image);

static void
xenvfb_fillrect(struct fb_info *p,
                const struct fb_fillrect *rect);


static struct fb_ops xenvfb_ops = {
	.fb_read        = xenvfb_read,
	.fb_write       = xenvfb_write,
	.fb_check_var	= xenvfb_check_var,
	.fb_set_par	= xenvfb_set_par,
	.fb_setcolreg	= xenvfb_setcolreg,
	.fb_pan_display	= xenvfb_pan_display,
	.fb_fillrect	= xenvfb_fillrect,
	.fb_copyarea	= xenvfb_copyarea,
	.fb_imageblit	= xenvfb_imageblit,
	.fb_mmap	= xenvfb_mmap,
};


static void
color_imageblit(const struct fb_image *image,
                struct fb_info *p,
			          void *dst1,
                u32 start_index,
                u32 pitch_index)
{
	u32 *dst, *dst2;
	u32 color = 0, val, shift;
	int i, n, bpp = p->var.bits_per_pixel;
	u32 null_bits = 32 - bpp;
	u32 *palette = (u32 *) p->pseudo_palette;
	const u8 *src = image->data;

	dst2 = dst1;
	for (i = image->height; i--; ) {
		n = image->width;
		dst = dst1;
		shift = 0;
		val = 0;

		if (start_index) {
			u32 start_mask = ~(FB_SHIFT_HIGH(p, ~(u32)0,
							 start_index));
			val = *dst & start_mask;
			shift = start_index;
		}
		while (n--) {
			if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
			    p->fix.visual == FB_VISUAL_DIRECTCOLOR )
				color = palette[*src];
			else
				color = *src;
			color <<= FB_LEFT_POS(p, bpp);
			val |= FB_SHIFT_HIGH(p, color, shift);
			if (shift >= null_bits) {
				*dst++ = val;

				val = (shift == null_bits) ? 0 :
					FB_SHIFT_LOW(p, color, 32 - shift);
			}
			shift += bpp;
			shift &= (32 - 1);
			src++;
		}
		if (shift) {
			u32 end_mask = FB_SHIFT_HIGH(p, ~(u32)0, shift);

			*dst &= end_mask;
			*dst |= val;
		}
		dst1 += p->fix.line_length;
		if (pitch_index) {
			dst2 += p->fix.line_length;
			dst1 = (u8 *)((long)dst2 & ~(sizeof(u32) - 1));

			start_index += pitch_index;
			start_index &= 32 - 1;
		}
	}
}

static void
slow_imageblit(const struct fb_image *image,
               struct fb_info *p,
				       void *dst1,
               u32 fgcolor,
               u32 bgcolor,
				       u32 start_index,
               u32 pitch_index)
{
	u32 shift, color = 0, bpp = p->var.bits_per_pixel;
	u32 *dst, *dst2;
	u32 val, pitch = p->fix.line_length;
	u32 null_bits = 32 - bpp;
	u32 spitch = (image->width+7)/8;
	const u8 *src = image->data, *s;
	u32 i, j, l;

	dst2 = dst1;
	fgcolor <<= FB_LEFT_POS(p, bpp);
	bgcolor <<= FB_LEFT_POS(p, bpp);

	for (i = image->height; i--; ) {
		shift = val = 0;
		l = 8;
		j = image->width;
		dst = dst1;
		s = src;

		if (start_index) {
			u32 start_mask = ~(FB_SHIFT_HIGH(p, ~(u32)0,
							 start_index));
			val = *dst & start_mask;
			shift = start_index;
		}

		while (j--) {
			l--;
			color = (*s & (1 << l)) ? fgcolor : bgcolor;
			val |= FB_SHIFT_HIGH(p, color, shift);

			if (shift >= null_bits) {
				*dst++ = val;
				val = (shift == null_bits) ? 0 :
					FB_SHIFT_LOW(p, color, 32 - shift);
			}
			shift += bpp;
			shift &= (32 - 1);
			if (!l) { l = 8; s++; };
		}

 		if (shift) {
			u32 end_mask = FB_SHIFT_HIGH(p, ~(u32)0, shift);

			*dst &= end_mask;
			*dst |= val;
		}

		dst1 += pitch;
		src += spitch;
		if (pitch_index) {
			dst2 += pitch;
			dst1 = (u8 *)((long)dst2 & ~(sizeof(u32) - 1));
			start_index += pitch_index;
			start_index &= 32 - 1;
		}

	}
}

static void
fast_imageblit(const struct fb_image *image,
               struct fb_info *p,
				       void *dst1,
               u32 fgcolor,
               u32 bgcolor)
{
	u32 fgx = fgcolor, bgx = bgcolor, bpp = p->var.bits_per_pixel;
	u32 ppw = 32/bpp, spitch = (image->width + 7)/8;
	u32 bit_mask, end_mask, eorx, shift;
	const char *s = image->data, *src;
	u32 *dst;
	const u32 *tab = NULL;
	int i, j, k;

	switch (bpp) {
	case 8:
		tab = fb_be_math(p) ? cfb_tab8_be : cfb_tab8_le;
		break;
	case 16:
		tab = fb_be_math(p) ? cfb_tab16_be : cfb_tab16_le;
		break;
	case 32:
	default:
		tab = cfb_tab32;
		break;
	}

	for (i = ppw-1; i--; ) {
		fgx <<= bpp;
		bgx <<= bpp;
		fgx |= fgcolor;
		bgx |= bgcolor;
	}

	bit_mask = (1 << ppw) - 1;
	eorx = fgx ^ bgx;
	k = image->width/ppw;

	for (i = image->height; i--; ) {
		dst = dst1;
		shift = 8;
		src = s;

		for (j = k; j--; ) {
			shift -= ppw;
			end_mask = tab[(*src >> shift) & bit_mask];
			*dst++ = (end_mask & eorx) ^ bgx;
			if (!shift) {
				shift = 8;
				src++;
			}
		}
		dst1 += p->fix.line_length;
		s += spitch;
	}
}


static void
bitfill_aligned(struct fb_info *p,
                unsigned long *dst,
                int dst_idx,
		            unsigned long pat,
                unsigned n,
                int bits)
{
	unsigned long first, last;

	if (!n)
		return;

	first = FB_SHIFT_HIGH(p, ~0UL, dst_idx);
	last = ~(FB_SHIFT_HIGH(p, ~0UL, (dst_idx+n) % bits));

	if (dst_idx+n <= bits) {
		if (last)
			first &= last;
		*dst = comp(pat, *dst, first);
	} else {

 		if (first!= ~0UL) {
			*dst = comp(pat, *dst, first);
			dst++;
			n -= bits - dst_idx;
		}

		n /= bits;
		while (n >= 8) {
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			*dst++ = pat;
			n -= 8;
		}
		while (n--)
			*dst++ = pat;
		if (last)
			*dst = comp(pat, *dst, last);
	}
}


static void
bitfill_unaligned(struct fb_info *p,
                  unsigned long *dst,
                  int dst_idx,
		              unsigned long pat,
                  int left,
                  int right,
                  unsigned n,
                  int bits)
{
	unsigned long first, last;

	if (!n)
		return;

	first = FB_SHIFT_HIGH(p, ~0UL, dst_idx);
	last = ~(FB_SHIFT_HIGH(p, ~0UL, (dst_idx+n) % bits));

	if (dst_idx+n <= bits) {
		if (last)
			first &= last;
		*dst = comp(pat, *dst, first);
	} else {
		if (first) {
			*dst = comp(pat, *dst, first);
			dst++;
			pat = pat << left | pat >> right;
			n -= bits - dst_idx;
		}

		n /= bits;
		while (n >= 4) {
			*dst++ = pat;
			pat = pat << left | pat >> right;
			*dst++ = pat;
			pat = pat << left | pat >> right;
			*dst++ = pat;
			pat = pat << left | pat >> right;
			*dst++ = pat;
			pat = pat << left | pat >> right;
			n -= 4;
		}
		while (n--) {
			*dst++ = pat;
			pat = pat << left | pat >> right;
		}

		if (last)
			*dst = comp(pat, *dst, last);
	}
}

static void
bitfill_aligned_rev(struct fb_info *p,
                    unsigned long *dst,
                    int dst_idx,
		                unsigned long pat,
                    unsigned n,
                    int bits)
{
	unsigned long val = pat;
	unsigned long first, last;

	if (!n)
		return;

	first = FB_SHIFT_HIGH(p, ~0UL, dst_idx);
	last = ~(FB_SHIFT_HIGH(p, ~0UL, (dst_idx+n) % bits));

	if (dst_idx+n <= bits) {
		if (last)
			first &= last;
		*dst = comp(*dst ^ val, *dst, first);
	} else {
		if (first!=0UL) {
			*dst = comp(*dst ^ val, *dst, first);
			dst++;
			n -= bits - dst_idx;
		}

		n /= bits;
		while (n >= 8) {
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			*dst++ ^= val;
			n -= 8;
		}
		while (n--)
			*dst++ ^= val;
		if (last)
			*dst = comp(*dst ^ val, *dst, last);
	}
}


static void
bitfill_unaligned_rev(struct fb_info *p,
                      unsigned long *dst,
                      int dst_idx,
		                  unsigned long pat,
                      int left,
                      int right,
                      unsigned n,
		                  int bits)
{
	unsigned long first, last;

	if (!n)
		return;

	first = FB_SHIFT_HIGH(p, ~0UL, dst_idx);
	last = ~(FB_SHIFT_HIGH(p, ~0UL, (dst_idx+n) % bits));

	if (dst_idx+n <= bits) {
		if (last)
			first &= last;
		*dst = comp(*dst ^ pat, *dst, first);
	} else {

		if (first != 0UL) {
			*dst = comp(*dst ^ pat, *dst, first);
			dst++;
			pat = pat << left | pat >> right;
			n -= bits - dst_idx;
		}

		n /= bits;
		while (n >= 4) {
			*dst++ ^= pat;
			pat = pat << left | pat >> right;
			*dst++ ^= pat;
			pat = pat << left | pat >> right;
			*dst++ ^= pat;
			pat = pat << left | pat >> right;
			*dst++ ^= pat;
			pat = pat << left | pat >> right;
			n -= 4;
		}
		while (n--) {
			*dst ^= pat;
			pat = pat << left | pat >> right;
		}

		if (last)
			*dst = comp(*dst ^ pat, *dst, last);
	}
}


static ssize_t
xenvfb_read(struct fb_info *info,
         char __user *buf,
         size_t count,
         loff_t *ppos)
{
	unsigned long p = *ppos;
	void *src;
	int err = 0;
	unsigned long total_size;

  printk(KERN_INFO "[xenvfb] xenvfb_read()....");
	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->screen_size;

	if (total_size == 0)
		total_size = info->fix.smem_len;

	if (p >= total_size)
		return 0;

	if (count >= total_size)
		count = total_size;

	if (count + p > total_size)
		count = total_size - p;

	src = (void __force *)(info->screen_base + p);

	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	if (copy_to_user(buf, src, count))
		err = -EFAULT;

	if  (!err)
		*ppos += count;

	return (err) ? err : count;
}


static ssize_t
xenvfb_write(struct fb_info *info,
          const char __user *buf,
          size_t count,
          loff_t *ppos)
{
	unsigned long p = *ppos;
	void *dst;
	int err = 0;
	unsigned long total_size;

	if (info->state != FBINFO_STATE_RUNNING)
		return -EPERM;

	total_size = info->screen_size;

	if (total_size == 0)
		total_size = info->fix.smem_len;

	if (p > total_size)
		return -EFBIG;

	if (count > total_size) {
		err = -EFBIG;
		count = total_size;
	}

	if (count + p > total_size) {
		if (!err)
			err = -ENOSPC;

		count = total_size - p;
	}

	dst = (void __force *) (info->screen_base + p);

	if (info->fbops->fb_sync)
		info->fbops->fb_sync(info);

	if (copy_from_user(dst, buf, count))
		err = -EFAULT;

	if  (!err)
		*ppos += count;

	return (err) ? err : count;
}


static void
xenvfb_imageblit(struct fb_info *p,
              const struct fb_image *image)
{
	u32 fgcolor, bgcolor, start_index, bitstart, pitch_index = 0;
	u32 bpl = sizeof(u32), bpp = p->var.bits_per_pixel;
	u32 width = image->width;
	u32 dx = image->dx, dy = image->dy;
	void *dst1;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	bitstart = (dy * p->fix.line_length * 8) + (dx * bpp);
	start_index = bitstart & (32 - 1);
	pitch_index = (p->fix.line_length & (bpl - 1)) * 8;

	bitstart /= 8;
	bitstart &= ~(bpl - 1);
	dst1 = (void __force *)p->screen_base + bitstart;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	if (image->depth == 1) {
		if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
		    p->fix.visual == FB_VISUAL_DIRECTCOLOR) {
			fgcolor = ((u32*)(p->pseudo_palette))[image->fg_color];
			bgcolor = ((u32*)(p->pseudo_palette))[image->bg_color];
		} else {
			fgcolor = image->fg_color;
			bgcolor = image->bg_color;
		}

		if (32 % bpp == 0 && !start_index && !pitch_index &&
		    ((width & (32/bpp-1)) == 0) &&
		    bpp >= 8 && bpp <= 32)
			fast_imageblit(image, p, dst1, fgcolor, bgcolor);
		else
			slow_imageblit(image, p, dst1, fgcolor, bgcolor,
					start_index, pitch_index);
	} else
		color_imageblit(image, p, dst1, start_index, pitch_index);
}


static void
xenvfb_fillrect(struct fb_info *p,
             const struct fb_fillrect *rect)
{
	unsigned long pat, pat2, fg;
	unsigned long width = rect->width, height = rect->height;
	int bits = BITS_PER_LONG, bytes = bits >> 3;
	u32 bpp = p->var.bits_per_pixel;
	unsigned long *dst;
	int dst_idx, left;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	if (p->fix.visual == FB_VISUAL_TRUECOLOR ||
	    p->fix.visual == FB_VISUAL_DIRECTCOLOR )
		fg = ((u32 *) (p->pseudo_palette))[rect->color];
	else
		fg = rect->color;

	pat = pixel_to_pat( bpp, fg);

	dst = (unsigned long *)((unsigned long)p->screen_base & ~(bytes-1));
	dst_idx = ((unsigned long)p->screen_base & (bytes - 1))*8;
	dst_idx += rect->dy*p->fix.line_length*8+rect->dx*bpp;
	left = bits % bpp;
	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);
	if (!left) {
		void (*fill_op32)(struct fb_info *p, unsigned long *dst,
				  int dst_idx, unsigned long pat, unsigned n,
				  int bits) = NULL;

		switch (rect->rop) {
		case ROP_XOR:
			fill_op32 = bitfill_aligned_rev;
			break;
		case ROP_COPY:
			fill_op32 = bitfill_aligned;
			break;
		default:
			printk( KERN_ERR "cfb_fillrect(): unknown rop, "
				"defaulting to ROP_COPY\n");
			fill_op32 = bitfill_aligned;
			break;
		}
		while (height--) {
			dst += dst_idx >> (ffs(bits) - 1);
			dst_idx &= (bits - 1);
			fill_op32(p, dst, dst_idx, pat, width*bpp, bits);
			dst_idx += p->fix.line_length*8;
		}
	} else {
		int right, r;
		void (*fill_op)(struct fb_info *p, unsigned long *dst,
				int dst_idx, unsigned long pat, int left,
				int right, unsigned n, int bits) = NULL;
#ifdef __LITTLE_ENDIAN
		right = left;
		left = bpp - right;
#else
		right = bpp - left;
#endif
		switch (rect->rop) {
		case ROP_XOR:
			fill_op = bitfill_unaligned_rev;
			break;
		case ROP_COPY:
			fill_op = bitfill_unaligned;
			break;
		default:
			printk(KERN_ERR "sys_fillrect(): unknown rop, "
				"defaulting to ROP_COPY\n");
			fill_op = bitfill_unaligned;
			break;
		}
		while (height--) {
			dst += dst_idx / bits;
			dst_idx &= (bits - 1);
			r = dst_idx % bpp;

			pat2 = le_long_to_cpu(rolx(cpu_to_le_long(pat), r, bpp));
			fill_op(p, dst, dst_idx, pat2, left, right,
				width*bpp, bits);
			dst_idx += p->fix.line_length*8;
		}
	}
}

static void
bitcpy(struct fb_info *p,
       unsigned long *dst,
       int dst_idx,
		   const unsigned long *src,
       int src_idx,
       int bits,
       unsigned n)
{
	unsigned long first, last;
	int const shift = dst_idx-src_idx;
	int left, right;

	first = FB_SHIFT_HIGH(p, ~0UL, dst_idx);
	last = ~(FB_SHIFT_HIGH(p, ~0UL, (dst_idx+n) % bits));

	if (!shift) {
		if (dst_idx+n <= bits) {
			if (last)
				first &= last;
			*dst = comp(*src, *dst, first);
		} else {
 			if (first != ~0UL) {
				*dst = comp(*src, *dst, first);
				dst++;
				src++;
				n -= bits - dst_idx;
			}

			n /= bits;
			while (n >= 8) {
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				*dst++ = *src++;
				n -= 8;
			}
			while (n--)
				*dst++ = *src++;

			if (last)
				*dst = comp(*src, *dst, last);
		}
	} else {
		unsigned long d0, d1;
		int m;

		right = shift & (bits - 1);
		left = -shift & (bits - 1);

		if (dst_idx+n <= bits) {
			if (last)
				first &= last;
			if (shift > 0) {
				*dst = comp(*src >> right, *dst, first);
			} else if (src_idx+n <= bits) {
				*dst = comp(*src << left, *dst, first);
			} else {
				d0 = *src++;
				d1 = *src;
				*dst = comp(d0 << left | d1 >> right, *dst,
					    first);
			}
		} else {
			d0 = *src++;
			if (shift > 0) {
				*dst = comp(d0 >> right, *dst, first);
				dst++;
				n -= bits - dst_idx;
			} else {
				d1 = *src++;
				*dst = comp(d0 << left | *dst >> right, *dst, first);
				d0 = d1;
				dst++;
				n -= bits - dst_idx;
			}

			m = n % bits;
			n /= bits;
			while (n >= 4) {
				d1 = *src++;
				*dst++ = d0 << left | d1 >> right;
				d0 = d1;
				d1 = *src++;
				*dst++ = d0 << left | d1 >> right;
				d0 = d1;
				d1 = *src++;
				*dst++ = d0 << left | d1 >> right;
				d0 = d1;
				d1 = *src++;
				*dst++ = d0 << left | d1 >> right;
				d0 = d1;
				n -= 4;
			}
			while (n--) {
				d1 = *src++;
				*dst++ = d0 << left | d1 >> right;
				d0 = d1;
			}

			if (last) {
				if (m <= right) {
					*dst = comp(d0 << left, *dst, last);
				} else {
 					d1 = *src;
					*dst = comp(d0 << left | d1 >> right,
						    *dst, last);
				}
			}
		}
	}
}

static void
bitcpy_rev(struct fb_info *p,
           unsigned long *dst,
           int dst_idx,
		       const unsigned long *src,
           int src_idx,
           int bits,
           unsigned n)
{
	unsigned long first, last;
	int shift;

	dst += (n-1)/bits;
	src += (n-1)/bits;
	if ((n-1) % bits) {
		dst_idx += (n-1) % bits;
		dst += dst_idx >> (ffs(bits) - 1);
		dst_idx &= bits - 1;
		src_idx += (n-1) % bits;
		src += src_idx >> (ffs(bits) - 1);
		src_idx &= bits - 1;
	}

	shift = dst_idx-src_idx;

	first = FB_SHIFT_LOW(p, ~0UL, bits - 1 - dst_idx);
	last = ~(FB_SHIFT_LOW(p, ~0UL, bits - 1 - ((dst_idx-n) % bits)));

	if (!shift) {
		if ((unsigned long)dst_idx+1 >= n) {
			if (last)
				first &= last;
			*dst = comp(*src, *dst, first);
		} else {

			if (first != ~0UL) {
				*dst = comp(*src, *dst, first);
				dst--;
				src--;
				n -= dst_idx+1;
			}

			n /= bits;
			while (n >= 8) {
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				*dst-- = *src--;
				n -= 8;
			}
			while (n--)
				*dst-- = *src--;
			if (last)
				*dst = comp(*src, *dst, last);
		}
	} else {

		int const left = -shift & (bits-1);
		int const right = shift & (bits-1);

		if ((unsigned long)dst_idx+1 >= n) {
			if (last)
				first &= last;
			if (shift < 0) {
				*dst = comp(*src << left, *dst, first);
			} else if (1+(unsigned long)src_idx >= n) {
				*dst = comp(*src >> right, *dst, first);
			} else {
				*dst = comp(*src >> right | *(src-1) << left,
					    *dst, first);
			}
		} else {
			unsigned long d0, d1;
			int m;

			d0 = *src--;
			if (shift < 0) {
				*dst = comp(d0 << left, *dst, first);
			} else {
				d1 = *src--;
				*dst = comp(d0 >> right | d1 << left, *dst,
					    first);
				d0 = d1;
			}
			dst--;
			n -= dst_idx+1;

			m = n % bits;
			n /= bits;
			while (n >= 4) {
				d1 = *src--;
				*dst-- = d0 >> right | d1 << left;
				d0 = d1;
				d1 = *src--;
				*dst-- = d0 >> right | d1 << left;
				d0 = d1;
				d1 = *src--;
				*dst-- = d0 >> right | d1 << left;
				d0 = d1;
				d1 = *src--;
				*dst-- = d0 >> right | d1 << left;
				d0 = d1;
				n -= 4;
			}
			while (n--) {
				d1 = *src--;
				*dst-- = d0 >> right | d1 << left;
				d0 = d1;
			}

			if (last) {
				if (m <= left) {
					*dst = comp(d0 >> right, *dst, last);
				} else {
					d1 = *src;
					*dst = comp(d0 >> right | d1 << left,
						    *dst, last);
				}
			}
		}
	}
}


static void
xenvfb_copyarea(struct fb_info *p,
             const struct fb_copyarea *area)
{
	u32 dx = area->dx, dy = area->dy, sx = area->sx, sy = area->sy;
	u32 height = area->height, width = area->width;
	unsigned long const bits_per_line = p->fix.line_length*8u;
	unsigned long *dst = NULL, *src = NULL;
	int bits = BITS_PER_LONG, bytes = bits >> 3;
	int dst_idx = 0, src_idx = 0, rev_copy = 0;

	if (p->state != FBINFO_STATE_RUNNING)
		return;

	if ((dy == sy && dx > sx) || (dy > sy)) {
		dy += height;
		sy += height;
		rev_copy = 1;
	}

	dst = src = (unsigned long *)((unsigned long)p->screen_base &
				      ~(bytes-1));
	dst_idx = src_idx = 8*((unsigned long)p->screen_base & (bytes-1));
	dst_idx += dy*bits_per_line + dx*p->var.bits_per_pixel;
	src_idx += sy*bits_per_line + sx*p->var.bits_per_pixel;

	if (p->fbops->fb_sync)
		p->fbops->fb_sync(p);

	if (rev_copy) {
		while (height--) {
			dst_idx -= bits_per_line;
			src_idx -= bits_per_line;
			dst += dst_idx >> (ffs(bits) - 1);
			dst_idx &= (bytes - 1);
			src += src_idx >> (ffs(bits) - 1);
			src_idx &= (bytes - 1);
			bitcpy_rev(p, dst, dst_idx, src, src_idx, bits,
				width*p->var.bits_per_pixel);
		}
	} else {
		while (height--) {
			dst += dst_idx >> (ffs(bits) - 1);
			dst_idx &= (bytes - 1);
			src += src_idx >> (ffs(bits) - 1);
			src_idx &= (bytes - 1);
			bitcpy(p, dst, dst_idx, src, src_idx, bits,
				width*p->var.bits_per_pixel);
			dst_idx += bits_per_line;
			src_idx += bits_per_line;
		}
	}
}


static u_long
get_line_length(int xres_virtual,
                int bpp)
{
	u_long length;

	length = xres_virtual * bpp;
	length = (length + 31) & ~31;
	length >>= 3;
	return (length);
}

    /*
     *  Setting the video mode has been split into two parts.
     *  First part, xxxfb_check_var, must not write anything
     *  to hardware, it should only verify and adjust var.
     *  This means it doesn't alter par but it does use hardware
     *  data from it to check this var.
     */

static int
xenvfb_check_var(struct fb_var_screeninfo *var,
			        struct fb_info *info)
{
	u_long line_length;

	/*
	 *  FB_VMODE_CONUPDATE and FB_VMODE_SMOOTH_XPAN are equal!
	 *  as FB_VMODE_SMOOTH_XPAN is only used internally
	 */

	if (var->vmode & FB_VMODE_CONUPDATE) {
		var->vmode |= FB_VMODE_YWRAP;
		var->xoffset = info->var.xoffset;
		var->yoffset = info->var.yoffset;
	}

	/*
	 *  Some very basic checks
	 */
	if (!var->xres)
		var->xres = 1;
	if (!var->yres)
		var->yres = 1;
	if (var->xres > var->xres_virtual)
		var->xres_virtual = var->xres;
	if (var->yres > var->yres_virtual)
		var->yres_virtual = var->yres;
	if (var->bits_per_pixel <= 1)
		var->bits_per_pixel = 1;
	else if (var->bits_per_pixel <= 8)
		var->bits_per_pixel = 8;
	else if (var->bits_per_pixel <= 16)
		var->bits_per_pixel = 16;
	else if (var->bits_per_pixel <= 24)
		var->bits_per_pixel = 24;
	else if (var->bits_per_pixel <= 32)
		var->bits_per_pixel = 32;
	else
		return -EINVAL;

	if (var->xres_virtual < var->xoffset + var->xres)
		var->xres_virtual = var->xoffset + var->xres;
	if (var->yres_virtual < var->yoffset + var->yres)
		var->yres_virtual = var->yoffset + var->yres;

	/*
	 *  Memory limit
	 */
	line_length =
	    get_line_length(var->xres_virtual, var->bits_per_pixel);
	if (line_length * var->yres_virtual > videomemorysize)
		return -ENOMEM;

	/*
	 * Now that we checked it we alter var. The reason being is that the video
	 * mode passed in might not work but slight changes to it might make it
	 * work. This way we let the user know what is acceptable.
	 */
	switch (var->bits_per_pixel) {
	case 1:
	case 8:
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 0;
		var->green.length = 8;
		var->blue.offset = 0;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 16:		/* RGBA 5551 */
		if (var->transp.length) {
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 5;
			var->blue.offset = 10;
			var->blue.length = 5;
			var->transp.offset = 15;
			var->transp.length = 1;
		} else {	/* RGB 565 */
			var->red.offset = 0;
			var->red.length = 5;
			var->green.offset = 5;
			var->green.length = 6;
			var->blue.offset = 11;
			var->blue.length = 5;
			var->transp.offset = 0;
			var->transp.length = 0;
		}
		break;
	case 24:		/* RGB 888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->transp.length = 0;
		break;
	case 32:		/* RGBA 8888 */
		var->red.offset = 0;
		var->red.length = 8;
		var->green.offset = 8;
		var->green.length = 8;
		var->blue.offset = 16;
		var->blue.length = 8;
		var->transp.offset = 24;
		var->transp.length = 8;
		break;
	}
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;

	return 0;
}

/* This routine actually sets the video mode. It's in here where we
 * the hardware state info->par and fix which can be affected by the
 * change in par. For this driver it doesn't do much.
 */
static int
xenvfb_set_par(struct fb_info *info)
{
	info->fix.line_length = get_line_length(info->var.xres_virtual,
						info->var.bits_per_pixel);
	return 0;
}

    /*
     *  Set a single color register. The values supplied are already
     *  rounded down to the hardware's capabilities (according to the
     *  entries in the var structure). Return != 0 for invalid regno.
     */

static int
xenvfb_setcolreg(u_int regno,
              u_int red,
              u_int green,
              u_int blue,
			        u_int transp,
              struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;
	/*
	 * Program hardware... do anything you want with transp
	 */

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

	/* Directcolor:
	 *   var->{color}.offset contains start of bitfield
	 *   var->{color}.length contains length of bitfield
	 *   {hardwarespecific} contains width of RAMDAC
	 *   cmap[X] is programmed to (X << red.offset) | (X << green.offset) | (X << blue.offset)
	 *   RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Pseudocolor:
	 *    var->{color}.offset is 0 unless the palette index takes less than
	 *                        bits_per_pixel bits and is stored in the upper
	 *                        bits of the pixel value
	 *    var->{color}.length is set so that 1 << length is the number of available
	 *                        palette entries
	 *    cmap is not used
	 *    RAMDAC[X] is programmed to (red, green, blue)
	 *
	 * Truecolor:
	 *    does not use DAC. Usually 3 are present.
	 *    var->{color}.offset contains start of bitfield
	 *    var->{color}.length contains length of bitfield
	 *    cmap is programmed to (red << red.offset) | (green << green.offset) |
	 *                      (blue << blue.offset) | (transp << transp.offset)
	 *    RAMDAC does not exist
	 */
#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}
	return 0;
}

    /*
     *  Pan or Wrap the Display
     *
     *  This call looks only at xoffset, yoffset and the FB_VMODE_YWRAP flag
     */

static int
xenvfb_pan_display(struct fb_var_screeninfo *var,
			          struct fb_info *info)
{
	if (var->vmode & FB_VMODE_YWRAP) {
		if (var->yoffset < 0
		    || var->yoffset >= info->var.yres_virtual
		    || var->xoffset)
			return -EINVAL;
	} else {
		if (var->xoffset + var->xres > info->var.xres_virtual ||
		    var->yoffset + var->yres > info->var.yres_virtual)
			return -EINVAL;
	}
	info->var.xoffset = var->xoffset;
	info->var.yoffset = var->yoffset;
	if (var->vmode & FB_VMODE_YWRAP)
		info->var.vmode |= FB_VMODE_YWRAP;
	else
		info->var.vmode &= ~FB_VMODE_YWRAP;
	return 0;
}


static int
xenvfb_mmap(struct fb_info *info,
		     struct vm_area_struct *vma)
{
	unsigned long start = vma->vm_start;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long page, pos;

	if (offset + size > info->fix.smem_len) {
		return -EINVAL;
	}

	pos = (unsigned long)info->fix.smem_start + offset;

	while (size > 0) {
		page = vmalloc_to_pfn((void *)pos);
		if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED)) {
			return -EAGAIN;
		}
		start += PAGE_SIZE;
		pos += PAGE_SIZE;
		if (size > PAGE_SIZE)
			size -= PAGE_SIZE;
		else
			size = 0;
	}

	vma->vm_flags |= VM_RESERVED;	/* avoid to swap out this VMA */
	return 0;

}

#ifndef MODULE
/*
 * The virtual framebuffer driver is only enabled if explicitly
 * requested by passing 'video=xenvfb:' (or any actual options).
 */
static int
__init xenvfb_setup(char *options)
{
	char *this_opt;

	xenvfb_enable = 0;

	if (!options)
		return 1;

	xenvfb_enable = 1;

	if (!*options)
		return 1;

	while ((this_opt = strsep(&options, ",")) != NULL) {
		if (!*this_opt)
			continue;
		/* Test disable for backwards compatibility */
		if (!strcmp(this_opt, "disable"))
			xenvfb_enable = 0;
	}
	return 1;
}
#endif  /*  MODULE  */

static int
__devinit xenvfb_probe(struct platform_device *dev)
{
	struct fb_info *fb;
	int retval = -ENOMEM;
  size_t width, height;
	/*
	 * For real video cards we use ioremap.
	 */
	if (!(videomemory = rvmalloc(videomemorysize)))
		return retval;

	/*
	 * VFB must clear memory to prevent kernel info
	 * leakage into userspace
	 * VGA-based drivers MUST NOT clear memory if
	 * they want to be able to take over vgacon
	 */

	memset(videomemory, 0, videomemorysize);

	fb = framebuffer_alloc(sizeof(u32) * 256, &dev->dev);
	if (!fb)
		goto err;

	platform_set_drvdata(dev, fb);

  /*
	retval = fb_find_mode(&fb->var, fb, NULL,
			      NULL, 0, NULL, 8);

	if (!retval || (retval == 4))
		fb->var = xenvfb_default;

  */


  width = XENVFB_WIDTH;
  height = XENVFB_HEIGHT;

	fb->screen_base = (char __iomem *)videomemory;
	fb->fbops = &xenvfb_ops;

  fb->flags = FBINFO_FLAG_DEFAULT;
  fb->fix.type = FB_TYPE_PACKED_PIXELS;
  fb->fix.visual = FB_VISUAL_TRUECOLOR;
  fb->fix.line_length = width * ANDROID_BYTES_PER_PIXEL;
  fb->fix.accel  = FB_ACCEL_NONE;
  fb->fix.ypanstep = 1;
  fb->var.xres   = width;
  fb->var.yres   = height;
  fb->var.xres_virtual = width;
  fb->var.yres_virtual = height * ANDROID_NUMBER_OF_BUFFERS;
  fb->var.bits_per_pixel = 16;
  fb->var.activate = FB_ACTIVATE_NOW;
  fb->var.height = height;
  fb->var.width  = width;

  fb->var.red.offset = 11;
  fb->var.red.length = 5;
  fb->var.green.offset = 5;
  fb->var.green.length = 6;
  fb->var.blue.offset = 0;
  fb->var.blue.length = 5;

  fb->fix.smem_start = (unsigned long) videomemory;
  fb->fix.smem_len = videomemorysize;

	fb->pseudo_palette = fb->par;
  fb->par = NULL;

	retval = fb_alloc_cmap(&fb->cmap, 256, 0);
	if (retval < 0)
		goto err1;


  retval = fb_set_var(fb, &fb->var);
  if (retval) {
	  printk(KERN_INFO "[xenvfb] ...error in fb_set_var()!");
		goto err2;
  }

	retval = register_framebuffer(fb);
	if (retval < 0) {
	  printk(KERN_INFO "[xenvfb] ...error in register_framebuffer()!");
		goto err2;
  }

	//platform_set_drvdata(dev, fb);

	printk(KERN_INFO
	       "fb%d: Virtual frame buffer device, using %ldK of video memory\n",
	       fb->node, videomemorysize >> 10);
	return 0;
err2:
	fb_dealloc_cmap(&fb->cmap);
err1:
	framebuffer_release(fb);
err:
	rvfree(videomemory, videomemorysize);
	return retval;
}

static int
xenvfb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (info) {
		unregister_framebuffer(info);
		rvfree(videomemory, videomemorysize);
		fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}
	return 0;
}

static struct platform_driver xenvfb_driver = {
	.probe	= xenvfb_probe,
	.remove = xenvfb_remove,
	.driver = {
	.name	= "xenvfb",
	},
};

static struct platform_device *xenvfb_device;

static int
__init xenvfb_init(void)
{
	int ret = 0;

#ifndef MODULE
	char *option = NULL;

	if (fb_get_options("xenvfb", &option))
		return -ENODEV;
	xenvfb_setup(option);
#endif

	if (!xenvfb_enable)
		return -ENXIO;

	ret = platform_driver_register(&xenvfb_driver);

	if (!ret) {
		xenvfb_device = platform_device_alloc("xenvfb", 0);

		if (xenvfb_device)
			ret = platform_device_add(xenvfb_device);
		else
			ret = -ENOMEM;

		if (ret) {
			platform_device_put(xenvfb_device);
			platform_driver_unregister(&xenvfb_driver);
		}
	}

	return ret;
}


static void
__exit xenvfb_exit(void)
{
	platform_device_unregister(xenvfb_device);
	platform_driver_unregister(&xenvfb_driver);
}

module_init(xenvfb_init);
module_exit(xenvfb_exit);

MODULE_LICENSE("GPL");
