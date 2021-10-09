/*
 * linux/include/asm-i386/pc9800_debug.h: Defines for debug routines
 *
 * Written by Linux/98 Project, 1998.
 *
 * Revised by TAKAI Kousuke, Nov 1999.
 */

#ifndef _ASM_PC9800_DEBUG_H
#define _ASM_PC9800_DEBUG_H

extern unsigned char __pc9800_beep_flag;

/* Don't depend on <asm/io.h> ... */
static __inline__ void pc9800_beep_on (void)
{
	__asm__ ("out%B0 %0,%1" : : "a"((char) 0x6), "N"(0x37));
	__pc9800_beep_flag = 0x6;
}

static __inline__ void pc9800_beep_off (void)
{
	__asm__ ("out%B0 %0,%1" : : "a"((char) 0x7), "N"(0x37));
	__pc9800_beep_flag = 0x7;
}

static __inline__ void pc9800_beep_toggle (void)
{
	__pc9800_beep_flag ^= 1;
	__asm__ ("out%B0 %0,%1" : : "a"(__pc9800_beep_flag), "N"(0x37));
}

static __inline__ void pc9800_udelay (unsigned int __usec)
{
	if ((__usec = __usec * 10 / 6) > 0)
		do
			__asm__ ("out%B0 %%al,%0" : : "N"(0x5F));
		while (--__usec);
}

# define assertk(expr)	({						\
	if (!(expr)) {							\
		__pc9800_saveregs ();					\
		__assert_fail (__BASE_FILE__, __FILE__, __LINE__,	\
			       __PRETTY_FUNCTION__,			\
			       __builtin_return_address (0), #expr);	\
	}								\
})
# define check_kernel_pointer(expr)	({				\
	void *__p = (expr);						\
	if ((unsigned long) __p < PAGE_OFFSET) {			\
		__pc9800_saveregs ();					\
		__invalid_kernel_pointer				\
			(__BASE_FILE__, __FILE__, __LINE__,		\
			 __PRETTY_FUNCTION__,				\
			 __builtin_return_address (0), #expr, __p);	\
	}								\
})

extern void __assert_fail (const char *, const char *, unsigned int,
			   const char *, void *, const char *)
     __attribute__ ((__noreturn__));
extern void __invalid_kernel_pointer (const char *, const char *, unsigned int,
				      const char *, void *,
				      const char *, void *)
     __attribute__ ((__noreturn__));
extern void __pc9800_saveregs (void);

extern void ucg_saveargs (unsigned int, ...);

#ifdef NEED_UNMAP_PHYSPAGE

#include <linux/mm.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>

/*
 * unmap_physpage (addr)
 */
static __inline__ void
unmap_physpage (unsigned long addr)
{
	pgd_t *pgd = pgd_offset_k (addr);
	pmd_t *pmd;
	pte_t *pte;

	if (pgd_none (*pgd))
		panic ("%s: No pgd?!?", __BASE_FILE__);
	pmd = pmd_offset (pgd, addr);
	if (pmd_none (*pmd))
		panic ("%s: No pmd?!?", __BASE_FILE__);
	if (pmd_val (*pmd) & _PAGE_PSE) {
		int i;
		unsigned long paddr = pmd_val (*pmd) & PAGE_MASK;

		pte = (pte_t *) __get_free_page (GFP_KERNEL);
		set_pmd (pmd, __pmd (_KERNPG_TABLE + __pa(pte)));
		for (i = 0; i < PTRS_PER_PTE; pte++, i++)
			*pte = mk_pte_phys (paddr + i * PAGE_SIZE,
					    PAGE_KERNEL);
	}
	pte = pte_offset (pmd, addr);

	set_pte (pte, pte_modify (*pte, PAGE_NONE));
	__flush_tlb_one (addr);
}
#endif /* NEED_UNMAP_PHYSPAGE */

#ifdef NEED_KERNEL_POINTER_CHECKER
# /* no dep */ include <asm/uaccess.h>

/*
 *  KERNEL_POINTER_VALIDP(PTR) validates kernel pointer PTR.
 *  If PTR points vaild memory address for kernel internal use,
 *  return nonzero; otherwise return zero.
 *
 *  Note PTR is invalid if PTR points user area.
 */
#define KERNEL_POINTER_VALIDP(PTR)	({	\
	const int *_ptr = (const int *) (PTR);	\
	int _dummy;				\
	(unsigned long) _ptr >= PAGE_OFFSET	\
		&& !__get_user (_dummy, _ptr);	\
})

/*
 *  Similar, but validates for a trivial string in kernel.
 *  Here `trivial' means that the string has no non-ASCII characters
 *  and is shorter than 80 characters.
 *
 *  Note this is intended for checking various `name' (I/O
 *  resources and so on).
 */
#define KERNEL_POINTER_TRIVIAL_STRING_P(PTR)	({			\
	const char *_ptr = (const char *) (PTR);			\
	char _dummy;							\
	(unsigned long) _ptr >= PAGE_OFFSET				\
		&& ({ int _result;					\
		      while (!(_result = __get_user (_dummy, _ptr))	\
			     && _dummy)					\
			      _ptr++;					\
		      !_result; }); })

#endif /* NEED_VALIDATE_KERNEL_POINTER */

#endif /* _ASM_PC9800_DEBUG_H */

/*
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
