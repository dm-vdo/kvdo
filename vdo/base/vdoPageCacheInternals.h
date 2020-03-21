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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoPageCacheInternals.h#15 $
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
 * A page_info_node is a ring node.
 **/
typedef RingNode page_info_node;

/**
 * The VDO Page Cache abstraction.
 **/
struct vdo_page_cache {
	/** the physical layer to page to */
	PhysicalLayer *layer;
	/** number of pages in cache */
	PageCount page_count;
	/** function to call on page read */
	vdo_page_read_function *read_hook;
	/** function to call on page write */
	vdo_page_write_function *write_hook;
	/** number of pages to write in the current batch */
	PageCount pages_in_batch;
	/** Whether the VDO is doing a read-only rebuild */
	bool rebuilding;

	/** array of page information entries */
	struct page_info *infos;
	/** raw memory for pages */
	char *pages;
	/** cache last found page info */
	struct page_info *last_found;
	/** map of page number to info */
	struct int_map *page_map;
	/** master LRU list (all infos) */
	page_info_node lru_list;
	/** dirty pages by period */
	struct dirty_lists *dirty_lists;
	/** free page list (oldest first) */
	page_info_node free_list;
	/** outgoing page list */
	page_info_node outgoing_list;
	/** number of read I/O operations pending */
	PageCount outstanding_reads;
	/** number of write I/O operations pending */
	PageCount outstanding_writes;
	/** number of pages covered by the current flush */
	PageCount pages_in_flush;
	/** number of pages waiting to be included in the next flush */
	PageCount pages_to_flush;
	/** number of discards in progress */
	unsigned int discard_count;
	/** how many VPCs waiting for free page */
	unsigned int waiter_count;
	/** queue of waiters who want a free page */
	struct wait_queue free_waiters;
	/** statistics */
	struct atomic_page_cache_statistics stats;
	/** counter for pressure reports */
	uint32_t pressure_report;
	/** the block map zone to which this cache belongs */
	struct block_map_zone *zone;
};

/**
 * The state of a page buffer. If the page buffer is free no particular page is
 * bound to it, otherwise the page buffer is bound to particular page whose
 * absolute pbn is in the pbn field. If the page is resident or dirty the page
 * data is stable and may be accessed. Otherwise the page is in flight
 * (incoming or outgoing) and its data should not be accessed.
 *
 * @note Update the static data in vpc_page_state_name() if you change this
 * enumeration.
 **/
typedef enum __attribute__((packed)) {
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
} page_state;

/**
 * The write status of page
 **/
typedef enum __attribute__((packed)) {
	WRITE_STATUS_NORMAL,
	WRITE_STATUS_DISCARD,
	WRITE_STATUS_DEFERRED,
} write_status;

/**
 * Per-page-slot information.
 **/
struct page_info {
	/** Preallocated page struct vio */
	struct vio *vio;
	/** back-link for references */
	struct vdo_page_cache *cache;
	/** the pbn of the page */
	PhysicalBlockNumber pbn;
	/** page is busy (temporarily locked) */
	uint16_t busy;
	/** the write status the page */
	write_status write_status;
	/** page state */
	page_state state;
	/** queue of completions awaiting this item */
	struct wait_queue waiting;
	/** state linked list node */
	page_info_node list_node;
	/** LRU node */
	page_info_node lru_node;
	/** Space for per-page client data */
	byte context[MAX_PAGE_CONTEXT_SIZE];
};

// PAGE INFO LIST OPERATIONS

/**********************************************************************/
static inline struct page_info *page_info_from_list_node(page_info_node *node)
{
	if (node == NULL) {
		return NULL;
	}
	return container_of(node, struct page_info, list_node);
}

/**********************************************************************/
static inline struct page_info *page_info_from_lru_node(page_info_node *node)
{
	if (node == NULL) {
		return NULL;
	}
	return container_of(node, struct page_info, lru_node);
}

// PAGE INFO STATE ACCESSOR FUNCTIONS

/**********************************************************************/
static inline bool is_free(const struct page_info *info)
{
	return info->state == PS_FREE;
}

/**********************************************************************/
static inline bool is_available(const struct page_info *info)
{
	return (info->state == PS_FREE) || (info->state == PS_FAILED);
}

/**********************************************************************/
static inline bool is_present(const struct page_info *info)
{
	return (info->state == PS_RESIDENT) || (info->state == PS_DIRTY);
}

/**********************************************************************/
static inline bool is_dirty(const struct page_info *info)
{
	return info->state == PS_DIRTY;
}

/**********************************************************************/
static inline bool is_resident(const struct page_info *info)
{
	return info->state == PS_RESIDENT;
}

/**********************************************************************/
static inline bool is_in_flight(const struct page_info *info)
{
	return (info->state == PS_INCOMING) || (info->state == PS_OUTGOING);
}

/**********************************************************************/
static inline bool is_incoming(const struct page_info *info)
{
	return info->state == PS_INCOMING;
}

/**********************************************************************/
static inline bool is_outgoing(const struct page_info *info)
{
	return info->state == PS_OUTGOING;
}

/**********************************************************************/
static inline bool is_valid(const struct page_info *info)
{
	return is_present(info) || is_outgoing(info);
}

// COMPLETION CONVERSIONS

/**********************************************************************/
static inline struct vdo_page_completion *
as_vdo_page_completion(struct vdo_completion *completion)
{
	assertCompletionType(completion->type, VDO_PAGE_COMPLETION);
	return container_of(completion, struct vdo_page_completion, completion);
}

/**********************************************************************/
static inline struct vdo_page_completion *
page_completion_from_waiter(struct waiter *waiter)
{
	if (waiter == NULL) {
		return NULL;
	}

	struct vdo_page_completion *completion =
		container_of(waiter, struct vdo_page_completion, waiter);
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
struct page_info *vpc_find_page(struct vdo_page_cache *cache,
				PhysicalBlockNumber pbn)
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
const char *vpc_page_state_name(page_state state)
	__attribute__((warn_unused_result));

#endif // VDO_PAGE_CACHE_INTERNALS_H
