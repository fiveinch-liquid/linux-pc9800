/*
 *  linux/mm/vmscan.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *
 *  Swap reorganised 29.12.95, Stephen Tweedie.
 *  kswapd added: 7.1.96  sct
 *  Removed kswapd_ctl limits, and swap out as many pages as needed
 *  to bring the system back to freepages.high: 2.4.97, Rik van Riel.
 *  Version: $Id: vmscan.c,v 1.5 1998/02/23 22:14:28 sct Exp $
 *  Zone aware kswapd started 02/00, Kanoj Sarcar (kanoj@sgi.com).
 *  Multiqueue VM started 5.8.00, Rik van Riel.
 */

#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapctl.h>
#include <linux/smp_lock.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/highmem.h>
#include <linux/file.h>

#include <asm/pgalloc.h>

#define MAX(a,b) ((a) > (b) ? (a) : (b))

/*
 * The swap-out function returns 1 if it successfully
 * scanned all the pages it was asked to (`count').
 * It returns zero if it couldn't do anything,
 *
 * rss may decrease because pages are shared, but this
 * doesn't count as having freed a page.
 */

/* mm->page_table_lock is held. mmap_sem is not held */
static void try_to_swap_out(struct mm_struct * mm, struct vm_area_struct* vma, unsigned long address, pte_t * page_table, struct page *page)
{
	pte_t pte;
	swp_entry_t entry;

	/* Don't look at this pte if it's been accessed recently. */
	if (ptep_test_and_clear_young(page_table)) {
		page->age += PAGE_AGE_ADV;
		if (page->age > PAGE_AGE_MAX)
			page->age = PAGE_AGE_MAX;
		return;
	}

	if (TryLockPage(page))
		return;

	/* From this point on, the odds are that we're going to
	 * nuke this pte, so read and clear the pte.  This hook
	 * is needed on CPUs which update the accessed and dirty
	 * bits in hardware.
	 */
	pte = ptep_get_and_clear(page_table);
	flush_tlb_page(vma, address);

	/*
	 * Is the page already in the swap cache? If so, then
	 * we can just drop our reference to it without doing
	 * any IO - it's already up-to-date on disk.
	 */
	if (PageSwapCache(page)) {
		entry.val = page->index;
		if (pte_dirty(pte))
			set_page_dirty(page);
set_swap_pte:
		swap_duplicate(entry);
		set_pte(page_table, swp_entry_to_pte(entry));
drop_pte:
		mm->rss--;
		if (!page->age)
			deactivate_page(page);
		UnlockPage(page);
		page_cache_release(page);
		return;
	}

	/*
	 * Is it a clean page? Then it must be recoverable
	 * by just paging it in again, and we can just drop
	 * it..
	 *
	 * However, this won't actually free any real
	 * memory, as the page will just be in the page cache
	 * somewhere, and as such we should just continue
	 * our scan.
	 *
	 * Basically, this just makes it possible for us to do
	 * some real work in the future in "refill_inactive()".
	 */
	flush_cache_page(vma, address);
	if (!pte_dirty(pte))
		goto drop_pte;

	/*
	 * Ok, it's really dirty. That means that
	 * we should either create a new swap cache
	 * entry for it, or we should write it back
	 * to its own backing store.
	 */
	if (page->mapping) {
		set_page_dirty(page);
		goto drop_pte;
	}

	/*
	 * This is a dirty, swappable page.  First of all,
	 * get a suitable swap entry for it, and make sure
	 * we have the swap cache set up to associate the
	 * page with that swap entry.
	 */
	entry = get_swap_page();
	if (!entry.val)
		goto out_unlock_restore; /* No swap space left */

	/* Add it to the swap cache and mark it dirty */
	add_to_swap_cache(page, entry);
	set_page_dirty(page);
	goto set_swap_pte;

out_unlock_restore:
	set_pte(page_table, pte);
	UnlockPage(page);
	return;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static int swap_out_pmd(struct mm_struct * mm, struct vm_area_struct * vma, pmd_t *dir, unsigned long address, unsigned long end, int count)
{
	pte_t * pte;
	unsigned long pmd_end;

	if (pmd_none(*dir))
		return count;
	if (pmd_bad(*dir)) {
		pmd_ERROR(*dir);
		pmd_clear(dir);
		return count;
	}
	
	pte = pte_offset(dir, address);
	
	pmd_end = (address + PMD_SIZE) & PMD_MASK;
	if (end > pmd_end)
		end = pmd_end;

	do {
		if (pte_present(*pte)) {
			struct page *page = pte_page(*pte);

			if (VALID_PAGE(page) && !PageReserved(page)) {
				try_to_swap_out(mm, vma, address, pte, page);
				if (!--count)
					break;
			}
		}
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));
	mm->swap_address = address + PAGE_SIZE;
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static inline int swap_out_pgd(struct mm_struct * mm, struct vm_area_struct * vma, pgd_t *dir, unsigned long address, unsigned long end, int count)
{
	pmd_t * pmd;
	unsigned long pgd_end;

	if (pgd_none(*dir))
		return count;
	if (pgd_bad(*dir)) {
		pgd_ERROR(*dir);
		pgd_clear(dir);
		return count;
	}

	pmd = pmd_offset(dir, address);

	pgd_end = (address + PGDIR_SIZE) & PGDIR_MASK;	
	if (pgd_end && (end > pgd_end))
		end = pgd_end;
	
	do {
		count = swap_out_pmd(mm, vma, pmd, address, end, count);
		if (!count)
			break;
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return count;
}

/* mm->page_table_lock is held. mmap_sem is not held */
static int swap_out_vma(struct mm_struct * mm, struct vm_area_struct * vma, unsigned long address, int count)
{
	pgd_t *pgdir;
	unsigned long end;

	/* Don't swap out areas which are locked down */
	if (vma->vm_flags & (VM_LOCKED|VM_RESERVED))
		return count;

	pgdir = pgd_offset(mm, address);

	end = vma->vm_end;
	if (address >= end)
		BUG();
	do {
		count = swap_out_pgd(mm, vma, pgdir, address, end, count);
		if (!count)
			break;
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		pgdir++;
	} while (address && (address < end));
	return count;
}

/*
 * Returns non-zero if we scanned all `count' pages
 */
static int swap_out_mm(struct mm_struct * mm, int count)
{
	unsigned long address;
	struct vm_area_struct* vma;

	if (!count)
		return 1;
	/*
	 * Go through process' page directory.
	 */

	/*
	 * Find the proper vm-area after freezing the vma chain 
	 * and ptes.
	 */
	spin_lock(&mm->page_table_lock);
	address = mm->swap_address;
	vma = find_vma(mm, address);
	if (vma) {
		if (address < vma->vm_start)
			address = vma->vm_start;

		for (;;) {
			count = swap_out_vma(mm, vma, address, count);
			if (!count)
				goto out_unlock;
			vma = vma->vm_next;
			if (!vma)
				break;
			address = vma->vm_start;
		}
	}
	/* Reset to 0 when we reach the end of address space */
	mm->swap_address = 0;

out_unlock:
	spin_unlock(&mm->page_table_lock);
	return !count;
}

#define SWAP_MM_SHIFT	4
#define SWAP_SHIFT	5
#define SWAP_MIN	8

static inline int swap_amount(struct mm_struct *mm)
{
	int nr = mm->rss >> SWAP_SHIFT;
	if (nr < SWAP_MIN) {
		nr = SWAP_MIN;
		if (nr > mm->rss)
			nr = mm->rss;
	}
	return nr;
}

static void swap_out(unsigned int priority, int gfp_mask)
{
	int counter;
	int retval = 0;
	struct mm_struct *mm = current->mm;

	/* Always start by trying to penalize the process that is allocating memory */
	if (mm)
		retval = swap_out_mm(mm, swap_amount(mm));

	/* Then, look at the other mm's */
	counter = (mmlist_nr << SWAP_MM_SHIFT) >> priority;
	do {
		struct list_head *p;

		spin_lock(&mmlist_lock);
		p = init_mm.mmlist.next;
		if (p == &init_mm.mmlist)
			goto empty;

		/* Move it to the back of the queue.. */
		list_del(p);
		list_add_tail(p, &init_mm.mmlist);
		mm = list_entry(p, struct mm_struct, mmlist);

		/* Make sure the mm doesn't disappear when we drop the lock.. */
		atomic_inc(&mm->mm_users);
		spin_unlock(&mmlist_lock);

		/* Walk about 6% of the address space each time */
		retval |= swap_out_mm(mm, swap_amount(mm));
		mmput(mm);
	} while (--counter >= 0);
	return;

empty:
	spin_unlock(&mmlist_lock);
}


/**
 * reclaim_page -	reclaims one page from the inactive_clean list
 * @zone: reclaim a page from this zone
 *
 * The pages on the inactive_clean can be instantly reclaimed.
 * The tests look impressive, but most of the time we'll grab
 * the first page of the list and exit successfully.
 */
struct page * reclaim_page(zone_t * zone)
{
	struct page * page = NULL;
	struct list_head * page_lru;
	int maxscan;

	/*
	 * We only need the pagemap_lru_lock if we don't reclaim the page,
	 * but we have to grab the pagecache_lock before the pagemap_lru_lock
	 * to avoid deadlocks and most of the time we'll succeed anyway.
	 */
	spin_lock(&pagecache_lock);
	spin_lock(&pagemap_lru_lock);
	maxscan = zone->inactive_clean_pages;
	while ((page_lru = zone->inactive_clean_list.prev) !=
			&zone->inactive_clean_list && maxscan--) {
		page = list_entry(page_lru, struct page, lru);

		/* Wrong page on list?! (list corruption, should not happen) */
		if (!PageInactiveClean(page)) {
			printk("VM: reclaim_page, wrong page on list.\n");
			list_del(page_lru);
			page->zone->inactive_clean_pages--;
			continue;
		}

		/* Page is or was in use?  Move it to the active list. */
		if (PageReferenced(page) || page->age > 0 ||
				(!page->buffers && page_count(page) > 1)) {
			del_page_from_inactive_clean_list(page);
			add_page_to_active_list(page);
			continue;
		}

		/* The page is dirty, or locked, move to inactive_dirty list. */
		if (page->buffers || PageDirty(page) || TryLockPage(page)) {
			del_page_from_inactive_clean_list(page);
			add_page_to_inactive_dirty_list(page);
			continue;
		}

		/* OK, remove the page from the caches. */
                if (PageSwapCache(page)) {
			__delete_from_swap_cache(page);
			goto found_page;
		}

		if (page->mapping) {
			__remove_inode_page(page);
			goto found_page;
		}

		/* We should never ever get here. */
		printk(KERN_ERR "VM: reclaim_page, found unknown page\n");
		list_del(page_lru);
		zone->inactive_clean_pages--;
		UnlockPage(page);
	}
	/* Reset page pointer, maybe we encountered an unfreeable page. */
	page = NULL;
	goto out;

found_page:
	memory_pressure++;
	del_page_from_inactive_clean_list(page);
	UnlockPage(page);
	page->age = PAGE_AGE_START;
	if (page_count(page) != 1)
		printk("VM: reclaim_page, found page with count %d!\n",
				page_count(page));
out:
	spin_unlock(&pagemap_lru_lock);
	spin_unlock(&pagecache_lock);
	return page;
}

/**
 * page_launder - clean dirty inactive pages, move to inactive_clean list
 * @gfp_mask: what operations we are allowed to do
 * @sync: are we allowed to do synchronous IO in emergencies ?
 *
 * When this function is called, we are most likely low on free +
 * inactive_clean pages. Since we want to refill those pages as
 * soon as possible, we'll make two loops over the inactive list,
 * one to move the already cleaned pages to the inactive_clean lists
 * and one to (often asynchronously) clean the dirty inactive pages.
 *
 * In situations where kswapd cannot keep up, user processes will
 * end up calling this function. Since the user process needs to
 * have a page before it can continue with its allocation, we'll
 * do synchronous page flushing in that case.
 *
 * This code is heavily inspired by the FreeBSD source code. Thanks
 * go out to Matthew Dillon.
 */
#define MAX_LAUNDER 		(4 * (1 << page_cluster))
#define CAN_DO_FS		(gfp_mask & __GFP_FS)
#define CAN_DO_IO		(gfp_mask & __GFP_IO)
int page_launder(int gfp_mask, int sync)
{
	int launder_loop, maxscan, cleaned_pages, maxlaunder;
	struct list_head * page_lru;
	struct page * page;

	launder_loop = 0;
	maxlaunder = 0;
	cleaned_pages = 0;

dirty_page_rescan:
	spin_lock(&pagemap_lru_lock);
	maxscan = nr_inactive_dirty_pages;
	while ((page_lru = inactive_dirty_list.prev) != &inactive_dirty_list &&
				maxscan-- > 0) {
		page = list_entry(page_lru, struct page, lru);

		/* Wrong page on list?! (list corruption, should not happen) */
		if (!PageInactiveDirty(page)) {
			printk("VM: page_launder, wrong page on list.\n");
			list_del(page_lru);
			nr_inactive_dirty_pages--;
			page->zone->inactive_dirty_pages--;
			continue;
		}

		/* Page is or was in use?  Move it to the active list. */
		if (PageReferenced(page) || page->age > 0 ||
				(!page->buffers && page_count(page) > 1) ||
				page_ramdisk(page)) {
			del_page_from_inactive_dirty_list(page);
			add_page_to_active_list(page);
			continue;
		}

		/*
		 * The page is locked. IO in progress?
		 * Move it to the back of the list.
		 */
		if (TryLockPage(page)) {
			list_del(page_lru);
			list_add(page_lru, &inactive_dirty_list);
			continue;
		}

		/*
		 * Dirty swap-cache page? Write it out if
		 * last copy..
		 */
		if (PageDirty(page)) {
			int (*writepage)(struct page *) = page->mapping->a_ops->writepage;

			if (!writepage)
				goto page_active;

			/* First time through? Move it to the back of the list */
			if (!launder_loop || !CAN_DO_FS) {
				list_del(page_lru);
				list_add(page_lru, &inactive_dirty_list);
				UnlockPage(page);
				continue;
			}

			/* OK, do a physical asynchronous write to swap.  */
			ClearPageDirty(page);
			page_cache_get(page);
			spin_unlock(&pagemap_lru_lock);

			writepage(page);
			page_cache_release(page);

			/* And re-start the thing.. */
			spin_lock(&pagemap_lru_lock);
			continue;
		}

		/*
		 * If the page has buffers, try to free the buffer mappings
		 * associated with this page. If we succeed we either free
		 * the page (in case it was a buffercache only page) or we
		 * move the page to the inactive_clean list.
		 *
		 * On the first round, we should free all previously cleaned
		 * buffer pages
		 */
		if (page->buffers) {
			unsigned int buffer_mask;
			int clearedbuf;
			int freed_page = 0;
			/*
			 * Since we might be doing disk IO, we have to
			 * drop the spinlock and take an extra reference
			 * on the page so it doesn't go away from under us.
			 */
			del_page_from_inactive_dirty_list(page);
			page_cache_get(page);
			spin_unlock(&pagemap_lru_lock);

			/* Will we do (asynchronous) IO? */
			if (launder_loop && maxlaunder == 0 && sync)
				buffer_mask = gfp_mask;				/* Do as much as we can */
			else if (launder_loop && maxlaunder-- > 0)
				buffer_mask = gfp_mask & ~__GFP_WAIT;			/* Don't wait, async write-out */
			else
				buffer_mask = gfp_mask & ~(__GFP_WAIT | __GFP_IO);	/* Don't even start IO */

			/* Try to free the page buffers. */
			clearedbuf = try_to_free_buffers(page, buffer_mask);

			/*
			 * Re-take the spinlock. Note that we cannot
			 * unlock the page yet since we're still
			 * accessing the page_struct here...
			 */
			spin_lock(&pagemap_lru_lock);

			/* The buffers were not freed. */
			if (!clearedbuf) {
				add_page_to_inactive_dirty_list(page);

			/* The page was only in the buffer cache. */
			} else if (!page->mapping) {
				atomic_dec(&buffermem_pages);
				freed_page = 1;
				cleaned_pages++;

			/* The page has more users besides the cache and us. */
			} else if (page_count(page) > 2) {
				add_page_to_active_list(page);

			/* OK, we "created" a freeable page. */
			} else /* page->mapping && page_count(page) == 2 */ {
				add_page_to_inactive_clean_list(page);
				cleaned_pages++;
			}

			/*
			 * Unlock the page and drop the extra reference.
			 * We can only do it here because we are accessing
			 * the page struct above.
			 */
			UnlockPage(page);
			page_cache_release(page);

			/* 
			 * If we're freeing buffer cache pages, stop when
			 * we've got enough free memory.
			 */
			if (freed_page && !free_shortage())
				break;
			continue;
		} else if (page->mapping && !PageDirty(page)) {
			/*
			 * If a page had an extra reference in
			 * deactivate_page(), we will find it here.
			 * Now the page is really freeable, so we
			 * move it to the inactive_clean list.
			 */
			del_page_from_inactive_dirty_list(page);
			add_page_to_inactive_clean_list(page);
			UnlockPage(page);
			cleaned_pages++;
		} else {
page_active:
			/*
			 * OK, we don't know what to do with the page.
			 * It's no use keeping it here, so we move it to
			 * the active list.
			 */
			del_page_from_inactive_dirty_list(page);
			add_page_to_active_list(page);
			UnlockPage(page);
		}
	}
	spin_unlock(&pagemap_lru_lock);

	/*
	 * If we don't have enough free pages, we loop back once
	 * to queue the dirty pages for writeout. When we were called
	 * by a user process (that /needs/ a free page) and we didn't
	 * free anything yet, we wait synchronously on the writeout of
	 * MAX_SYNC_LAUNDER pages.
	 *
	 * We also wake up bdflush, since bdflush should, under most
	 * loads, flush out the dirty pages before we have to wait on
	 * IO.
	 */
	if (CAN_DO_IO && !launder_loop && free_shortage()) {
		launder_loop = 1;
		/* If we cleaned pages, never do synchronous IO. */
		if (cleaned_pages)
			sync = 0;
		/* We only do a few "out of order" flushes. */
		maxlaunder = MAX_LAUNDER;
		/* Kflushd takes care of the rest. */
		wakeup_bdflush(0);
		goto dirty_page_rescan;
	}

	/* Return the number of pages moved to the inactive_clean list. */
	return cleaned_pages;
}

/**
 * refill_inactive_scan - scan the active list and find pages to deactivate
 * @priority: the priority at which to scan
 * @target: number of pages to deactivate, zero for background aging
 *
 * This function will scan a portion of the active list to find
 * unused pages, those pages will then be moved to the inactive list.
 */
int refill_inactive_scan(unsigned int priority, int target)
{
	struct list_head * page_lru;
	struct page * page;
	int maxscan = nr_active_pages >> priority;
	int page_active = 0;
	int nr_deactivated = 0;

	/*
	 * When we are background aging, we try to increase the page aging
	 * information in the system.
	 */
	if (!target)
		maxscan = nr_active_pages >> 4;

	/* Take the lock while messing with the list... */
	spin_lock(&pagemap_lru_lock);
	while (maxscan-- > 0 && (page_lru = active_list.prev) != &active_list) {
		page = list_entry(page_lru, struct page, lru);

		/* Wrong page on list?! (list corruption, should not happen) */
		if (!PageActive(page)) {
			printk("VM: refill_inactive, wrong page on list.\n");
			list_del(page_lru);
			nr_active_pages--;
			continue;
		}

		/* Do aging on the pages. */
		if (PageTestandClearReferenced(page)) {
			age_page_up_nolock(page);
			page_active = 1;
		} else {
			age_page_down_ageonly(page);
			/*
			 * Since we don't hold a reference on the page
			 * ourselves, we have to do our test a bit more
			 * strict then deactivate_page(). This is needed
			 * since otherwise the system could hang shuffling
			 * unfreeable pages from the active list to the
			 * inactive_dirty list and back again...
			 *
			 * SUBTLE: we can have buffer pages with count 1.
			 */
			if (page->age == 0 && page_count(page) <=
						(page->buffers ? 2 : 1)) {
				deactivate_page_nolock(page);
				page_active = 0;
			} else {
				page_active = 1;
			}
		}
		/*
		 * If the page is still on the active list, move it
		 * to the other end of the list. Otherwise we exit if
		 * we have done enough work.
		 */
		if (page_active || PageActive(page)) {
			list_del(page_lru);
			list_add(page_lru, &active_list);
		} else {
			nr_deactivated++;
			if (target && nr_deactivated >= target)
				break;
		}
	}
	spin_unlock(&pagemap_lru_lock);

	return nr_deactivated;
}

/*
 * Check if there are zones with a severe shortage of free pages,
 * or if all zones have a minor shortage.
 */
int free_shortage(void)
{
	pg_data_t *pgdat = pgdat_list;
	int sum = 0;
	int freeable = nr_free_pages() + nr_inactive_clean_pages();
	int freetarget = freepages.high;

	/* Are we low on free pages globally? */
	if (freeable < freetarget)
		return freetarget - freeable;

	/* If not, are we very low on any particular zone? */
	do {
		int i;
		for(i = 0; i < MAX_NR_ZONES; i++) {
			zone_t *zone = pgdat->node_zones+ i;
			if (zone->size && (zone->inactive_clean_pages +
					zone->free_pages < zone->pages_min)) {
				sum += zone->pages_min;
				sum -= zone->free_pages;
				sum -= zone->inactive_clean_pages;
			}
		}
		pgdat = pgdat->node_next;
	} while (pgdat);

	return sum;
}

/*
 * How many inactive pages are we short?
 */
int inactive_shortage(void)
{
	int shortage = 0;
	pg_data_t *pgdat = pgdat_list;

	/* Is the inactive dirty list too small? */

	shortage += freepages.high;
	shortage += inactive_target;
	shortage -= nr_free_pages();
	shortage -= nr_inactive_clean_pages();
	shortage -= nr_inactive_dirty_pages;

	if (shortage > 0)
		return shortage;

	/* If not, do we have enough per-zone pages on the inactive list? */

	shortage = 0;

	do {
		int i;
		for(i = 0; i < MAX_NR_ZONES; i++) {
			int zone_shortage;
			zone_t *zone = pgdat->node_zones+ i;

			if (!zone->size)
				continue;
			zone_shortage = zone->pages_high;
			zone_shortage -= zone->inactive_dirty_pages;
			zone_shortage -= zone->inactive_clean_pages;
			zone_shortage -= zone->free_pages;
			if (zone_shortage > 0)
				shortage += zone_shortage;
		}
		pgdat = pgdat->node_next;
	} while (pgdat);

	return shortage;
}

/*
 * Refill_inactive is the function used to scan and age the pages on
 * the active list and in the working set of processes, moving the
 * little-used pages to the inactive list.
 *
 * When called by kswapd, we try to deactivate as many pages as needed
 * to recover from the inactive page shortage. This makes it possible
 * for kswapd to keep up with memory demand so user processes can get
 * low latency on memory allocations.
 *
 * However, when the system starts to get overloaded we can get called
 * by user processes. For user processes we want to both reduce the
 * latency and make sure that multiple user processes together don't
 * deactivate too many pages. To achieve this we simply do less work
 * when called from a user process.
 */
#define DEF_PRIORITY (6)
static int refill_inactive(unsigned int gfp_mask, int user)
{
	int count, start_count, maxtry;

	if (user) {
		count = (1 << page_cluster);
		maxtry = 6;
	} else {
		count = inactive_shortage();
		maxtry = 1 << DEF_PRIORITY;
	}

	start_count = count;
	do {
		if (current->need_resched) {
			__set_current_state(TASK_RUNNING);
			schedule();
			if (!inactive_shortage())
				return 1;
		}

		/* Walk the VM space for a bit.. */
		swap_out(DEF_PRIORITY, gfp_mask);

		count -= refill_inactive_scan(DEF_PRIORITY, count);
		if (count <= 0)
			goto done;

		if (--maxtry <= 0)
				return 0;
		
	} while (inactive_shortage());

done:
	return (count < start_count);
}

static int do_try_to_free_pages(unsigned int gfp_mask, int user)
{
	int ret = 0;

	/*
	 * If we're low on free pages, move pages from the
	 * inactive_dirty list to the inactive_clean list.
	 *
	 * Usually bdflush will have pre-cleaned the pages
	 * before we get around to moving them to the other
	 * list, so this is a relatively cheap operation.
	 */
	if (free_shortage()) {
		ret += page_launder(gfp_mask, user);
		shrink_dcache_memory(DEF_PRIORITY, gfp_mask);
		shrink_icache_memory(DEF_PRIORITY, gfp_mask);
	}

	/*
	 * If needed, we move pages from the active list
	 * to the inactive list.
	 */
	if (inactive_shortage())
		ret += refill_inactive(gfp_mask, user);

	/* 	
	 * Reclaim unused slab cache if memory is low.
	 */
	kmem_cache_reap(gfp_mask);

	return ret;
}

DECLARE_WAIT_QUEUE_HEAD(kswapd_wait);
DECLARE_WAIT_QUEUE_HEAD(kswapd_done);

/*
 * The background pageout daemon, started as a kernel thread
 * from the init process. 
 *
 * This basically trickles out pages so that we have _some_
 * free memory available even if there is no other activity
 * that frees anything up. This is needed for things like routing
 * etc, where we otherwise might have all activity going on in
 * asynchronous contexts that cannot page things out.
 *
 * If there are applications that are active memory-allocators
 * (most normal use), this basically shouldn't matter.
 */
int kswapd(void *unused)
{
	struct task_struct *tsk = current;

	daemonize();
	strcpy(tsk->comm, "kswapd");
	sigfillset(&tsk->blocked);
	
	/*
	 * Tell the memory management that we're a "memory allocator",
	 * and that if we need more memory we should get access to it
	 * regardless (see "__alloc_pages()"). "kswapd" should
	 * never get caught in the normal page freeing logic.
	 *
	 * (Kswapd normally doesn't need memory anyway, but sometimes
	 * you need a small amount of memory in order to be able to
	 * page out something else, and this flag essentially protects
	 * us from recursively trying to free more memory as we're
	 * trying to free the first piece of memory in the first place).
	 */
	tsk->flags |= PF_MEMALLOC;

	/*
	 * Kswapd main loop.
	 */
	for (;;) {
		static long recalc = 0;

		/* If needed, try to free some memory. */
		if (inactive_shortage() || free_shortage()) 
			do_try_to_free_pages(GFP_KSWAPD, 0);

		/* Once a second ... */
		if (time_after(jiffies, recalc + HZ)) {
			recalc = jiffies;

			/* Recalculate VM statistics. */
			recalculate_vm_stats();

			/* Do background page aging. */
			refill_inactive_scan(DEF_PRIORITY, 0);
		}

		run_task_queue(&tq_disk);

		/* 
		 * We go to sleep if either the free page shortage
		 * or the inactive page shortage is gone. We do this
		 * because:
		 * 1) we need no more free pages   or
		 * 2) the inactive pages need to be flushed to disk,
		 *    it wouldn't help to eat CPU time now ...
		 *
		 * We go to sleep for one second, but if it's needed
		 * we'll be woken up earlier...
		 */
		if (!free_shortage() || !inactive_shortage()) {
			interruptible_sleep_on_timeout(&kswapd_wait, HZ);
		/*
		 * If we couldn't free enough memory, we see if it was
		 * due to the system just not having enough memory.
		 * If that is the case, the only solution is to kill
		 * a process (the alternative is enternal deadlock).
		 *
		 * If there still is enough memory around, we just loop
		 * and try free some more memory...
		 */
		} else if (out_of_memory()) {
			oom_kill();
		}
	}
}

void wakeup_kswapd(void)
{
	if (waitqueue_active(&kswapd_wait))
		wake_up_interruptible(&kswapd_wait);
}

/*
 * Called by non-kswapd processes when they want more
 * memory but are unable to sleep on kswapd because
 * they might be holding some IO locks ...
 */
int try_to_free_pages(unsigned int gfp_mask)
{
	int ret = 1;

	if (gfp_mask & __GFP_WAIT) {
		current->flags |= PF_MEMALLOC;
		ret = do_try_to_free_pages(gfp_mask, 1);
		current->flags &= ~PF_MEMALLOC;
	}

	return ret;
}

DECLARE_WAIT_QUEUE_HEAD(kreclaimd_wait);
/*
 * Kreclaimd will move pages from the inactive_clean list to the
 * free list, in order to keep atomic allocations possible under
 * all circumstances.
 */
int kreclaimd(void *unused)
{
	struct task_struct *tsk = current;
	pg_data_t *pgdat;

	daemonize();
	strcpy(tsk->comm, "kreclaimd");
	sigfillset(&tsk->blocked);
	current->flags |= PF_MEMALLOC;

	while (1) {

		/*
		 * We sleep until someone wakes us up from
		 * page_alloc.c::__alloc_pages().
		 */
		interruptible_sleep_on(&kreclaimd_wait);

		/*
		 * Move some pages from the inactive_clean lists to
		 * the free lists, if it is needed.
		 */
		pgdat = pgdat_list;
		do {
			int i;
			for(i = 0; i < MAX_NR_ZONES; i++) {
				zone_t *zone = pgdat->node_zones + i;
				if (!zone->size)
					continue;

				while (zone->free_pages < zone->pages_low) {
					struct page * page;
					page = reclaim_page(zone);
					if (!page)
						break;
					__free_page(page);
				}
			}
			pgdat = pgdat->node_next;
		} while (pgdat);
	}
}


static int __init kswapd_init(void)
{
	printk("Starting kswapd v1.8\n");
	swap_setup();
	kernel_thread(kswapd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
	kernel_thread(kreclaimd, NULL, CLONE_FS | CLONE_FILES | CLONE_SIGNAL);
	return 0;
}

module_init(kswapd_init)
