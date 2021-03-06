/*
 *  linux/arch/arm/mach-ebsa110/isamem.c
 *
 *  Copyright (C) 2001 Russell King
 *
 * Perform "ISA" memory and IO accesses.  The EBSA110 has some "peculiarities"
 * in the way it handles accesses to odd IO ports on 16-bit devices.  These
 * devices have their D0-D15 lines connected to the processors D0-D15 lines.
 * Since they expect all byte IO operations to be performed on D0-D7, and the
 * StrongARM expects to transfer the byte to these odd addresses on D8-D15,
 * we must use a trick to get the required behaviour.
 *
 * The trick employed here is to use long word stores to odd address -1.  The
 * glue logic picks this up as a "trick" access, and asserts the LSB of the
 * peripherals address bus, thereby accessing the odd IO port.  Meanwhile, the
 * StrongARM transfers its data on D0-D7 as expected.
 *
 * Things get more interesting on the pass-1 EBSA110 - the PCMCIA controller
 * wiring was screwed in such a way that it had limited memory space access.
 * Luckily, the work-around for this is not too horrible.  See
 * __isamem_convert_addr for the details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <asm/io.h>
#include <asm/page.h>

static u32 __isamem_convert_addr(void *addr)
{
	u32 ret, a = (u32) addr;

	/*
	 * The PCMCIA controller is wired up as follows:
	 *        +---------+---------+---------+---------+---------+---------+
	 * PCMCIA | 2 2 2 2 | 1 1 1 1 | 1 1 1 1 | 1 1     |         |         |
	 *        | 3 2 1 0 | 9 8 7 6 | 5 4 3 2 | 1 0 9 8 | 7 6 5 4 | 3 2 1 0 |
	 *        +---------+---------+---------+---------+---------+---------+
	 *  CPU   | 2 2 2 2 | 2 1 1 1 | 1 1 1 1 | 1 1 1   |         |         |
	 *        | 4 3 2 1 | 0 9 9 8 | 7 6 5 4 | 3 2 0 9 | 8 7 6 5 | 4 3 2 x |
	 *        +---------+---------+---------+---------+---------+---------+
	 *
	 * This means that we can access PCMCIA regions as follows:
	 *	0x*10000 -> 0x*1ffff
	 *	0x*70000 -> 0x*7ffff
	 *	0x*90000 -> 0x*9ffff
	 *	0x*f0000 -> 0x*fffff
	 */
	ret  = (a & 0xf803fe) << 1;
	ret |= (a & 0x03fc00) << 2;

	ret += 0xe8000000;

	if ((a & 0x20000) == (a & 0x40000) >> 1)
		return ret;

	BUG();
	return 0;
}

/*
 * read[bwl] and write[bwl]
 */
u8 __readb(void *addr)
{
	u32 ret, a = __isamem_convert_addr(addr);

	if ((int)addr & 1)
		ret = __arch_getl(a);
	else
		ret = __arch_getb(a);
	return ret;
}

u16 __readw(void *addr)
{
	u32 a = __isamem_convert_addr(addr);

	if ((int)addr & 1)
		BUG();

	return __arch_getw(a);
}

u32 __readl(void *addr)
{
	u32 ret, a = __isamem_convert_addr(addr);

	if ((int)addr & 3)
		BUG();

	ret = __arch_getw(a);
	ret |= __arch_getw(a + 4) << 16;
	return ret;
}

EXPORT_SYMBOL(__readb);
EXPORT_SYMBOL(__readw);
EXPORT_SYMBOL(__readl);

void __writeb(u8 val, void *addr)
{
	u32 a = __isamem_convert_addr(addr);

	if ((int)addr & 1)
		__arch_putl(val, a);
	else
		__arch_putb(val, a);
}

void __writew(u16 val, void *addr)
{
	u32 a = __isamem_convert_addr(addr);

	if ((int)addr & 1)
		BUG();

	__arch_putw(val, a);
}

void __writel(u32 val, void *addr)
{
	u32 a = __isamem_convert_addr(addr);

	if ((int)addr & 3)
		BUG();

	__arch_putw(val, a);
	__arch_putw(val >> 16, a + 4);
}

EXPORT_SYMBOL(__writeb);
EXPORT_SYMBOL(__writew);
EXPORT_SYMBOL(__writel);

#define SUPERIO_PORT(p) \
	(((p) >> 3) == (0x3f8 >> 3) || \
	 ((p) >> 3) == (0x2f8 >> 3) || \
	 ((p) >> 3) == (0x378 >> 3))

u8 __inb(int port)
{
	u32 ret;

	/*
	 * The SuperIO registers use sane addressing techniques...
	 */
	if (SUPERIO_PORT(port))
		ret = __arch_getb(ISAIO_BASE + (port << 2));
	else {
		u32 a = ISAIO_BASE + ((port & ~1) << 1);

		/*
		 * Shame nothing else does
		 */
		if (port & 1)
			ret = __arch_getl(a);
		else
			ret = __arch_getb(a);
	}
	return ret;
}

u16 __inw(int port)
{
	u32 ret;

	/*
	 * The SuperIO registers use sane addressing techniques...
	 */
	if (SUPERIO_PORT(port))
		ret = __arch_getw(ISAIO_BASE + (port << 2));
	else {
		u32 a = ISAIO_BASE + ((port & ~1) << 1);

		/*
		 * Shame nothing else does
		 */
		if (port & 1)
			BUG();

		ret = __arch_getw(a);
	}
	return ret;
}

u32 __inl(int port)
{
	BUG();
	return 0;
}

EXPORT_SYMBOL(__inb);
EXPORT_SYMBOL(__inw);
EXPORT_SYMBOL(__inl);

void __outb(u8 val, int port)
{
	/*
	 * The SuperIO registers use sane addressing techniques...
	 */
	if (SUPERIO_PORT(port))
		__arch_putb(val, ISAIO_BASE + (port << 2));
	else {
		u32 a = ISAIO_BASE + ((port & ~1) << 1);

		/*
		 * Shame nothing else does
		 */
		if (port & 1)
			__arch_putl(val, a);
		else
			__arch_putb(val, a);
	}
}

void __outw(u16 val, int port)
{
	u32 off;

	/*
	 * The SuperIO registers use sane addressing techniques...
	 */
	if (SUPERIO_PORT(port))
		off = port << 2;
	else {
		off = (port & ~1) << 1;
		if (port & 1)
			BUG();

	}
	__arch_putw(val, ISAIO_BASE + off);
}

void __outl(u32 val, int port)
{
	BUG();
}

EXPORT_SYMBOL(__outb);
EXPORT_SYMBOL(__outw);
EXPORT_SYMBOL(__outl);

extern void __arch_writesb(unsigned long virt, const void *from, int len);
extern void __arch_writesw(unsigned long virt, const void *from, int len);
extern void __arch_writesl(unsigned long virt, const void *from, int len);
extern void __arch_readsb(unsigned long virt, void *from, int len);
extern void __arch_readsw(unsigned long virt, void *from, int len);
extern void __arch_readsl(unsigned long virt, void *from, int len);

void outsb(unsigned int port, const void *from, int len)
{
	u32 off;

	if (SUPERIO_PORT(port))
		off = port << 2;
	else {
		off = (port & ~1) << 1;
		if (port & 1)
			BUG();
	}

	__arch_writesb(ISAIO_BASE + off, from, len);
}

void insb(unsigned int port, void *from, int len)
{
	u32 off;

	if (SUPERIO_PORT(port))
		off = port << 2;
	else {
		off = (port & ~1) << 1;
		if (port & 1)
			BUG();
	}

	__arch_readsb(ISAIO_BASE + off, from, len);
}

void outsw(unsigned int port, const void *from, int len)
{
	u32 off;

	if (SUPERIO_PORT(port))
		off = port << 2;
	else {
		off = (port & ~1) << 1;
		if (port & 1)
			BUG();
	}

	__arch_writesw(ISAIO_BASE + off, from, len);
}

void insw(unsigned int port, void *from, int len)
{
	u32 off;

	if (SUPERIO_PORT(port))
		off = port << 2;
	else {
		off = (port & ~1) << 1;
		if (port & 1)
			BUG();
	}

	__arch_readsw(ISAIO_BASE + off, from, len);
}

void outsl(unsigned int port, const void *from, int len)
{
	panic("outsl not supported on this architecture");
}

void insl(unsigned int port, void *from, int len)
{
	panic("insl not supported on this architecture");
}
