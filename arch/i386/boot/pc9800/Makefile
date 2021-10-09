#
# arch/i386/boot/pc9800/Makefile
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 1994 by Linus Torvalds
#

BOOT_INCL =	$(TOPDIR)/include/linux/config.h \
		$(TOPDIR)/include/linux/autoconf.h \
		$(TOPDIR)/include/asm/boot.h

all: bootsect setup bbootsect bsetup

bootsect: bootsect.o
	$(LD) -Ttext 0x0 -s --oformat binary -o $@ $<

bootsect.o: bootsect.s
	$(AS) -o $@ $<

bootsect.s: bootsect.S Makefile $(BOOT_INCL)
	$(CPP) $(CPPFLAGS) -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

bbootsect: bbootsect.o
	$(LD) -Ttext 0x0 -s --oformat binary $< -o $@

bbootsect.o: bbootsect.s
	$(AS) -o $@ $<

bbootsect.s: bootsect.S Makefile $(BOOT_INCL)
	$(CPP) $(CPPFLAGS) -D__BIG_KERNEL__ -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

setup: setup.o
	$(LD) -Ttext 0x0 -s --oformat binary -e begtext -o $@ $<

setup.o: setup.s
	$(AS) -o $@ $<

setup.s: setup.S video.S Makefile $(BOOT_INCL) $(TOPDIR)/include/linux/version.h $(TOPDIR)/include/linux/compile.h
	$(CPP) $(CPPFLAGS) -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

bsetup: bsetup.o
	$(LD) -Ttext 0x0 -s --oformat binary -e begtext -o $@ $<

bsetup.o: bsetup.s
	$(AS) -o $@ $<

bsetup.s: setup.S video.S Makefile $(BOOT_INCL) $(TOPDIR)/include/linux/version.h $(TOPDIR)/include/linux/compile.h
	$(CPP) $(CPPFLAGS) -D__BIG_KERNEL__ -traditional $(SVGA_MODE) $(RAMDISK) $< -o $@

dep:

clean:
	rm -f setup bootsect
	rm -f bsetup bbootsect
