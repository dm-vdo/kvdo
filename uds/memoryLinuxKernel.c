/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/memoryLinuxKernel.c#4 $
 */

#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>     // needed for SQUEEZE builds
#include <linux/vmalloc.h>
#include <linux/version.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"

/*
 * XXX - TODO - name unifications with VDO
 */

static ThreadRegistry allocatingThreads;

// We allocate very few large objects, and allocation/deallocation isn't done
// in a performance-critical stage for us, so a linked list should be fine.
typedef struct vmallocBlockInfo {
  void                    *ptr;
  size_t                   size;
  struct vmallocBlockInfo *next;
} VmallocBlockInfo;

static struct {
  spinlock_t        lock;
  size_t            kmallocBlocks;
  size_t            kmallocBytes;
  size_t            vmallocBlocks;
  size_t            vmallocBytes;
  size_t            peakBytes;
  size_t            bioCount;
  size_t            peakBioCount;
  VmallocBlockInfo *vmallocList;
} memoryStats __cacheline_aligned;

/*
 * We do not use kernel functions like the kvasprintf() method, which allocate
 * memory indirectly using kmalloc.
 */

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
 * The advantages of kmalloc do not help out Albireo, because we allocate all
 * our memory up front and do not free and reallocate it.  Sometimes we have
 * problems using kmalloc, because the Linux memory page map can become so
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
static INLINE bool useKmalloc(size_t size)
{
  return size <= PAGE_SIZE;
}

/*****************************************************************************/
static bool allocationsAllowed(void)
{
  const bool *pointer = lookupThread(&allocatingThreads);
  return pointer != NULL ? *pointer : false;
}

/*****************************************************************************/
void registerAllocatingThread(RegisteredThread *newThread, const bool *flagPtr)
{
  if (flagPtr == NULL) {
    static const bool allocationAlwaysAllowed = true;
    flagPtr = &allocationAlwaysAllowed;
  }
  registerThread(&allocatingThreads, newThread, flagPtr);
}

/*****************************************************************************/
void unregisterAllocatingThread(void)
{
  unregisterThread(&allocatingThreads);
}

/*****************************************************************************/
static void updatePeakUsage(void)
{
  size_t totalBytes = memoryStats.kmallocBytes + memoryStats.vmallocBytes;
  if (totalBytes > memoryStats.peakBytes) {
    memoryStats.peakBytes = totalBytes;
  }
}

/*****************************************************************************/
static void addKmallocBlock(size_t size)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  memoryStats.kmallocBlocks++;
  memoryStats.kmallocBytes += size;
  updatePeakUsage();
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/*****************************************************************************/
static void removeKmallocBlock(size_t size)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  memoryStats.kmallocBlocks--;
  memoryStats.kmallocBytes -= size;
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/*****************************************************************************/
static void addVmallocBlock(VmallocBlockInfo *block)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  block->next = memoryStats.vmallocList;
  memoryStats.vmallocList = block;
  memoryStats.vmallocBlocks++;
  memoryStats.vmallocBytes += block->size;
  updatePeakUsage();
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/*****************************************************************************/
static void removeVmallocBlock(void *ptr)
{
  VmallocBlockInfo *block, **blockPtr;
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  for (blockPtr = &memoryStats.vmallocList;
       (block = *blockPtr) != NULL;
       blockPtr = &block->next) {
    if (block->ptr == ptr) {
      *blockPtr = block->next;
      memoryStats.vmallocBlocks--;
      memoryStats.vmallocBytes -= block->size;
      break;
    }
  }
  spin_unlock_irqrestore(&memoryStats.lock, flags);
  if (block != NULL) {
    FREE(block);
  } else {
    logInfo("attempting to remove ptr %p not found in vmalloc list", ptr);
  }
}

/*****************************************************************************/
int allocateMemory(size_t size, size_t align, const char *what, void *ptr)
{
  if (ptr == NULL) {
    return UDS_INVALID_ARGUMENT;
  }

  /*
   * The names of the NORETRY and REPEAT flags are misleading.
   *
   * The Linux source code says that REPEAT flag means "Try hard to allocate
   * the memory, but the allocation attempt _might_ fail".  And that this
   * depends upon the particular VM implementation.  We want to repeat trying
   * to allocate the memory, because for large allocations the page reclamation
   * code needs time to free up the memory that we need.
   *
   * The Linux source code says that NORETRY flag means "The VM implementation
   * must not retry indefinitely".  In practise, the Linux allocator will loop
   * trying to perform the allocation, but this flag suppresses extreme
   * measures to satisfy the request.  The extreme measure that we want to
   * avoid is the invocation of the OOM killer.
   */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,13,0)
  gfp_t gfpFlags = __GFP_NORETRY | __GFP_RETRY_MAYFAIL | __GFP_ZERO;
#else
  gfp_t gfpFlags = __GFP_NORETRY | __GFP_REPEAT | __GFP_ZERO;
#endif
  if (allocationsAllowed()) {
    /*
     * Setup or teardown, when the VDO device isn't active, or a non-critical
     * path thread.  It's okay for this allocation to block, and even block
     * waiting for certain selected pages to be written out through the VDO
     * device.
     */
    gfpFlags |= GFP_KERNEL;
  } else {
    /*
     * Device is configured and running.  We can't afford to block on writing a
     * page through VDO!
     *
     * We strongly discourage allocations at this stage, but there are a small
     * number of necessary allocations.  In particular, an intMap may need to
     * allocate memory.
     */
    gfpFlags |= GFP_NOIO;
  }

  unsigned long startTime = jiffies;
  void *p = NULL;
  if (size > 0) {
    if (useKmalloc(size) && (align < PAGE_SIZE)) {
      p = kmalloc(size, gfpFlags | __GFP_NOWARN);
      if (p == NULL) {
        /*
         * If we had just done kmalloc(size, gfpFlags) it is possible that the
         * allocation would fail (see VDO-3688).  The kernel log would then
         * contain a long report about the failure.  Although the failure
         * occurs because there is no page available to allocate, by the time
         * it logs the available space, there is a page available.  So
         * hopefully a short sleep will allow the page reclaimer to free a
         * single page, which is all that we need.
         */
        msleep(1);
        p = kmalloc(size, gfpFlags);
      }
      if (p != NULL) {
        addKmallocBlock(ksize(p));
      }
    } else {
      VmallocBlockInfo *block;
      if (ALLOCATE(1, VmallocBlockInfo, __func__, &block) == UDS_SUCCESS) {
        /*
         * If we just do __vmalloc(size, gfpFlags, PAGE_KERNEL) it is possible
         * that the allocation will fail (see VDO-3661).  The kernel log will
         * then contain a long report about the failure.  Although the failure
         * occurs because there are not enough pages available to allocate, by
         * the time it logs the available space, there may enough pages
         * available for smaller allocations.  So hopefully a short sleep will
         * allow the page reclaimer to free enough pages for us.
         *
         * For larger allocations, the kernel page_alloc code is racing against
         * the page reclaimer.  If the page reclaimer can stay ahead of
         * page_alloc, the __vmalloc will succeed.  But if page_alloc overtakes
         * the page reclaimer, the allocation fails.  It is possible that more
         * retries will succeed.
         */
        for (;;) {
          p = __vmalloc(size, gfpFlags | __GFP_NOWARN, PAGE_KERNEL);
          // Try again unless we succeeded or more than 1 second has elapsed.
          if ((p != NULL) || (jiffies_to_msecs(jiffies - startTime) > 1000)) {
            break;
          }
          msleep(1);
        }
        if (p == NULL) {
          // Try one more time, logging a failure for this call.
          p = __vmalloc(size, gfpFlags, PAGE_KERNEL);
        }
        if (p == NULL) {
          FREE(block);
        } else {
          block->ptr = p;
          block->size = PAGE_ALIGN(size);
          addVmallocBlock(block);
        }
      }
    }
    if (p == NULL) {
      unsigned int duration = jiffies_to_msecs(jiffies - startTime);
      logError("Could not allocate %lu bytes for %s in %u msecs",
               size, what, duration);
      return ENOMEM;
    }
  }
  *((void **) ptr) = p;
  return UDS_SUCCESS;
}

/*****************************************************************************/
void freeMemory(void *ptr)
{
  if (ptr != NULL) {
    if (is_vmalloc_addr(ptr)) {
      removeVmallocBlock(ptr);
      vfree(ptr);
    } else {
      removeKmallocBlock(ksize(ptr));
      kfree(ptr);
    }
  }
}

/*****************************************************************************/
int doPlatformVasprintf(char **strp, const char *fmt, va_list ap)
{
  va_list args;
  va_copy(args, ap);
  int count = vsnprintf(NULL, 0, fmt, args) + 1;
  va_end(args);
  int result = ALLOCATE(count, char, NULL, strp);
  if (result != UDS_SUCCESS) {
    return result;
  }
  vsnprintf(*strp, count, fmt, ap);
  return UDS_SUCCESS;
}

/*****************************************************************************/
int reallocateMemory(void       *ptr,
                     size_t      oldSize,
                     size_t      size,
                     const char *what,
                     void       *newPtr)
{
  // Handle special case of zero sized result
  if (size == 0) {
    FREE(ptr);
    *(void **)newPtr = NULL;
    return UDS_SUCCESS;
  }

  int result = ALLOCATE(size, char, what, newPtr);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (ptr != NULL) {
    if (oldSize < size) {
      size = oldSize;
    }
    memcpy(*((void **) newPtr), ptr, size);
    FREE(ptr);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
void memoryInit(void)
{
  spin_lock_init(&memoryStats.lock);
  initializeThreadRegistry(&allocatingThreads);
}


/*****************************************************************************/
void memoryExit(void)
{
  ASSERT_LOG_ONLY(memoryStats.kmallocBytes == 0,
                  "kmalloc memory used (%zd bytes in %zd blocks)"
                  " is returned to the kernel",
                  memoryStats.kmallocBytes, memoryStats.kmallocBlocks);
  ASSERT_LOG_ONLY(memoryStats.vmallocBytes == 0,
                  "vmalloc memory used (%zd bytes in %zd blocks)"
                  " is returned to the kernel",
                  memoryStats.vmallocBytes, memoryStats.vmallocBlocks);
  logDebug("%s peak usage %zd bytes", THIS_MODULE->name,
           memoryStats.peakBytes);

  ASSERT_LOG_ONLY(memoryStats.bioCount == 0,
                  "bio structures used (%zd) are returned to the kernel",
                  memoryStats.bioCount);
  logDebug("%s peak usage %zd bio structures", THIS_MODULE->name,
           memoryStats.peakBioCount);
}

/**********************************************************************/
void recordBioAlloc(void)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  memoryStats.bioCount++;
  if (memoryStats.bioCount > memoryStats.peakBioCount) {
    memoryStats.peakBioCount = memoryStats.bioCount;
  }
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/**********************************************************************/
void recordBioFree(void)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  memoryStats.bioCount--;
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/**********************************************************************/
void getMemoryStats(uint64_t *bytesUsed,
                    uint64_t *peakBytesUsed,
                    uint64_t *biosUsed,
                    uint64_t *peakBioCount)
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  *bytesUsed     = memoryStats.kmallocBytes + memoryStats.vmallocBytes;
  *peakBytesUsed = memoryStats.peakBytes;
  *biosUsed      = memoryStats.bioCount;
  *peakBioCount  = memoryStats.peakBioCount;
  spin_unlock_irqrestore(&memoryStats.lock, flags);
}

/**********************************************************************/
void reportMemoryUsage()
{
  unsigned long flags;
  spin_lock_irqsave(&memoryStats.lock, flags);
  uint64_t kmallocBlocks = memoryStats.kmallocBlocks;
  uint64_t kmallocBytes = memoryStats.kmallocBytes;
  uint64_t vmallocBlocks = memoryStats.vmallocBlocks;
  uint64_t vmallocBytes = memoryStats.vmallocBytes;
  uint64_t peakUsage = memoryStats.peakBytes;
  uint64_t bioCount = memoryStats.bioCount;
  uint64_t peakBioCount = memoryStats.peakBioCount;
  spin_unlock_irqrestore(&memoryStats.lock, flags);
  uint64_t totalBytes = kmallocBytes + vmallocBytes;
  logInfo("current module memory tracking"
          " (actual allocation sizes, not requested):");
  logInfo("  %" PRIu64 " bytes in %" PRIu64 " kmalloc blocks",
          kmallocBytes, kmallocBlocks);
  logInfo("  %" PRIu64 " bytes in %" PRIu64 " vmalloc blocks",
          vmallocBytes, vmallocBlocks);
  logInfo("  total %" PRIu64 " bytes, peak usage %" PRIu64 " bytes",
          totalBytes, peakUsage);
  // Someday maybe we could track the size of allocated bios too.
  logInfo("  %" PRIu64 " bio structs, peak %" PRIu64,
          bioCount, peakBioCount);
}

