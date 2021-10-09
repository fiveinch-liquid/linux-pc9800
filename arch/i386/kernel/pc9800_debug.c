/*
 *  linux/arch/i386/kernel/pc9800_debug.c
 *
 *  Copyright (C) 1998 Linux/98 Project
 *
 * Revised by TAKAI Kousuke, Nov 1999.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kdev_t.h>
#include <linux/console.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/pc9800_debug.h>
#include <asm/pc9800.h>

unsigned char __pc9800_beep_flag = 0x7;

/* pc9800_beep_{on|off|toggle} are moved to <asm/pc9800_debug.h>. */

/* Normal CG window begins at physical address 0xA4000,
   but user-definable characters are on odd address only... */
#define UCG_WINDOW	(phys_to_virt (0xA4001))

#define	UCG_MAX_SIZE	(UCG_LOG_END - UCG_LOG_START + 1)

#define UCG_CHAR_NR(x)	((x) / 32)
#define UCG_2ND_BYTE(x)	((UCG_CHAR_NR (x) & 0x1f) + 0x80)
#define UCG_1ST_BYTE(x)	((UCG_CHAR_NR (x) >> 5) + 0x76 - 0x20 + 0x80)
#define UCG_LR(x)	(((x) & 16) << 1)
#define UCG_LR_MASK	(1U << 5)
#define UCG_OFFSET(x)	((x) % 16)

#ifdef CONFIG_PC9800_UCGLOG
/*
 * Notes for PC-9800 UCG-log facility:
 *
 *  Official specification of PC-9800 says they can have user-definable
 *  character-generator (UCG) RAM for 188 characters, but actual
 *  implementations appear to have 256 characters (`188' seems to come
 *  from adapting it to 94x94 character set).  Thus there is 2KB-odd of
 *  unused area in UCG-RAM (one character consists of 16*16 dots, or
 *  32 bytes).  This area is not touched and its content will be preserved
 *  around system reset.  This facility uses this area for saving
 *  kernel messages.
 *
 * UCG-RAM layout:
 *
 *  Page  Char	Description
 *  ----  ----	----------------------------------------------------
 *   76    00	Magic string and character count in first 16 bytes
 *	   00					(remaining 16 bytes)
 *	    :	Log messages 1/2
 *	   1f
 *	   20	(unused)
 *	   21
 *	    :	Officially used for user-definable characters
 *	   7e
 *	   7f	(unused)
 *   77	   00
 *	    :	Log messages 2/2
 *	   1f
 *	   20	(unused)
 *	   21
 *	    :	Officially used for user-definable characters
 *	   7e
 *	   7f	(unused)
 *
 *    + Characters 20--7f on each page seem to be initialized by
 *	system firmware. 
 */


/* UCG-RAM offsets. */
#define UCG_LOG_MAGIC	0
#define UCG_LOG_HEAD	12
#define UCG_LOG_SIZE	14
#define UCG_LOG_START	16
#define UCG_LOG_END	(32 * 32 * 2 - 1)

#define UCG_MAGIC_STRING	"Linux/98"

static unsigned int ucg_log_head = UCG_LOG_START;
static unsigned int ucg_log_size = 0;

static void
ucglog_write (struct console *console, const char *buf, unsigned int length)
{
	unsigned char *const cg_window = UCG_WINDOW;

	/*
	 * Note that we are called with interrupt disabled
	 * (spin_lock_irqsave in kernel/printk.c).
	 */

	if ((ucg_log_size += length) > UCG_MAX_SIZE)
		ucg_log_size = UCG_MAX_SIZE;

	outb (0x0b, 0x68);	/* bitmap access mode */

	while (length) {
		unsigned char *p;
		unsigned int count;
		u8 lr;

		outb (UCG_2ND_BYTE (ucg_log_head), 0xa1);
		outb (UCG_1ST_BYTE (ucg_log_head), 0xa3);
		lr = UCG_LR (ucg_log_head);
		do {
			outb (lr, 0xa5);
			p = cg_window + UCG_OFFSET (ucg_log_head) * 2;
			count = 16 - UCG_OFFSET (ucg_log_head);
			if (count > length)
				count = length;
			length -= count;
			ucg_log_head += count;
			do {
				*p = *buf++;
				p += 2;
			} while (--count);
		} while (length && (lr ^= UCG_LR_MASK));
	}

	if (ucg_log_head > UCG_LOG_END)
		ucg_log_head = UCG_LOG_START;
	outb (UCG_2ND_BYTE (UCG_LOG_HEAD), 0xa1);
	outb (UCG_1ST_BYTE (UCG_LOG_HEAD), 0xa3);
	outb (UCG_LR (UCG_LOG_HEAD), 0xa5);
	cg_window[(UCG_OFFSET (UCG_LOG_HEAD)    ) * 2] = ucg_log_head;
	cg_window[(UCG_OFFSET (UCG_LOG_HEAD) + 1) * 2] = ucg_log_head >> 8;
#if UCG_CHAR_NR (UCG_LOG_HEAD) != UCG_CHAR_NR (UCG_LOG_SIZE)
	outb (UCG_2ND_BYTE (UCG_LOG_SIZE), 0xa1);
	outb (UCG_1ST_BYTE (UCG_LOG_SIZE), 0xa3);
#endif
#if UCG_LR (UCG_LOG_HEAD) != UCG_LR (UCG_LOG_SIZE)
	outb (UCG_LR (UCG_LOG_SIZE), 0xa5);
#endif
	cg_window[(UCG_OFFSET (UCG_LOG_SIZE)    ) * 2] = ucg_log_size;
	cg_window[(UCG_OFFSET (UCG_LOG_SIZE) + 1) * 2] = ucg_log_size >> 8;

	outb (0x0a, 0x68);
}

static struct console ucglog_console = {
	name:	"ucg",
	write:	ucglog_write,
	setup:	NULL,
	flags:	CON_PRINTBUFFER,
	index:	-1,
};

static int __init
ucglog_init (void)
{
	unsigned long flags;
	const u8 *p;
	u8 *cg_window;
	static const union {
		struct {
			char magic[12];
			u16 start;
			u16 size;
		} s;
		u8 bytes[16];
	} ucg_init_data __initdata = { { UCG_MAGIC_STRING, 0, 0 } };

	if (PC9800_HIGHRESO_P ()) {
		/* Not implemented (yet)... */
		return 0;
	}

	save_flags (flags);
	cli ();
	outb (0x0b, 0x68);	/* bitmap access mode */
	outb (UCG_2ND_BYTE (UCG_LOG_MAGIC), 0xa1);
	outb (UCG_1ST_BYTE (UCG_LOG_MAGIC), 0xa3);
	outb (UCG_LR (UCG_LOG_MAGIC), 0xa5);
	for (cg_window = UCG_WINDOW, p = ucg_init_data.bytes;
	     p < (&ucg_init_data + 1)->bytes; cg_window += 2)
		*cg_window = *p++;
	outb (0x0a, 0x68);
	restore_flags (flags);

	register_console (&ucglog_console);
	printk (KERN_INFO "UCG-RAM console driver installed\n");
	return 0;
}

__initcall (ucglog_init);

#endif /* CONFIG_PC9800_UCGLOG */

/*
#define CONFIG_PC9800_UCGSAVEARGS
*/

#ifdef CONFIG_PC9800_UCGSAVEARGS

#define UCG_SAVEARGS_START	(1 * 32)

void
ucg_saveargs (unsigned int n, ...)
{
	u8 *cg;
	unsigned int count;
	unsigned int addr;
	unsigned long flags;
	const u8 *p = (const u8 *) (&n - 1);

	save_flags (flags);
	cli ();
	outb (0x0b, 0x68);	/* bitmap access mode */
	outb (UCG_2ND_BYTE (UCG_SAVEARGS_START), 0xa1);
	outb (UCG_1ST_BYTE (UCG_SAVEARGS_START), 0xa3);
	outb (UCG_LR (UCG_SAVEARGS_START), 0xa5);
	for (cg = UCG_WINDOW, count = 0; count < 4; count++)
		cg[count * 2] = p[count];

	addr = UCG_SAVEARGS_START + 4;
	for (p += 8; n--; p += 4) {
		if (UCG_OFFSET (addr) == 0) {
			outb (UCG_2ND_BYTE (addr), 0xa1);
			outb (UCG_1ST_BYTE (addr), 0xa3);
			outb (UCG_LR (addr), 0xa5);
		}
		cg[(UCG_OFFSET (addr) + 0) * 2] = p[0];
		cg[(UCG_OFFSET (addr) + 1) * 2] = p[1];
		cg[(UCG_OFFSET (addr) + 2) * 2] = p[2];
		cg[(UCG_OFFSET (addr) + 3) * 2] = p[3];
		addr += 4;
	}

	outb (UCG_2ND_BYTE (0), 0xa1);
	outb (UCG_1ST_BYTE (0), 0xa3);
	outb (UCG_LR (0), 0xa5);

	outb (0x0a, 0x68);
	restore_flags (flags);
}
#endif

#ifdef CONFIG_PC9800_ASSERT
void
__assert_fail (const char *base_file, const char *file, unsigned int line,
	       const char *function, void *return_address, const char *expr)
{
  panic ("In function `%s' (called from [<%p>])\n" KERN_EMERG
	 "%s%s%s%s:%u: Assertion `%s' failed.",
	 function, return_address, file,
	 base_file == file ? "" : " (",
	 base_file == file ? "" : base_file,
	 base_file == file ? "" : ")",
	 line, expr);
}

void
__invalid_kernel_pointer (const char *base_file, const char *file,
			  unsigned int line, const char *function,
			  void *return_address,
			  const char *expr, void *val)
{
  panic ("In function `%s' (called from [<%p>])\n" KERN_EMERG
	 "%s%s%s%s:%u: Invalid kernel pointer `%s' (%p).",
	 function, return_address, file,
	 base_file == file ? "" : " (",
	 base_file == file ? "" : base_file,
	 base_file == file ? "" : ")",
	 line, expr, val);
}

#endif /* CONFIG_PC9800_ASSERT */

unsigned char pc9800_saveregs_enabled;

__asm__ (".text\n"
	 "	.global	" SYMBOL_NAME_STR (__pc9800_saveregs) "\n"
	 SYMBOL_NAME_STR (__pc9800_saveregs) ":\n"
#if 1
	 "	pushfl\n"
	 "	cmpb	$0," SYMBOL_NAME_STR (pc9800_saveregs_enabled) "\n"
	 "	je	1f\n"
	 "	pushl	%edi\n"			/* reverse order of PUSHA */
	 "	pushl	%esi\n"
	 "	pushl	%ebp\n"
	 "	leal	20(%esp),%esi\n"	/* original ESP */
	 "	pushl	%esi\n"
	 "	pushl	%ebx\n"
	 "	pushl	%edx\n"
	 "	pushl	%ecx\n"
	 "	pushl	%eax\n"
	 "	movl	$0xc0000780,%edi\n"	/* save few words on stack */
	 "	movl	$20, %ecx\n"
	 "	cld; rep; ss; movsl\n"		/* EDI becomes 0xC00007D0 */
	 "	subl	$(20+1+1+8)*4,%esi\n"	/* ESI points EAX on stack */
	 "	movl	$8,%ecx\n"
	 "	rep; ss; movsl\n"		/* save GP registers */
	 "	ss; lodsl\n"			/* EFLAGS */
	 "	ss; movsl\n"			/* save EIP */
	 "	stosl\n"			/* save EFLAGS */
	 "	movl	%cr3,%eax\n"		/* save control registers */
	 "	stosl\n"
	 "	movl	%cr0,%eax\n"
	 "	stosl\n"
	 "	popl	%eax\n"
	 "	popl	%ecx\n"
	 "	addl	$4*4,%esp\n"		/* discard EDX/EBX/ESP/EBP */
	 "	popl	%esi\n"
	 "	popl	%edi\n"
	 "1:	popfl\n"
#else
	 "	cmpb	$0," SYMBOL_NAME_STR (pc9800_saveregs_enabled) "\n"
	 "	je	1f\n"
	 "	pushl	%eax\n"
	 "	movl	%eax,0xc00007d0\n"
	 "	movl	%ecx,0xc00007d4\n"
	 "	movl	%edx,0xc00007d8\n"
	 "	movl	%ebx,0xc00007dc\n"
	 "	leal	8(%esp),%eax\n"		/* original ESP */
	 "	movl	%eax,0xc00007e0\n"
	 "	movl	%ebp,0xc00007e4\n"
	 "	movl	%esi,0xc00007e8\n"
	 "	movl	%edi,0xc00007ec\n"
	 "	movl	4(%esp),%eax\n"		/* EIP as return address */
	 "	movl	%eax,0xc00007f0\n"
	 "	pushfl\n"
	 "	popl	%eax\n"
	 "	movl	%eax,0xc00007f4\n"
	 "	movl	%cr3,%eax\n"
	 "	movl	%eax,0xc00007f8\n"
	 "	movl	%cr0,%eax\n"
	 "	movl	%eax,0xc00007fc\n"
	 "	pushl	%ecx\n"
	 "	pushl	%esi\n"
	 "	pushl	%edi\n"
	 "	leal	20(%esp),%esi\n"
	 "	movl	$0xc0000780,%edi\n"
	 "	movl	$16,%ecx\n"
	 "	cld; rep; ss; movsl\n"
	 "	popl	%edi\n"
	 "	popl	%esi\n"
	 "	popl	%ecx\n"
	 "	popl	%eax\n"
	 "1:\n"
#endif
	 "	ret");

__asm__ (".weak mcount; mcount = __pc9800_saveregs");

#if 0
int
test_mcount (void)
{
	printk ("Calling mcount...\n");
	pc9800_saveregs_enabled = 1;
	mcount ();
}

__initcall (test_mcount);
#endif
