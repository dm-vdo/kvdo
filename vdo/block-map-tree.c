// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "block-map-tree.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "block-map.h"
#include "block-map-page.h"
#include "constants.h"
#include "data-vio.h"
#include "dirty-lists.h"
#include "forest.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "num-utils.h"
#include "physical-zone.h"
#include "recovery-journal.h"
#include "reference-operation.h"
#include "slab-depot.h"
#include "slab-journal.h"
#include "types.h"
#include "vdo.h"
#include "vdo-page-cache.h"
#include "vio.h"
#include "vio-pool.h"

enum {
	BLOCK_MAP_VIO_POOL_SIZE = 64,
};

struct page_descriptor {
	root_count_t root_index;
	height_t height;
	page_number_t page_index;
	slot_number_t slot;
} __packed;

union page_key {
	struct page_descriptor descriptor;
	uint64_t key;
};

struct write_if_not_dirtied_context {
	struct block_map_tree_zone *zone;
	uint8_t generation;
};

/*
 * Used to indicate that the page holding the location of a tree root has been
 * "loaded".
 */
const physical_block_number_t VDO_INVALID_PBN = 0xFFFFFFFFFFFFFFFF;

static inline struct tree_page *
tree_page_from_list_entry(struct list_head *entry)
{
	return list_entry(entry, struct tree_page, entry);
}

static void write_dirty_pages_callback(struct list_head *expired,
				       void *context);

/*
 * Implements vio_constructor.
 */
static int __must_check
make_block_map_vios(struct vdo *vdo,
		    void *parent,
		    void *buffer,
		    struct vio **vio_ptr)
{
	return create_metadata_vio(vdo,
				   VIO_TYPE_BLOCK_MAP_INTERIOR,
				   VIO_PRIORITY_METADATA,
				   parent,
				   buffer,
				   vio_ptr);
}

int vdo_initialize_tree_zone(struct block_map_zone *zone,
			     struct vdo *vdo,
			     block_count_t era_length)
{
	int result;

	struct block_map_tree_zone *tree_zone = &zone->tree_zone;

	STATIC_ASSERT_SIZEOF(struct page_descriptor, sizeof(uint64_t));
	tree_zone->map_zone = zone;

	result = vdo_make_dirty_lists(era_length, write_dirty_pages_callback,
				      tree_zone, &tree_zone->dirty_lists);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = make_int_map(VDO_LOCK_MAP_CAPACITY, 0,
			      &tree_zone->loading_pages);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return make_vio_pool(vdo,
			     BLOCK_MAP_VIO_POOL_SIZE,
			     zone->thread_id,
			     make_block_map_vios,
			     tree_zone,
			     &tree_zone->vio_pool);
}


void vdo_uninitialize_block_map_tree_zone(struct block_map_tree_zone *tree_zone)
{
	UDS_FREE(UDS_FORGET(tree_zone->dirty_lists));
	free_vio_pool(UDS_FORGET(tree_zone->vio_pool));
	free_int_map(UDS_FORGET(tree_zone->loading_pages));
}

void vdo_set_tree_zone_initial_period(struct block_map_tree_zone *tree_zone,
				      sequence_number_t period)
{
	vdo_set_dirty_lists_current_period(tree_zone->dirty_lists, period);
}

static inline struct block_map_tree_zone * __must_check
get_block_map_tree_zone(struct data_vio *data_vio)
{
	return &(data_vio->logical.zone->block_map_zone->tree_zone);
}

/*
 * Get the page referred to by the lock's tree slot at its current height.
 */
static inline struct tree_page *
get_tree_page(const struct block_map_tree_zone *zone,
	      const struct tree_lock *lock)
{
	return vdo_get_tree_page_by_index(zone->map_zone->block_map->forest,
					  lock->root_index, lock->height,
					  lock->tree_slots[lock->height].page_index);
}

/*
 * Validate and copy a buffer to a page.
 * @pbn: the expected PBN
 */
bool vdo_copy_valid_page(char *buffer, nonce_t nonce,
			 physical_block_number_t pbn,
			 struct block_map_page *page)
{
	struct block_map_page *loaded = (struct block_map_page *) buffer;
	enum block_map_page_validity validity =
		vdo_validate_block_map_page(loaded, nonce, pbn);
	if (validity == VDO_BLOCK_MAP_PAGE_VALID) {
		memcpy(page, loaded, VDO_BLOCK_SIZE);
		return true;
	}

	if (validity == VDO_BLOCK_MAP_PAGE_BAD) {
		uds_log_error_strerror(VDO_BAD_PAGE,
				       "Expected page %llu but got page %llu instead",
				       (unsigned long long) pbn,
				       (unsigned long long) vdo_get_block_map_page_pbn(loaded));
	}

	return false;
}

bool vdo_is_tree_zone_active(struct block_map_tree_zone *zone)
{
	return ((zone->active_lookups != 0) ||
		has_waiters(&zone->flush_waiters) ||
		is_vio_pool_busy(zone->vio_pool));
}

static void enter_zone_read_only_mode(struct block_map_tree_zone *zone,
				      int result)
{
	vdo_enter_read_only_mode(zone->map_zone->read_only_notifier, result);

	/*
	 * We are in read-only mode, so we won't ever write any page out. Just
	 * take all waiters off the queue so the tree zone can be closed.
	 */
	while (has_waiters(&zone->flush_waiters)) {
		dequeue_next_waiter(&zone->flush_waiters);
	}

	vdo_block_map_check_for_drain_complete(zone->map_zone);
}

/*
 * Check whether the given value is between the lower and upper bounds,
 * within a cyclic range of values from 0 to (modulus - 1). The value
 * and both bounds must be smaller than the modulus.
 *
 * @lower: The lowest value to accept
 * @value: The value to check
 * @upper: The highest value to accept
 * @modulus: The size of the cyclic space, no more than 2^15
 * @return whether the value is in range
 */
static bool in_cyclic_range(uint16_t lower, uint16_t value,
				     uint16_t upper, uint16_t modulus)
{
	if (value < lower) {
		value += modulus;
	}
	if (upper < lower) {
		upper += modulus;
	}
	return (value <= upper);
}

/*
 * Check whether a generation is strictly older than some other generation in
 * the context of a zone's current generation range.
 *
 * @zone: The zone in which to do the comparison
 * @a: The generation in question
 * @b: The generation to compare to
 *
 * @return if generation @a is not strictly older than generation @b in the
 *	   context of @zone
 */
static bool __must_check
is_not_older(struct block_map_tree_zone *zone, uint8_t a, uint8_t b)
{
	int result = ASSERT((in_cyclic_range(zone->oldest_generation, a,
					     zone->generation, 1 << 8)
			     && in_cyclic_range(zone->oldest_generation, b,
						zone->generation, 1 << 8)),
			    "generation(s) %u, %u are out of range [%u, %u]",
			    a, b, zone->oldest_generation, zone->generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return true;
	}

	return in_cyclic_range(b, a, zone->generation, 1 << 8);
}

static void release_generation(struct block_map_tree_zone *zone,
			       uint8_t generation)
{
	int result = ASSERT((zone->dirty_page_counts[generation] > 0),
			    "dirty page count underflow for generation %u",
			    generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	zone->dirty_page_counts[generation]--;
	while ((zone->dirty_page_counts[zone->oldest_generation] == 0) &&
	       (zone->oldest_generation != zone->generation)) {
		zone->oldest_generation++;
	}
}

static void set_generation(struct block_map_tree_zone *zone,
			   struct tree_page *page, uint8_t new_generation)
{
	uint32_t new_count;
	int result;
	bool decrement_old = is_waiting(&page->waiter);

	uint8_t old_generation = page->generation;

	if (decrement_old && (old_generation == new_generation)) {
		return;
	}

	page->generation = new_generation;
	new_count = ++zone->dirty_page_counts[new_generation];
	result = ASSERT((new_count != 0),
			"dirty page count overflow for generation %u",
			new_generation);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
		return;
	}

	if (decrement_old) {
		release_generation(zone, old_generation);
	}
}

static void write_page(struct tree_page *tree_page,
		       struct vio_pool_entry *entry);

/*
 * Implements waiter_callback
 */
static void write_page_callback(struct waiter *waiter, void *context)
{
	write_page(container_of(waiter, struct tree_page, waiter),
		   (struct vio_pool_entry *) context);
}

static void acquire_vio(struct waiter *waiter, struct block_map_tree_zone *zone)
{
	int result;

	waiter->callback = write_page_callback;
	result = acquire_vio_from_pool(zone->vio_pool, waiter);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
	}
}

/*
 * @return true if all possible generations were not already active
 */
static bool attempt_increment(struct block_map_tree_zone *zone)
{
	uint8_t generation = zone->generation + 1;

	if (zone->oldest_generation == generation) {
		return false;
	}

	zone->generation = generation;
	return true;
}

/*
 * Launches a flush if one is not already in progress.
 */
static void enqueue_page(struct tree_page *page,
			 struct block_map_tree_zone *zone)
{
	int result;

	if ((zone->flusher == NULL) && attempt_increment(zone)) {
		zone->flusher = page;
		acquire_vio(&page->waiter, zone);
		return;
	}

	result = enqueue_waiter(&zone->flush_waiters, &page->waiter);
	if (result != VDO_SUCCESS) {
		enter_zone_read_only_mode(zone, result);
	}
}

static void write_page_if_not_dirtied(struct waiter *waiter, void *context)
{
	struct tree_page *page = container_of(waiter, struct tree_page, waiter);
	struct write_if_not_dirtied_context *write_context = context;

	if (page->generation == write_context->generation) {
		acquire_vio(waiter, write_context->zone);
		return;
	}

	enqueue_page(page, write_context->zone);
}

static void return_to_pool(struct block_map_tree_zone *zone,
			   struct vio_pool_entry *entry)
{
	return_vio_to_pool(zone->vio_pool, entry);
	vdo_block_map_check_for_drain_complete(zone->map_zone);
}

/*
 * This callback is registered in write_initialized_page().
 */
static void finish_page_write(struct vdo_completion *completion)
{
	bool dirty;

	struct vio_pool_entry *entry = completion->parent;
	struct tree_page *page = entry->parent;
	struct block_map_tree_zone *zone = entry->context;

	vdo_release_recovery_journal_block_reference(zone->map_zone->block_map->journal,
						     page->writing_recovery_lock,
						     VDO_ZONE_TYPE_LOGICAL,
						     zone->map_zone->zone_number);

	dirty = (page->writing_generation != page->generation);
	release_generation(zone, page->writing_generation);
	page->writing = false;

	if (zone->flusher == page) {
		struct write_if_not_dirtied_context context = {
			.zone = zone,
			.generation = page->writing_generation,
		};
		notify_all_waiters(&zone->flush_waiters,
				   write_page_if_not_dirtied,
				   &context);
		if (dirty && attempt_increment(zone)) {
			write_page(page, entry);
			return;
		}

		zone->flusher = NULL;
	}

	if (dirty) {
		enqueue_page(page, zone);
	} else if ((zone->flusher == NULL) &&
		   has_waiters(&zone->flush_waiters) &&
		   attempt_increment(zone)) {
		zone->flusher =
			container_of(dequeue_next_waiter(&zone->flush_waiters),
				     struct tree_page, waiter);
		write_page(zone->flusher, entry);
		return;
	}

	return_to_pool(zone, entry);
}

static void handle_write_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct block_map_tree_zone *zone = entry->context;

	record_metadata_io_error(as_vio(completion));
	enter_zone_read_only_mode(zone, result);
	return_to_pool(zone, entry);
}

static void write_page_endio(struct bio *bio);

static void write_initialized_page(struct vdo_completion *completion)
{
	struct vio_pool_entry *entry = completion->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	struct tree_page *tree_page = (struct tree_page *) entry->parent;
	struct block_map_page *page = (struct block_map_page *) entry->buffer;
	unsigned int operation = REQ_OP_WRITE | REQ_PRIO;

	/*
	 * Now that we know the page has been written at least once, mark
	 * the copy we are writing as initialized.
	 */
	vdo_mark_block_map_page_initialized(page, true);


	if (zone->flusher == tree_page) {
		operation |= REQ_PREFLUSH;
	}

	submit_metadata_vio(entry->vio,
			    vdo_get_block_map_page_pbn(page),
			    write_page_endio,
			    handle_write_error,
			    operation);
}

static void write_page_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct block_map_tree_zone *zone = entry->context;
	struct block_map_page *page = (struct block_map_page *) entry->buffer;

	continue_vio_after_io(vio,
			      (vdo_is_block_map_page_initialized(page)
			       ? finish_page_write
			       : write_initialized_page),
			      zone->map_zone->thread_id);
}

static void write_page(struct tree_page *tree_page,
		       struct vio_pool_entry *entry)
{
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	struct vdo_completion *completion = vio_as_completion(entry->vio);
	struct block_map_page *page = vdo_as_block_map_page(tree_page);

	if ((zone->flusher != tree_page) &&
	    (is_not_older(zone, tree_page->generation, zone->generation))) {
		/*
		 * This page was re-dirtied after the last flush was issued,
		 * hence we need to do another flush.
		 */
		enqueue_page(tree_page, zone);
		return_to_pool(zone, entry);
		return;
	}

	entry->parent = tree_page;
	memcpy(entry->buffer, tree_page->page_buffer, VDO_BLOCK_SIZE);

	completion->callback_thread_id = zone->map_zone->thread_id;

	tree_page->writing = true;
	tree_page->writing_generation = tree_page->generation;
	tree_page->writing_recovery_lock = tree_page->recovery_lock;

	/* Clear this now so that we know this page is not on any dirty list. */
	tree_page->recovery_lock = 0;

	/*
         * We've already copied the page into the vio which will write it, so
         * if it was not yet initialized, the first write will indicate that
         * (for torn write protection). It is now safe to mark it as
         * initialized in memory since if the write fails, the in memory state
         * will become irrelevant.
	 */
	if (!vdo_mark_block_map_page_initialized(page, true)) {
		write_initialized_page(completion);
		return;
	}

	submit_metadata_vio(entry->vio,
			    vdo_get_block_map_page_pbn(page),
			    write_page_endio,
			    handle_write_error,
			    REQ_OP_WRITE | REQ_PRIO);
}

/*
 * Schedule a batch of dirty pages for writing.
 *
 * Implements vdo_dirty_callback.
 **/
static void write_dirty_pages_callback(struct list_head *expired, void *context)
{
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) context;
	uint8_t generation = zone->generation;

	while (!list_empty(expired)) {
		int result;
		struct list_head *entry = expired->next;
		struct tree_page *page = tree_page_from_list_entry(entry);

		list_del_init(entry);

		result = ASSERT(!is_waiting(&page->waiter),
				"Newly expired page not already waiting to write");
		if (result != VDO_SUCCESS) {
			enter_zone_read_only_mode(zone, result);
			continue;
		}

		set_generation(zone, page, generation);
		if (!page->writing) {
			enqueue_page(page, zone);
		}
	}
}

void vdo_advance_zone_tree_period(struct block_map_tree_zone *zone,
				  sequence_number_t period)
{
	vdo_advance_dirty_lists_period(zone->dirty_lists, period);
}

/*
 * This method must not be called when lookups are active.
 **/
void vdo_drain_zone_trees(struct block_map_tree_zone *zone)
{
	ASSERT_LOG_ONLY((zone->active_lookups == 0),
			"vdo_drain_zone_trees() called with no active lookups");
	if (!vdo_is_state_suspending(&zone->map_zone->state)) {
		vdo_flush_dirty_lists(zone->dirty_lists);
	}
}

/*
 * Release a lock on a page which was being loaded or allocated.
 */
static void release_page_lock(struct data_vio *data_vio, char *what)
{
	struct block_map_tree_zone *zone;
	struct tree_lock *lock_holder;

	struct tree_lock *lock = &data_vio->tree_lock;

	ASSERT_LOG_ONLY(lock->locked,
			"release of unlocked block map page %s for key %llu in tree %u",
			what, (unsigned long long) lock->key,
			lock->root_index);

	zone = get_block_map_tree_zone(data_vio);
	lock_holder = int_map_remove(zone->loading_pages, lock->key);
	ASSERT_LOG_ONLY((lock_holder == lock),
			"block map page %s mismatch for key %llu in tree %u",
			what, (unsigned long long) lock->key,
			lock->root_index);
	lock->locked = false;
}

static void finish_lookup(struct data_vio *data_vio, int result)
{
	struct block_map_tree_zone *zone;
	struct vdo_completion *completion = data_vio_as_completion(data_vio);

	data_vio->tree_lock.height = 0;

	zone = get_block_map_tree_zone(data_vio);
	--zone->active_lookups;

	vdo_set_completion_result(completion, result);
	vdo_launch_completion_callback(completion,
				       data_vio->tree_lock.callback,
				       data_vio->tree_lock.thread_id);
}

static void abort_lookup_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	int result = *((int *) context);

	if (is_read_data_vio(data_vio)) {
		if (result == VDO_NO_SPACE) {
			result = VDO_SUCCESS;
		}
	} else if (result != VDO_NO_SPACE) {
		result = VDO_READ_ONLY;
	}

	finish_lookup(data_vio, result);
}

static void abort_lookup(struct data_vio *data_vio, int result, char *what)
{
	if (result != VDO_NO_SPACE) {
		enter_zone_read_only_mode(get_block_map_tree_zone(data_vio),
					  result);
	}

	if (data_vio->tree_lock.locked) {
		release_page_lock(data_vio, what);
		notify_all_waiters(&data_vio->tree_lock.waiters,
				   abort_lookup_for_waiter, &result);
	}

	finish_lookup(data_vio, result);
}

static void abort_load(struct data_vio *data_vio, int result)
{
	abort_lookup(data_vio, result, "load");
}

static bool __must_check
is_invalid_tree_entry(const struct vdo *vdo,
		      const struct data_location *mapping,
		      height_t height)
{
	if (!vdo_is_valid_location(mapping) ||
	    vdo_is_state_compressed(mapping->state) ||
	    (vdo_is_mapped_location(mapping) &&
	     (mapping->pbn == VDO_ZERO_BLOCK))) {
		return true;
	}

	/* Roots aren't physical data blocks, so we can't check their PBNs. */
	if (height == VDO_BLOCK_MAP_TREE_HEIGHT) {
		return false;
	}

	return !vdo_is_physical_data_block(vdo->depot, mapping->pbn);
}

static void load_block_map_page(struct block_map_tree_zone *zone,
				struct data_vio *data_vio);

static void allocate_block_map_page(struct block_map_tree_zone *zone,
				    struct data_vio *data_vio);

static void continue_with_loaded_page(struct data_vio *data_vio,
				      struct block_map_page *page)
{
	struct tree_lock *lock = &data_vio->tree_lock;
	struct block_map_tree_slot slot = lock->tree_slots[lock->height];
	struct data_location mapping =
		vdo_unpack_block_map_entry(&page->entries[slot.block_map_slot.slot]);
	if (is_invalid_tree_entry(vdo_from_data_vio(data_vio), &mapping,
				  lock->height)) {
		uds_log_error_strerror(VDO_BAD_MAPPING,
				       "Invalid block map tree PBN: %llu with state %u for page index %u at height %u",
				       (unsigned long long) mapping.pbn,
				       mapping.state,
				       lock->tree_slots[lock->height - 1].page_index,
				       lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!vdo_is_mapped_location(&mapping)) {
		/* The page we need is unallocated */
		allocate_block_map_page(get_block_map_tree_zone(data_vio),
					data_vio);
		return;
	}

	lock->tree_slots[lock->height - 1].block_map_slot.pbn = mapping.pbn;
	if (lock->height == 1) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	/* We know what page we need to load next */
	load_block_map_page(get_block_map_tree_zone(data_vio), data_vio);
}

static void continue_load_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);

	data_vio->tree_lock.height--;
	continue_with_loaded_page(data_vio, (struct block_map_page *) context);
}

static void finish_block_map_page_load(struct vdo_completion *completion)
{
	physical_block_number_t pbn;
	struct tree_page *tree_page;
	struct block_map_page *page;
	nonce_t nonce;

	struct vio_pool_entry *entry = completion->parent;
	struct data_vio *data_vio = entry->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;
	struct tree_lock *tree_lock = &data_vio->tree_lock;

	tree_lock->height--;
	pbn = tree_lock->tree_slots[tree_lock->height].block_map_slot.pbn;
	tree_page = get_tree_page(zone, tree_lock);
	page = (struct block_map_page *) tree_page->page_buffer;
	nonce = zone->map_zone->block_map->nonce;

	if (!vdo_copy_valid_page(entry->buffer, nonce, pbn, page)) {
		vdo_format_block_map_page(page, nonce, pbn, false);
	}
	return_vio_to_pool(zone->vio_pool, entry);

	/* Release our claim to the load and wake any waiters */
	release_page_lock(data_vio, "load");
	notify_all_waiters(&tree_lock->waiters, continue_load_for_waiter, page);
	continue_with_loaded_page(data_vio, page);
}

static void handle_io_error(struct vdo_completion *completion)
{
	int result = completion->result;
	struct vio_pool_entry *entry = completion->parent;
	struct data_vio *data_vio = entry->parent;
	struct block_map_tree_zone *zone =
		(struct block_map_tree_zone *) entry->context;

	record_metadata_io_error(as_vio(completion));
	return_vio_to_pool(zone->vio_pool, entry);
	abort_load(data_vio, result);
}

static void load_page_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vio_pool_entry *entry = vio->completion.parent;
	struct data_vio *data_vio = entry->parent;

	continue_vio_after_io(vio,
			      finish_block_map_page_load,
			      data_vio->logical.zone->thread_id);
}

static void load_page(struct waiter *waiter, void *context)
{
	struct vio_pool_entry *entry = context;
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct tree_lock *lock = &data_vio->tree_lock;
	physical_block_number_t pbn =
		lock->tree_slots[lock->height - 1].block_map_slot.pbn;

	entry->parent = data_vio;
	submit_metadata_vio(entry->vio,
			    pbn,
			    load_page_endio,
			    handle_io_error,
			    REQ_OP_READ | REQ_PRIO);
}

/*
 * If the page is already locked, queue up to wait for the lock to be released.
 * If the lock is acquired, @data_vio->tree_lock.locked will be true.
 */
static int attempt_page_lock(struct block_map_tree_zone *zone,
			     struct data_vio *data_vio)
{
	int result;
	struct tree_lock *lock_holder;

	struct tree_lock *lock = &data_vio->tree_lock;
	height_t height = lock->height;
	struct block_map_tree_slot tree_slot = lock->tree_slots[height];
	union page_key key;

	key.descriptor = (struct page_descriptor) {
		.root_index = lock->root_index,
		.height = height,
		.page_index = tree_slot.page_index,
		.slot = tree_slot.block_map_slot.slot,
	};
	lock->key = key.key;

	result = int_map_put(zone->loading_pages, lock->key, lock, false,
				 (void **) &lock_holder);
	if (result != VDO_SUCCESS) {
		return result;
	}

	if (lock_holder == NULL) {
		/* We got the lock */
		data_vio->tree_lock.locked = true;
		return VDO_SUCCESS;
	}

	/* Someone else is loading or allocating the page we need */
	return enqueue_data_vio(&lock_holder->waiters, data_vio);
}

/*
 * Load a block map tree page from disk, for the next level in the data vio
 * tree lock.
 */
static void load_block_map_page(struct block_map_tree_zone *zone,
				struct data_vio *data_vio)
{
	int result = attempt_page_lock(zone, data_vio);

	if (result != VDO_SUCCESS) {
		abort_load(data_vio, result);
		return;
	}

	if (data_vio->tree_lock.locked) {
		struct waiter *waiter = data_vio_as_waiter(data_vio);

		waiter->callback = load_page;
		result = acquire_vio_from_pool(zone->vio_pool, waiter);
		if (result != VDO_SUCCESS) {
			abort_load(data_vio, result);
		}
	}
}

static void set_post_allocation_callback(struct data_vio *data_vio)
{
	vdo_set_completion_callback(data_vio_as_completion(data_vio),
				    data_vio->tree_lock.callback,
				    data_vio->tree_lock.thread_id);
}

static void abort_allocation(struct data_vio *data_vio, int result)
{
	set_post_allocation_callback(data_vio);
	abort_lookup(data_vio, result, "allocation");
}

static void allocation_failure(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	if (vdo_get_callback_thread_id() !=
	    data_vio->logical.zone->thread_id) {
		launch_data_vio_logical_callback(data_vio, allocation_failure);
		return;
	}

	completion->error_handler = NULL;
	abort_allocation(data_vio, completion->result);
}

static void continue_allocation_for_waiter(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	physical_block_number_t pbn = *((physical_block_number_t *) context);

	tree_lock->height--;
	data_vio->tree_lock.tree_slots[tree_lock->height].block_map_slot.pbn =
		pbn;

	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(get_block_map_tree_zone(data_vio), data_vio);
}

/*
 * Record the allocation in the tree and wake any waiters now that the write
 * lock has been released.
 */
static void finish_block_map_allocation(struct vdo_completion *completion)
{
	physical_block_number_t pbn;
	struct tree_page *tree_page;
	struct block_map_page *page;
	sequence_number_t old_lock;

	struct data_vio *data_vio = as_data_vio(completion);
	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);
	struct tree_lock *tree_lock = &data_vio->tree_lock;
	height_t height = tree_lock->height;

	assert_data_vio_in_logical_zone(data_vio);

	completion->error_handler = NULL;

	tree_page = get_tree_page(zone, tree_lock);

	pbn = tree_lock->tree_slots[height - 1].block_map_slot.pbn;

	/* Record the allocation. */
	page = (struct block_map_page *) tree_page->page_buffer;
	old_lock = tree_page->recovery_lock;
	vdo_update_block_map_page(page, data_vio, pbn,
				  VDO_MAPPING_STATE_UNCOMPRESSED,
				  &tree_page->recovery_lock);

	if (is_waiting(&tree_page->waiter)) {
		/* This page is waiting to be written out. */
		if (zone->flusher != tree_page) {
			/*
			 * The outstanding flush won't cover the update we just
			 * made, so mark the page as needing another flush.
			 */
			set_generation(zone, tree_page, zone->generation);
		}
	} else {
		/* Put the page on a dirty list */
		if (old_lock == 0) {
			INIT_LIST_HEAD(&tree_page->entry);
		}
		vdo_add_to_dirty_lists(zone->dirty_lists,
				       &tree_page->entry,
				       old_lock,
				       tree_page->recovery_lock);
	}

	tree_lock->height--;
	if (height > 1) {
		/* Format the interior node we just allocated (in memory). */
		tree_page = get_tree_page(zone, tree_lock);
		vdo_format_block_map_page(tree_page->page_buffer,
					  zone->map_zone->block_map->nonce,
					  pbn,
					  false);
	}

	/* Release our claim to the allocation and wake any waiters */
	release_page_lock(data_vio, "allocation");
	notify_all_waiters(&tree_lock->waiters, continue_allocation_for_waiter,
			   &pbn);
	if (tree_lock->height == 0) {
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	allocate_block_map_page(zone, data_vio);
}

static void release_block_map_write_lock(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_allocated_zone(data_vio);

	release_data_vio_allocation_lock(data_vio, true);
	launch_data_vio_logical_callback(data_vio,
					 finish_block_map_allocation);
}

/*
 * Newly allocated block map pages are set to have to MAXIMUM_REFERENCES after
 * they are journaled, to prevent deduplication against the block after
 * we release the write lock on it, but before we write out the page.
 */
static void
set_block_map_page_reference_count(struct vdo_completion *completion)
{
	physical_block_number_t pbn;

	struct data_vio *data_vio = as_data_vio(completion);
	struct tree_lock *lock = &data_vio->tree_lock;

	assert_data_vio_in_allocated_zone(data_vio);

	pbn = lock->tree_slots[lock->height - 1].block_map_slot.pbn;
	completion->callback = release_block_map_write_lock;
	vdo_add_slab_journal_entry(vdo_get_slab_journal(completion->vdo->depot,
							pbn),
				   data_vio);
}

static void journal_block_map_allocation(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);

	assert_data_vio_in_journal_zone(data_vio);

	set_data_vio_allocated_zone_callback(data_vio,
					     set_block_map_page_reference_count);
	vdo_add_recovery_journal_entry(vdo_from_data_vio(data_vio)->recovery_journal,
				       data_vio);
}

static void allocate_block(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	struct tree_lock *lock = &data_vio->tree_lock;
	physical_block_number_t pbn;

	assert_data_vio_in_allocated_zone(data_vio);

	if (!vdo_allocate_block_in_zone(data_vio)) {
		return;
	}

	pbn = data_vio->allocation.pbn;
	lock->tree_slots[lock->height - 1].block_map_slot.pbn = pbn;
	vdo_set_up_reference_operation_with_lock(VDO_JOURNAL_BLOCK_MAP_INCREMENT,
						 pbn,
						 VDO_MAPPING_STATE_UNCOMPRESSED,
						 data_vio->allocation.lock,
						 &data_vio->operation);
	launch_data_vio_journal_callback(data_vio,
					 journal_block_map_allocation);
}

static void allocate_block_map_page(struct block_map_tree_zone *zone,
				    struct data_vio *data_vio)
{
	int result;

	if (!is_write_data_vio(data_vio) || is_trim_data_vio(data_vio)) {
		/*
		 * This is a pure read, the read phase of a read-modify-write,
		 * or a trim, so there's nothing left to do here.
		 */
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	result = attempt_page_lock(zone, data_vio);
	if (result != VDO_SUCCESS) {
		abort_allocation(data_vio, result);
		return;
	}

	if (!data_vio->tree_lock.locked) {
		return;
	}

	data_vio_allocate_data_block(data_vio,
				     VIO_BLOCK_MAP_WRITE_LOCK,
				     allocate_block,
				     allocation_failure);
}

/*
 * Look up the PBN of the block map page containing the mapping for a
 * data_vio's LBN. All ancestors in the tree will be allocated or loaded, as
 * needed.
 */
void vdo_lookup_block_map_pbn(struct data_vio *data_vio)
{
	page_number_t page_index;
	struct block_map_tree_slot tree_slot;
	struct data_location mapping;

	struct block_map_page *page = NULL;
	struct tree_lock *lock = &data_vio->tree_lock;
	struct block_map_tree_zone *zone = get_block_map_tree_zone(data_vio);

	zone->active_lookups++;
	if (vdo_is_state_draining(&zone->map_zone->state)) {
		finish_lookup(data_vio, VDO_SHUTTING_DOWN);
		return;
	}

	page_index = (lock->tree_slots[0].page_index /
		      zone->map_zone->block_map->root_count);
	tree_slot = (struct block_map_tree_slot) {
		.page_index = page_index / VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		.block_map_slot = {
			.pbn = 0,
			.slot = page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE,
		},
	};

	for (lock->height = 1; lock->height <= VDO_BLOCK_MAP_TREE_HEIGHT;
	     lock->height++) {
		physical_block_number_t pbn;

		lock->tree_slots[lock->height] = tree_slot;
		page = (struct block_map_page *) (get_tree_page(zone, lock)->page_buffer);
		pbn = vdo_get_block_map_page_pbn(page);
		if (pbn != VDO_ZERO_BLOCK) {
			lock->tree_slots[lock->height].block_map_slot.pbn = pbn;
			break;
		}

		/* Calculate the index and slot for the next level. */
		tree_slot.block_map_slot.slot =
			tree_slot.page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
		tree_slot.page_index =
			tree_slot.page_index / VDO_BLOCK_MAP_ENTRIES_PER_PAGE;
	}

	/* The page at this height has been allocated and loaded. */
	mapping =
		vdo_unpack_block_map_entry(&page->entries[tree_slot.block_map_slot.slot]);
	if (is_invalid_tree_entry(vdo_from_data_vio(data_vio), &mapping,
				  lock->height)) {
		uds_log_error_strerror(VDO_BAD_MAPPING,
				       "Invalid block map tree PBN: %llu with state %u for page index %u at height %u",
				       (unsigned long long) mapping.pbn,
				       mapping.state,
				       lock->tree_slots[lock->height - 1].page_index,
				       lock->height - 1);
		abort_load(data_vio, VDO_BAD_MAPPING);
		return;
	}

	if (!vdo_is_mapped_location(&mapping)) {
		/*
		 * The page we want one level down has not been allocated, so
		 * allocate it.
		 */
		allocate_block_map_page(zone, data_vio);
		return;
	}

	lock->tree_slots[lock->height - 1].block_map_slot.pbn = mapping.pbn;
	if (lock->height == 1) {
		/* This is the ultimate block map page, so we're done */
		finish_lookup(data_vio, VDO_SUCCESS);
		return;
	}

	/* We know what page we need to load. */
	load_block_map_page(zone, data_vio);
}

/*
 * Find the PBN of a leaf block map page. This method may only be used after
 * all allocated tree pages have been loaded, otherwise, it may give the wrong
 * answer (0).
 */
physical_block_number_t vdo_find_block_map_page_pbn(struct block_map *map,
						    page_number_t page_number)
{
	struct data_location mapping;
	struct tree_page *tree_page;
	struct block_map_page *page;

	root_count_t root_index = page_number % map->root_count;
	page_number_t page_index = page_number / map->root_count;
	slot_number_t slot = page_index % VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	page_index /= VDO_BLOCK_MAP_ENTRIES_PER_PAGE;

	tree_page =
		vdo_get_tree_page_by_index(map->forest, root_index, 1, page_index);
	page = (struct block_map_page *) tree_page->page_buffer;
	if (!vdo_is_block_map_page_initialized(page)) {
		return VDO_ZERO_BLOCK;
	}

	mapping = vdo_unpack_block_map_entry(&page->entries[slot]);
	if (!vdo_is_valid_location(&mapping) ||
	    vdo_is_state_compressed(mapping.state)) {
		return VDO_ZERO_BLOCK;
	}
	return mapping.pbn;
}

/*
 * Write a tree page or indicate that it has been re-dirtied if it is already
 * being written. This method is used when correcting errors in the tree during
 * read-only rebuild.
 */
void vdo_write_tree_page(struct tree_page *page,
			 struct block_map_tree_zone *zone)
{
	bool waiting = is_waiting(&page->waiter);

	if (waiting && (zone->flusher == page)) {
		return;
	}

	set_generation(zone, page, zone->generation);
	if (waiting || page->writing) {
		return;
	}

	enqueue_page(page, zone);
}
