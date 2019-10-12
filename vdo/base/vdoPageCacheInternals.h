/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoPageCacheInternals.h#9 $
 */

#ifndef VDO_PAGE_CACHE_INTERNALS_H
#define VDO_PAGE_CACHE_INTERNALS_H

#include "vdoPageCache.h"


#include "blockMapInternals.h"
#include "completion.h"
#include "dirtyLists.h"
#include "intMap.h"
#include "physicalLayer.h"
#include "ringNode.h"

enum {
  MAX_PAGE_CONTEXT_SIZE = 8,
};

static const PhysicalBlockNumber NO_PAGE = 0xFFFFFFFFFFFFFFFF;

/**
 * A PageInfoNode is a ring node.
 **/
typedef RingNode PageInfoNode;

/**
 * The VDO Page Cache abstraction.
 **/
struct vdo_page_cache {
  /** the physical layer to page to */
  PhysicalLayer                       *layer;
  /** number of pages in cache */
  PageCount                            pageCount;
  /** function to call on page read */
  VDOPageReadFunction                 *readHook;
  /** function to call on page write */
  VDOPageWriteFunction                *writeHook;
  /** number of pages to write in the current batch */
  PageCount                            pagesInBatch;
  /** Whether the VDO is doing a read-only rebuild */
  bool                                 rebuilding;

  /** array of page information entries */
  struct page_info                    *infos;
  /** raw memory for pages */
  char                                *pages;
  /** cache last found page info */
  struct page_info                    *lastFound;
  /** map of page number to info */
  struct int_map                      *pageMap;
  /** master LRU list (all infos) */
  PageInfoNode                         lruList;
  /** dirty pages by period */
  struct dirty_lists                  *dirtyLists;
  /** free page list (oldest first) */
  PageInfoNode                         freeList;
  /** outgoing page list */
  PageInfoNode                         outgoingList;
  /** number of read I/O operations pending */
  PageCount                            outstandingReads;
  /** number of write I/O operations pending */
  PageCount                            outstandingWrites;
  /** number of pages covered by the current flush */
  PageCount                            pagesInFlush;
  /** number of pages waiting to be included in the next flush */
  PageCount                            pagesToFlush;
  /** number of discards in progress */
  unsigned int                         discardCount;
  /** how many VPCs waiting for free page */
  unsigned int                         waiterCount;
  /** queue of waiters who want a free page */
  struct wait_queue                    freeWaiters;
  /** statistics */
  struct atomic_page_cache_statistics  stats;
  /** counter for pressure reports */
  uint32_t                             pressureReport;
  /** the block map zone to which this cache belongs */
  BlockMapZone                        *zone;
};

/**
 * The state of a page buffer. If the page buffer is free no particular page is
 * bound to it, otherwise the page buffer is bound to particular page whose
 * absolute pbn is in the pbn field. If the page is resident or dirty the page
 * data is stable and may be accessed. Otherwise the page is in flight
 * (incoming or outgoing) and its data should not be accessed.
 *
 * @note Update the static data in vpcPageStateName() and vpcPageStateFlag()
 *       if you change this enumeration.
 **/
typedef enum __attribute__((packed)) pageState {
  /* this page buffer is not being used */
  PS_FREE,
  /* this page is being read from store */
  PS_INCOMING,
  /* attempt to load this page failed */
  PS_FAILED,
  /* this page is valid and un-modified */
  PS_RESIDENT,
  /* this page is valid and modified */
  PS_DIRTY,
  /* this page is being written and should not be used */
  PS_OUTGOING,
  /* not a state */
  PAGE_STATE_COUNT,
} PageState;

/**
 * The write status of page
 **/
typedef enum __attribute__((packed)) {
  WRITE_STATUS_NORMAL,
  WRITE_STATUS_DISCARD,
  WRITE_STATUS_DEFERRED,
} WriteStatus;

/**
 * Per-page-slot information.
 **/
struct page_info {
  /** Preallocated page VIO */
  VIO                   *vio;
  /** back-link for references */
  struct vdo_page_cache *cache;
  /** the pbn of the page */
  PhysicalBlockNumber    pbn;
  /** page is busy (temporarily locked) */
  uint16_t               busy;
  /** the write status the page */
  WriteStatus            writeStatus;
  /** page state */
  PageState              state;
  /** queue of completions awaiting this item */
  struct wait_queue      waiting;
  /** state linked list node */
  PageInfoNode           listNode;
  /** LRU node */
  PageInfoNode           lruNode;
  /** Space for per-page client data */
  byte                   context[MAX_PAGE_CONTEXT_SIZE];
};

// PAGE INFO LIST OPERATIONS

/**********************************************************************/
static inline struct page_info *pageInfoFromListNode(PageInfoNode *node)
{
  if (node == NULL) {
    return NULL;
  }
  return (struct page_info *)
    ((uintptr_t) node - offsetof(struct page_info, listNode));
}

/**********************************************************************/
static inline struct page_info *pageInfoFromLRUNode(PageInfoNode *node)
{
  if (node == NULL) {
    return NULL;
  }
  return (struct page_info *)
    ((uintptr_t) node - offsetof(struct page_info, lruNode));
}

// PAGE INFO STATE ACCESSOR FUNCTIONS

/**********************************************************************/
static inline bool isFree(const struct page_info *info)
{
  return info->state == PS_FREE;
}

/**********************************************************************/
static inline bool isAvailable(const struct page_info *info)
{
  return (info->state == PS_FREE) || (info->state == PS_FAILED);
}

/**********************************************************************/
static inline bool isPresent(const struct page_info *info)
{
  return (info->state == PS_RESIDENT) || (info->state == PS_DIRTY);
}

/**********************************************************************/
static inline bool isDirty(const struct page_info *info)
{
  return info->state == PS_DIRTY;
}

/**********************************************************************/
static inline bool isResident(const struct page_info *info)
{
  return info->state == PS_RESIDENT;
}

/**********************************************************************/
static inline bool isInFlight(const struct page_info *info)
{
  return (info->state == PS_INCOMING) || (info->state == PS_OUTGOING);
}

/**********************************************************************/
static inline bool isIncoming(const struct page_info *info)
{
  return info->state == PS_INCOMING;
}

/**********************************************************************/
static inline bool isOutgoing(const struct page_info *info)
{
  return info->state == PS_OUTGOING;
}

/**********************************************************************/
static inline bool isValid(const struct page_info *info)
{
  return isPresent(info) || isOutgoing(info);
}

// COMPLETION CONVERSIONS

/**********************************************************************/
static inline struct vdo_page_completion *
asVDOPageCompletion(VDOCompletion *completion)
{
  assertCompletionType(completion->type, VDO_PAGE_COMPLETION);
  return (struct vdo_page_completion *) ((uintptr_t) completion
                                         - offsetof(struct vdo_page_completion,
                                                    completion));
}

/**********************************************************************/
static inline
struct vdo_page_completion *pageCompletionFromWaiter(Waiter *waiter)
{
  if (waiter == NULL) {
    return NULL;
  }

  struct vdo_page_completion *completion = (struct vdo_page_completion *)
    ((uintptr_t) waiter - offsetof(struct vdo_page_completion, waiter));
  assertCompletionType(completion->completion.type, VDO_PAGE_COMPLETION);
  return completion;
}

// COMMONLY USED FUNCTIONS

// All of these functions are prefixed "vpc" in order to prevent namespace
// issues (ordinarily they would be static).

/**
 * Find the page info (if any) associated with a given pbn.
 *
 * @param cache  the page cache
 * @param pbn    the absolute physical block number of the page
 *
 * @return the page info for the page if available, or NULL if not
 **/
struct page_info *vpcFindPage(struct vdo_page_cache *cache,
                              PhysicalBlockNumber    pbn)
  __attribute__((warn_unused_result));

/**
 * Return the name of a page state.
 *
 * @param state     a page state
 *
 * @return a pointer to a static page state name
 *
 * @note If the page state is invalid a static string is returned and the
 *       invalid state is logged.
 **/
const char *vpcPageStateName(PageState state)
  __attribute__((warn_unused_result));

#endif // VDO_PAGE_CACHE_INTERNALS_H
