/*
 *  gdc.h - macro & inline functions for accessing GDC text-VRAM
 *
 *  Copyright (C) 1997-2000   KITAGAWA Takurou,
 *			      UGAWA Tomoharu,
 *			      TAKAI Kosuke
 *			      (Linux/98 Project)
 */
#ifndef _LINUX_ASM_GDC_H_
#define _LINUX_ASM_GDC_H_

#include <linux/config.h>

#define GDC_MAP_MEM(x) (unsigned long)phys_to_virt(x)

#define gdc_readb(x) (*(x))
#define gdc_writeb(x,y) (*(y) = (x))

#define VT_BUF_HAVE_RW
#define scr_writew(val, addr)	(*(volatile __u16 *)(addr) = (val))
#define scr_readw(addr)		(*(volatile __u16 *)(addr))

#define VT_BUF_HAVE_MEMSETW
extern inline void
scr_memsetw (u16 *s, u16 c, unsigned int count)
{
#ifdef CONFIG_GDC_32BITACCESS
	__asm__ __volatile__ ("shr%L1 %1
	jz 2f
" /*	cld	kernel code now assumes DF = 0 any time */ "\
	test%L0 %3,%0
	jz 1f
	stos%W2
	dec%L1 %1
1:	shr%L1 %1
	rep
	stos%L2
	jnc 2f
	stos%W2
	rep
	stos%W2
2:"
			      : "=D"(s), "=c"(count)
			      : "a"((((u32) c) << 16) | c), "g"(2),
			        "0"(s), "1"(count));
#else
	__asm__ __volatile__ ("rep\n\tstosw"
			      : "=D"(s), "=c"(count)
			      : "0"(s), "1"(count / 2), "a"(c));
#endif	
}

#define VT_BUF_HAVE_MEMCPYW
extern inline void
scr_memcpyw (u16 *d, u16 *s, unsigned int count)
{
#if 1 /* def CONFIG_GDC_32BITACCESS */
	__asm__ __volatile__ ("shr%L2 %2
	jz 2f
" /*	cld	*/ "\
	test%L0 %3,%0
	jz 1f
	movs%W0
	dec%L2 %2
1:	shr%L2 %2
	rep
	movs%L0
	jnc 2f
	movs%W0
2:"
			      : "=D"(d), "=S"(s), "=c"(count)
			      : "g"(2), "0"(d), "1"(s), "2"(count));
#else
	__asm__ __volatile__ ("rep\n\tmovsw"
			      : "=D"(d), "=S"(s), "=c"(count)
			      : "0"(d), "1"(s), "2"(count / 2));
#endif
}

extern inline void
scr_memrcpyw (u16 *d, u16 *s, unsigned int count)
{
#if 1 /* def CONFIG_GDC_32BITACCESS */
	u16 tmp;

	__asm__ __volatile__ ("shr%L3 %3
	jz 2f
	std
	lea%L1 -4(%1,%3,2),%1
	lea%L2 -4(%2,%3,2),%2
	test%L1 %4,%1
	jz 1f
	mov%W0 2(%2),%0
	sub%L2 %4,%2
	dec%L3 %3
	mov%W0 %0,2(%1)
	sub%L1 %4,%1
1:	shr%L3 %3
	rep
	movs%L0
	jnc 3f
	mov%W0 2(%2),%0
	mov%W0 %0,2(%1)
3:	cld
2:"
			      : "=r"(tmp), "=D"(d), "=S"(s), "=c"(count)
			      : "g"(2), "1"(d), "2"(s), "3"(count));
#else
	__asm__ __volatile__ ("std\n\trep\n\tmovsw\n\tcld"
			      : "=D"(d), "=S"(s), "=c"(count)
			      : "0"((void *) d + count - 2),
			        "1"((void *) s + count - 2), "2"(count / 2));
#endif	
}

#define VT_BUF_HAVE_MEMMOVEW
extern inline void
scr_memmovew (u16 *d, u16 *s, unsigned int count)
{
	if (d > s)
		scr_memrcpyw (d, s, count);
	else
		scr_memcpyw (d, s, count);
}	

#define VT_BUF_HAVE_MEMCPYF
extern inline void
scr_memcpyw_from (u16 *d, u16 *s, unsigned int count)
{
#ifdef CONFIG_GDC_32BITACCESS
	/* VRAM is quite slow, so we align source pointer (%esi)
	   to double-word alignment. */
	__asm__ __volatile__ ("shr%L2 %2
	jz 2f
" /*	cld	*/ "\
	test%L0 %3,%0
	jz 1f
	movs%W0
	dec%L2 %2
1:	shr%L2 %2
	rep
	movs%L0
	jnc 2f
	movs%W0
2:"
			      : "=D"(d), "=S"(s), "=c"(count)
			      : "g"(2), "0"(d), "1"(s), "2"(count));
#else
	__asm__ __volatile__ ("rep\n\tmovsw"
			      : "=D"(d), "=S"(s), "=c"(count)
			      : "0"(d), "1"(s), "2"(count / 2));
#endif
}

#ifdef CONFIG_GDC_32BITACCESS
# define scr_memcpyw_to	scr_memcpyw
#else
# define scr_memcpyw_to scr_memcpyw_from
#endif

#define GDCCON_DEBUG_MEMFUNCS
#ifdef GDCCON_DEBUG_MEMFUNCS

extern void gdccon_check_address (const void *, size_t, const char *,
				  const char *, const char *, unsigned int);

#undef scr_writew
#define scr_writew(val, addr)	({					\
	__u16 *_p_ = (void *)(addr);					\
	gdccon_check_address (_p_, 2, "destination",			\
			      "scr_writew", __FILE__, __LINE__);	\
	*(volatile __u16 *)_p_ = (val);					\
})

#undef scr_readw
#define scr_readw(addr)	({					\
	__u16 *_p_ = (void *)(addr);				\
	gdccon_check_address (_p_, 2, "source",			\
			      "scr_readw", __FILE__, __LINE__);	\
	*(volatile __u16 *)_p_;					\
})

#define scr_memsetw(dest, ch, count)	({				\
	__u16 *_dest_ = (dest);						\
	size_t _count_ = (count);					\
	gdccon_check_address (_dest_, _count_, "destitnation",		\
			      "scr_memsetw", __FILE__, __LINE__);	\
	scr_memsetw (_dest_, (ch), _count_);				\
})

#define scr_memcpyw(dest, src, count)	({				\
	__u16 *_dest_ = (dest);						\
	__u16 *_src_ = (src);						\
	size_t _count_ = (count);					\
	gdccon_check_address (_dest_, _count_, "destitnation",		\
			      "scr_memcpyw", __FILE__, __LINE__);	\
	gdccon_check_address (_src_, _count_, "source",			\
			      "scr_memcpyw", __FILE__, __LINE__);	\
	scr_memcpyw (_dest_, _src_, _count_);				\
})

#define scr_memmovew(dest, src, count)	({				\
	__u16 *_dest_ = (dest);						\
	__u16 *_src_ = (src);						\
	size_t _count_ = (count);					\
	gdccon_check_address (_dest_, _count_, "destitnation",		\
			      "scr_memmovew", __FILE__, __LINE__);	\
	gdccon_check_address (_src_, _count_, "source",			\
			      "scr_memmovew", __FILE__, __LINE__);	\
	scr_memmovew (_dest_, _src_, _count_);				\
})

#define scr_memcpyw_from(dest, src, count)	({			\
	__u16 *_dest_ = (dest);						\
	__u16 *_src_ = (src);						\
	size_t _count_ = (count);					\
	gdccon_check_address (_dest_, _count_, "destitnation",		\
			      "scr_memcpyw_from", __FILE__, __LINE__);	\
	gdccon_check_address (_src_, _count_, "source",			\
			      "scr_memcpyw_from", __FILE__, __LINE__);	\
	scr_memcpyw_from (_dest_, _src_, _count_);			\
})
#endif /* GDCCON_DEBUG_MEMFUNCS */

#endif /* _LINUX_ASM_GDC_H_ */
