/*
 *  drivers/s390/char/controlchar.c
 *  Unified handling of special chars.
 *
 *    Copyright (C) 2001 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Fritz Elfert <felfert@millenux.com> <elfert@de.ibm.com>
 *
 */

#include <linux/types.h>
#include <linux/tty.h>

extern void ctrlchar_init(void);
extern char *ctrlchar_handle(const char *buf, int len, struct tty_struct *tty);
