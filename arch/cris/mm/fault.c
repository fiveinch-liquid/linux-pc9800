/*
 *  linux/arch/cris/mm/fault.c
 *
 *  Copyright (C) 2000, 2001  Axis Communications AB
 *
 *  Authors:  Bjorn Wesen 
 * 
 *  $Log: fault.c,v $
 *  Revision 1.16  2001/06/13 00:06:08  bjornw
 *  current_pgd should be volatile
 *
 *  Revision 1.15  2001/06/13 00:02:23  bjornw
 *  Use a separate variable to store the current pgd to avoid races in schedule
 *
 *  Revision 1.14  2001/05/16 17:41:07  hp
 *  Last comment tweak further tweaked.
 *
 *  Revision 1.13  2001/05/15 00:58:44  hp
 *  Expand a bit on the comment why we compare address >= TASK_SIZE rather
 *  than >= VMALLOC_START.
 *
 *  Revision 1.12  2001/04/04 10:51:14  bjornw
 *  mmap_sem is grabbed for reading
 *
 *  Revision 1.11  2001/03/23 07:36:07  starvik
 *  Corrected according to review remarks
 *
 *  Revision 1.10  2001/03/21 16:10:11  bjornw
 *  CRIS_FRAME_FIXUP not needed anymore, use FRAME_NORMAL
 *
 *  Revision 1.9  2001/03/05 13:22:20  bjornw
 *  Spell-fix and fix in vmalloc_fault handling
 *
 *  Revision 1.8  2000/11/22 14:45:31  bjornw
 *  * 2.4.0-test10 removed the set_pgdir instantaneous kernel global mapping
 *    into all processes. Instead we fill in the missing PTE entries on demand.
 *
 *  Revision 1.7  2000/11/21 16:39:09  bjornw
 *  fixup switches frametype
 *
 *  Revision 1.6  2000/11/17 16:54:08  bjornw
 *  More detailed siginfo reporting
 *
 *
 */

#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/svinto.h>

extern void die_if_kernel(const char *,struct pt_regs *,long);

asmlinkage void do_invalid_op (struct pt_regs *, unsigned long);
asmlinkage void do_page_fault(unsigned long address, struct pt_regs *regs,
			      int error_code);

/* debug of low-level TLB reload */
#define D(x)
/* debug of higher-level faults */
#define DPG(x)

/* current active page directory */

volatile pgd_t *current_pgd;

/* fast TLB-fill fault handler */

void
handle_mmu_bus_fault(struct pt_regs *regs)
{
	int cause, select;
	int index;
	int page_id;
	int miss, we, acc, inv;  
	struct mm_struct *mm = current->active_mm;
	pmd_t *pmd;
	pte_t pte;
	int errcode = 0;
	unsigned long address;

	cause = *R_MMU_CAUSE;
	select = *R_TLB_SELECT;

	address = cause & PAGE_MASK; /* get faulting address */
	
	D(page_id = IO_EXTRACT(R_MMU_CAUSE,  page_id,   cause));
	D(acc     = IO_EXTRACT(R_MMU_CAUSE,  acc_excp,  cause));
	D(inv     = IO_EXTRACT(R_MMU_CAUSE,  inv_excp,  cause));  
	D(index  =  IO_EXTRACT(R_TLB_SELECT, index,     select));
	miss    = IO_EXTRACT(R_MMU_CAUSE,  miss_excp, cause);
	we      = IO_EXTRACT(R_MMU_CAUSE,  we_excp,   cause);
	
	D(printk("bus_fault from IRP 0x%x: addr 0x%x, miss %d, inv %d, we %d, acc %d, "
		 "idx %d pid %d\n",
		 regs->irp, address, miss, inv, we, acc, index, page_id));

	/* for a miss, we need to reload the TLB entry */

	if(miss) {

		/* see if the pte exists at all
		 * refer through current_pgd, dont use mm->pgd
		 */
		
		pmd = (pmd_t *)(current_pgd + pgd_index(address));
		if(pmd_none(*pmd))
			goto dofault;
		if(pmd_bad(*pmd)) {
			printk("bad pgdir entry 0x%x at 0x%x\n", *pmd, pmd);
			pmd_clear(pmd);
			return;
		}
		pte = *pte_offset(pmd, address);
		if(!pte_present(pte))
			goto dofault;
		
		D(printk(" found pte %x pg %x ", pte_val(pte), pte_page(pte)));
		D(
		  {
			  if(pte_val(pte) & _PAGE_SILENT_WRITE)
				  printk("Silent-W ");
			  if(pte_val(pte) & _PAGE_KERNEL)
				  printk("Kernel ");
			  if(pte_val(pte) & _PAGE_SILENT_READ)
				  printk("Silent-R ");
			  if(pte_val(pte) & _PAGE_GLOBAL)
				  printk("Global ");
			  if(pte_val(pte) & _PAGE_PRESENT)
				  printk("Present ");
			  if(pte_val(pte) & _PAGE_ACCESSED)
				  printk("Accessed ");
			  if(pte_val(pte) & _PAGE_MODIFIED)
				  printk("Modified ");
			  if(pte_val(pte) & _PAGE_READ)
				  printk("Readable ");
			  if(pte_val(pte) & _PAGE_WRITE)
				  printk("Writeable ");
			  printk("\n");
		  });

		/* load up the chosen TLB entry
		 * this assumes the pte format is the same as the TLB_LO layout.
		 *
		 * the write to R_TLB_LO also writes the vpn and page_id fields from
		 * R_MMU_CAUSE, which we in this case obviously want to keep
		 */
		
		*R_TLB_LO = pte_val(pte);

		return;
	} 

	errcode = 0x01 | (we << 1);

 dofault:
	/* leave it to the MM system fault handler below */
	D(printk("do_page_fault %p errcode %d\n", address, errcode));
	do_page_fault(address, regs, errcode);
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 *
 * Notice that the address we're given is aligned to the page the fault
 * occurred in, since we only get the PFN in R_MMU_CAUSE not the complete
 * address.
 *
 * error_code:
 *	bit 0 == 0 means no page found, 1 means protection fault
 *	bit 1 == 0 means read, 1 means write
 *
 * If this routine detects a bad access, it returns 1, otherwise it
 * returns 0.
 */

asmlinkage void
do_page_fault(unsigned long address, struct pt_regs *regs,
	      int error_code)
{
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct * vma;
	int writeaccess;
	int fault;
	unsigned long fixup;
	siginfo_t info;

	tsk = current;

        /*
         * We fault-in kernel-space virtual memory on-demand. The
         * 'reference' page table is init_mm.pgd.
         *
         * NOTE! We MUST NOT take any locks for this case. We may
         * be in an interrupt or a critical region, and should
         * only copy the information from the master page table,
         * nothing more.
	 *
	 * NOTE2: This is done so that, when updating the vmalloc
	 * mappings we don't have to walk all processes pgdirs and
	 * add the high mappings all at once. Instead we do it as they
	 * are used.
	 *
	 * TODO: On CRIS, we have a PTE Global bit which should be set in
	 * all the PTE's related to vmalloc in all processes - that means if
	 * we switch process and a vmalloc PTE is still in the TLB, it won't
	 * need to be reloaded. It's an optimization.
	 *
	 * Linux/CRIS's kernel is not page-mapped, so the comparision below
	 * should really be >= VMALLOC_START, however, kernel fixup errors
	 * will be handled more quickly by going through vmalloc_fault and then
	 * into bad_area_nosemaphore than falling through the find_vma user-mode
	 * tests.  As an aside can be mentioned that the difference in
	 * compiled code is neglibible; the instruction is the same, just a
	 * comparison with a different address of the same size.
         */

        if (address >= TASK_SIZE)
                goto vmalloc_fault;

	mm = tsk->mm;
	writeaccess = error_code & 2;
	info.si_code = SEGV_MAPERR;

	/*
	 * If we're in an interrupt or have no user
	 * context, we must not take the fault..
	 */

	if (in_interrupt() || !mm)
		goto no_context;

	down_read(&mm->mmap_sem);
	vma = find_vma(mm, address);
	if (!vma)
		goto bad_area;
	if (vma->vm_start <= address)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (user_mode(regs)) {
		/*
		 * accessing the stack below usp is always a bug.
		 * we get page-aligned addresses so we can only check
		 * if we're within a page from usp, but that might be
		 * enough to catch brutal errors at least.
		 */
		if (address + PAGE_SIZE < rdusp())
			goto bad_area;
	}
	if (expand_stack(vma, address))
		goto bad_area;

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */

 good_area:
	info.si_code = SEGV_ACCERR;

	/* first do some preliminary protection checks */

	if (writeaccess) {
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
	} else {
		if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
			goto bad_area;
	}

	/*
	 * If for any reason at all we couldn't handle the fault,
	 * make sure we exit gracefully rather than endlessly redo
	 * the fault.
	 */

	switch (handle_mm_fault(mm, vma, address, writeaccess)) {
        case 1:
                tsk->min_flt++;
                break;
        case 2:
                tsk->maj_flt++;
                break;
        case 0:
                goto do_sigbus;
        default:
                goto out_of_memory;
	}

	up_read(&mm->mmap_sem);
	return;
	
	/*
	 * Something tried to access memory that isn't in our memory map..
	 * Fix it, but check if it's kernel or user first..
	 */

 bad_area:

	up_read(&mm->mmap_sem);

 bad_area_nosemaphore:
	DPG(show_registers(regs));

	/* User mode accesses just cause a SIGSEGV */

	if (user_mode(regs)) {
		info.si_signo = SIGSEGV;
		info.si_errno = 0;
		/* info.si_code has been set above */
		info.si_addr = (void *)address;
		force_sig_info(SIGSEGV, &info, tsk);
		return;
	}

 no_context:

	/* Are we prepared to handle this kernel fault?
	 *
	 * (The kernel has valid exception-points in the source 
	 *  when it acesses user-memory. When it fails in one
	 *  of those points, we find it in a table and do a jump
	 *  to some fixup code that loads an appropriate error
	 *  code)
	 */

        if ((fixup = search_exception_table(regs->irp)) != 0) {
		/* Adjust the instruction pointer in the stackframe */

                regs->irp = fixup;

		/* We do not want to return by restoring the CPU-state
		 * anymore, so switch frame-types (see ptrace.h)
		 */

		regs->frametype = CRIS_FRAME_NORMAL;

		D(printk("doing fixup to 0x%x\n", fixup));
                return;
        }

	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice.
	 */

	if ((unsigned long) (address) < PAGE_SIZE)
		printk(KERN_ALERT "Unable to handle kernel NULL pointer dereference");
	else
		printk(KERN_ALERT "Unable to handle kernel access");
	printk(" at virtual address %08lx\n",address);

	die_if_kernel("Oops", regs, error_code);

	do_exit(SIGKILL);

	/*
	 * We ran out of memory, or some other thing happened to us that made
	 * us unable to handle the page fault gracefully.
	 */

 out_of_memory:
        up_read(&mm->mmap_sem);
	printk("VM: killing process %s\n", tsk->comm);
	if(user_mode(regs))
		do_exit(SIGKILL);
	goto no_context;

 do_sigbus:
	up_read(&mm->mmap_sem);

	/*
         * Send a sigbus, regardless of whether we were in kernel
         * or user mode.
         */
	info.si_code = SIGBUS;
	info.si_errno = 0;
	info.si_code = BUS_ADRERR;
	info.si_addr = (void *)address;
	force_sig_info(SIGBUS, &info, tsk);
	
        /* Kernel mode? Handle exceptions or die */
        if (!user_mode(regs))
                goto no_context;
        return;

vmalloc_fault:
        {
                /*
                 * Synchronize this task's top level page-table
                 * with the 'reference' page table.
		 *
		 * Use current_pgd instead of tsk->active_mm->pgd
		 * since the latter might be unavailable if this
		 * code is executed in a misfortunately run irq
                 */

                int offset = pgd_index(address);
                pgd_t *pgd, *pgd_k;
                pmd_t *pmd, *pmd_k;

                pgd = current_pgd + offset;
                pgd_k = init_mm.pgd + offset;

                if (!pgd_present(*pgd)) {
                        if (!pgd_present(*pgd_k))
                                goto bad_area_nosemaphore;
                        set_pgd(pgd, *pgd_k);
                        return;
                }

                pmd = pmd_offset(pgd, address);
                pmd_k = pmd_offset(pgd_k, address);

                if (!pmd_present(*pmd_k))
                        goto bad_area_nosemaphore;
                set_pmd(pmd, *pmd_k);
                return;
        }

}
