/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/memoryLinuxKernel.c#7 $
 */

#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/vmalloc.h>

#include "compiler.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"


/*
 ******************************************************************************
 * Production: UDS and VDO keep track of which threads are allowed to allocate
 * memory freely, and which threads must be careful to not do a memory
 * allocation that does an I/O request.  The allocating_threads ThreadsRegistry
 * and its associated methods implement this tracking.
 */

static struct thread_registry allocating_threads;

/*****************************************************************************/
static bool allocations_allowed(void)
{
	const bool *pointer = lookup_thread(&allocating_threads);
	return pointer != NULL ? *pointer : false;
}

/*****************************************************************************/
void register_allocating_thread(struct registered_thread *new_thread,
				const bool *flag_ptr)
{
	if (flag_ptr == NULL) {
		static const bool allocation_always_allowed = true;
		flag_ptr = &allocation_always_allowed;
	}
	register_thread(&allocating_threads, new_thread, flag_ptr);
}

/*****************************************************************************/
void unregister_allocating_thread(void)
{
	unregister_thread(&allocating_threads);
}

/*
 ******************************************************************************
 * Production: We track how much memory has been allocated and freed.  When we
 * unload the UDS module, we log an error if we have not freed all the memory
 * that we allocated.  Nearly all memory allocation and freeing is done using
 * this module.
 *
 * We do not use kernel functions like the kvasprintf() method, which allocate
 * memory indirectly using kmalloc.
 *
 * These data structures and methods are used to track the amount of memory
 * used.
 */

// We allocate very few large objects, and allocation/deallocation isn't done
// in a performance-critical stage for us, so a linked list should be fine.
struct vmalloc_block_info {
	void *ptr;
	size_t size;
	struct vmalloc_block_info *next;
};

static struct {
	spinlock_t lock;
	size_t kmalloc_blocks;
	size_t kmalloc_bytes;
	size_t vmalloc_blocks;
	size_t vmalloc_bytes;
	size_t peak_bytes;
	struct vmalloc_block_info *vmalloc_list;
} memory_stats __cacheline_aligned;

/*****************************************************************************/
static void update_peak_usage(void)
{
	size_t total_bytes =
		memory_stats.kmalloc_bytes + memory_stats.vmalloc_bytes;
	if (total_bytes > memory_stats.peak_bytes) {
		memory_stats.peak_bytes = total_bytes;
	}
}

/*****************************************************************************/
static void add_kmalloc_block(size_t size)
{
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	memory_stats.kmalloc_blocks++;
	memory_stats.kmalloc_bytes += size;
	update_peak_usage();
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

/*****************************************************************************/
static void remove_kmalloc_block(size_t size)
{
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	memory_stats.kmalloc_blocks--;
	memory_stats.kmalloc_bytes -= size;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

/*****************************************************************************/
static void add_vmalloc_block(struct vmalloc_block_info *block)
{
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	block->next = memory_stats.vmalloc_list;
	memory_stats.vmalloc_list = block;
	memory_stats.vmalloc_blocks++;
	memory_stats.vmalloc_bytes += block->size;
	update_peak_usage();
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

/*****************************************************************************/
static void remove_vmalloc_block(void *ptr)
{
	struct vmalloc_block_info *block, **block_ptr;
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	for (block_ptr = &memory_stats.vmalloc_list;
	     (block = *block_ptr) != NULL;
	     block_ptr = &block->next) {
		if (block->ptr == ptr) {
			*block_ptr = block->next;
			memory_stats.vmalloc_blocks--;
			memory_stats.vmalloc_bytes -= block->size;
			break;
		}
	}
	spin_unlock_irqrestore(&memory_stats.lock, flags);
	if (block != NULL) {
		FREE(block);
	} else {
		logInfo("attempting to remove ptr %" PRIptr " not found in vmalloc list",
			ptr);
	}
}



/**
 * Determine whether allocating a memory block should use kmalloc or vmalloc.
 *
 * vmalloc can allocate any integral number of pages.
 *
 * kmalloc can allocate any number of bytes up to a configured limit, which
 * defaults to 8 megabytes on some of our systems.  kmalloc is especially good
 * when memory is being both allocated and freed, and it does this efficiently
 * in a multi CPU environment.
 *
 * kmalloc usually rounds the size of the block up to the next power of two.
 * So when the requested block is bigger than PAGE_SIZE / 2 bytes, kmalloc will
 * never give you less space than the corresponding vmalloc allocation.
 * Sometimes vmalloc will use less overhead than kmalloc.
 *
 * The advantages of kmalloc do not help out UDS or VDO, because we allocate
 * all our memory up front and do not free and reallocate it.  Sometimes we
 * have problems using kmalloc, because the Linux memory page map can become so
 * fragmented that kmalloc will not give us a 32KB chunk.  We have used vmalloc
 * as a backup to kmalloc in the past, and a followup vmalloc of 32KB will
 * work.  But there is no strong case to be made for using kmalloc over vmalloc
 * for these size chunks.
 *
 * The kmalloc/vmalloc boundary is set at 4KB, and kmalloc gets the 4KB
 * requests.  There is no strong reason for favoring either kmalloc or vmalloc
 * for 4KB requests, except that the keeping of vmalloc statistics uses a
 * linked list implementation.  Using a simple test, this choice of boundary
 * results in 132 vmalloc calls.  Using vmalloc for requests of exactly 4KB
 * results in an additional 6374 vmalloc calls, which will require a change to
 * the code that tracks vmalloc statistics.
 *
 * @param size  How many bytes to allocate
 **/
static INLINE bool use_kmalloc(size_t size)
{
	return size <= PAGE_SIZE;
}

/*****************************************************************************/
int allocate_memory(size_t size, size_t align, const char *what, void *ptr)
{
	if (ptr == NULL) {
		return UDS_INVALID_ARGUMENT;
	}
	if (size == 0) {
		*((void **) ptr) = NULL;
		return UDS_SUCCESS;
	}


	/*
	 * The __GFP_RETRY_MAYFAIL means: The VM implementation will retry
	 * memory reclaim procedures that have previously failed if there is
	 * some indication that progress has been made else where.  It can wait
	 * for other tasks to attempt high level approaches to freeing memory
	 * such as compaction (which removes fragmentation) and page-out. There
	 * is still a definite limit to the number of retries, but it is a
	 * larger limit than with __GFP_NORETRY. Allocations with this flag may
	 * fail, but only when there is genuinely little unused memory. While
	 * these allocations do not directly trigger the OOM killer, their
	 * failure indicates that the system is likely to need to use the OOM
	 * killer soon.  The caller must handle failure, but can reasonably do
	 * so by failing a higher-level request, or completing it only in a
	 * much less efficient manner.
	 */
	const gfp_t gfp_flags = GFP_KERNEL | __GFP_ZERO | __GFP_RETRY_MAYFAIL;

	bool allocations_restricted = !allocations_allowed();
	unsigned int noio_flags;
	if (allocations_restricted) {
		noio_flags = memalloc_noio_save();
	}

	unsigned long start_time = jiffies;
	void *p = NULL;
	if (use_kmalloc(size) && (align < PAGE_SIZE)) {
		p = kmalloc(size, gfp_flags | __GFP_NOWARN);
		if (p == NULL) {
			/*
			 * If we had just done kmalloc(size, gfp_flags) it is
			 * possible that the allocation would fail (see
			 * VDO-3688).  The kernel log would then contain a long
			 * report about the failure.  Although the failure
			 * occurs because there is no page available to
			 * allocate, by the time it logs the available space,
			 * there is a page available.  So hopefully a short
			 * sleep will allow the page reclaimer to free a single
			 * page, which is all that we need.
			 */
			msleep(1);
			p = kmalloc(size, gfp_flags);
		}
		if (p != NULL) {
			add_kmalloc_block(ksize(p));
		}
	} else {
		struct vmalloc_block_info *block;
		if (ALLOCATE(1, struct vmalloc_block_info, __func__, &block) ==
		    UDS_SUCCESS) {
			/*
			 * If we just do __vmalloc(size, gfp_flags,
			 * PAGE_KERNEL) it is possible that the allocation will
			 * fail (see VDO-3661).  The kernel log will then
			 * contain a long report about the failure.  Although
			 * the failure occurs because there are not enough
			 * pages available to allocate, by the time it logs the
			 * available space, there may enough pages available
			 * for smaller allocations.  So hopefully a short sleep
			 * will allow the page reclaimer to free enough pages
			 * for us.
			 *
			 * For larger allocations, the kernel page_alloc code
			 * is racing against the page reclaimer.  If the page
			 * reclaimer can stay ahead of page_alloc, the
			 * __vmalloc will succeed.  But if page_alloc overtakes
			 * the page reclaimer, the allocation fails.  It is
			 * possible that more retries will succeed.
			 */
			for (;;) {
// XXX Take out when all Fedora lab machines have upgraded to 5.8+. ALB-3032.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
				p = __vmalloc(size, gfp_flags | __GFP_NOWARN);
#else
				p = __vmalloc(size,
					      gfp_flags | __GFP_NOWARN,
					      PAGE_KERNEL);
#endif
				// Try again unless we succeeded or more than 1
				// second has elapsed.
				if ((p != NULL) ||
				    (jiffies_to_msecs(jiffies - start_time) >
				     1000)) {
					break;
				}
				msleep(1);
			}
			if (p == NULL) {
				// Try one more time, logging a failure for
				// this call.
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
				p = __vmalloc(size, gfp_flags);
#else
				p = __vmalloc(size, gfp_flags, PAGE_KERNEL);
#endif
			}
			if (p == NULL) {
				FREE(block);
			} else {
				block->ptr = p;
				block->size = PAGE_ALIGN(size);
				add_vmalloc_block(block);
			}
		}
	}

	if (allocations_restricted) {
		memalloc_noio_restore(noio_flags);
	}

	if (p == NULL) {
		unsigned int duration = jiffies_to_msecs(jiffies - start_time);
		logError("Could not allocate %zu bytes for %s in %u msecs",
			 size,
			 what,
			 duration);
		return ENOMEM;
	}
	*((void **) ptr) = p;
	return UDS_SUCCESS;
}

/*****************************************************************************/
void *allocate_memory_nowait(size_t size,
			     const char *what __attribute__((unused)))
{
	void *p = kmalloc(size, GFP_NOWAIT | __GFP_ZERO);
	if (p != NULL) {
		add_kmalloc_block(ksize(p));
	}
	return p;
}

/*****************************************************************************/
void free_memory(void *ptr)
{
	if (ptr != NULL) {
		if (is_vmalloc_addr(ptr)) {
			remove_vmalloc_block(ptr);
			vfree(ptr);
		} else {
			remove_kmalloc_block(ksize(ptr));
			kfree(ptr);
		}
	}
}

/*****************************************************************************/
int reallocate_memory(void *ptr,
		      size_t old_size,
		      size_t size,
		      const char *what,
		      void *new_ptr)
{
	// Handle special case of zero sized result
	if (size == 0) {
		FREE(ptr);
		*(void **) new_ptr = NULL;
		return UDS_SUCCESS;
	}

	int result = ALLOCATE(size, char, what, new_ptr);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (ptr != NULL) {
		if (old_size < size) {
			size = old_size;
		}
		memcpy(*((void **) new_ptr), ptr, size);
		FREE(ptr);
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
void memory_init(void)
{

	spin_lock_init(&memory_stats.lock);
	initialize_thread_registry(&allocating_threads);
}

/*****************************************************************************/
void memory_exit(void)
{

	ASSERT_LOG_ONLY(memory_stats.kmalloc_bytes == 0,
			"kmalloc memory used (%zd bytes in %zd blocks) is returned to the kernel",
			memory_stats.kmalloc_bytes,
			memory_stats.kmalloc_blocks);
	ASSERT_LOG_ONLY(memory_stats.vmalloc_bytes == 0,
			"vmalloc memory used (%zd bytes in %zd blocks) is returned to the kernel",
			memory_stats.vmalloc_bytes,
			memory_stats.vmalloc_blocks);
	logDebug("%s peak usage %zd bytes",
		 THIS_MODULE->name,
		 memory_stats.peak_bytes);
}

/**********************************************************************/
void get_memory_stats(uint64_t *bytes_used, uint64_t *peak_bytes_used)
{
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	*bytes_used = memory_stats.kmalloc_bytes + memory_stats.vmalloc_bytes;
	*peak_bytes_used = memory_stats.peak_bytes;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
}

/**********************************************************************/
void report_memory_usage()
{
	unsigned long flags;
	spin_lock_irqsave(&memory_stats.lock, flags);
	uint64_t kmalloc_blocks = memory_stats.kmalloc_blocks;
	uint64_t kmalloc_bytes = memory_stats.kmalloc_bytes;
	uint64_t vmalloc_blocks = memory_stats.vmalloc_blocks;
	uint64_t vmalloc_bytes = memory_stats.vmalloc_bytes;
	uint64_t peak_usage = memory_stats.peak_bytes;
	spin_unlock_irqrestore(&memory_stats.lock, flags);
	uint64_t total_bytes = kmalloc_bytes + vmalloc_bytes;
	logInfo("current module memory tracking (actual allocation sizes, not requested):");
	logInfo("  %llu bytes in %llu kmalloc blocks",
		kmalloc_bytes,
		kmalloc_blocks);
	logInfo("  %llu bytes in %llu vmalloc blocks",
		vmalloc_bytes,
		vmalloc_blocks);
	logInfo("  total %llu bytes, peak usage %llu bytes",
		total_bytes,
		peak_usage);
}
