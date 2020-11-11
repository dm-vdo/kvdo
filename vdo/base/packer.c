/*
 * Copyright Red Hat
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/packer.c#58 $
 */

#include "packerInternals.h"

#include "logger.h"
#include "memoryAlloc.h"

#include "adminState.h"
#include "allocatingVIO.h"
#include "allocationSelector.h"
#include "compressionState.h"
#include "dataVIO.h"
#include "hashLock.h"
#include "pbnLock.h"
#include "vdo.h"
#include "vdoInternal.h"

/**
 * Check that we are on the packer thread.
 *
 * @param packer  The packer
 * @param caller  The function which is asserting
 **/
static inline void assert_on_packer_thread(struct packer *packer,
					   const char *caller)
{
	ASSERT_LOG_ONLY((get_callback_thread_id() == packer->thread_id),
			"%s() called from packer thread", caller);
}

/**********************************************************************/
struct input_bin *next_bin(const struct packer *packer, struct input_bin *bin)
{
	if (bin->list.next == &packer->input_bins) {
		return NULL;
	} else {
		return container_of(bin->list.next, struct input_bin, list);
	}
}

/**********************************************************************/
struct input_bin *get_fullest_bin(const struct packer *packer)
{
	if (list_empty(&packer->input_bins)) {
		return NULL;
	} else {
		return container_of(packer->input_bins.next,
				    struct input_bin, list);
	}
}

/**
 * Insert an input bin to the list, which is in ascending order of free space.
 * Since all bins are already in the list, this actually moves the bin to the
 * correct position in the list.
 *
 * @param packer  The packer
 * @param bin     The input bin to move to its sorted position
 **/
static void insert_in_sorted_list(struct packer *packer, struct input_bin *bin)
{
	struct input_bin *active_bin;
	for (active_bin = get_fullest_bin(packer); active_bin != NULL;
	     active_bin = next_bin(packer, active_bin)) {
		if (active_bin->free_space > bin->free_space) {
			list_move_tail(&bin->list, &active_bin->list);
			return;
		}
	}

	list_move_tail(&bin->list, &packer->input_bins);
}

/**
 * Allocate an input bin and put it into the packer's list.
 *
 * @param packer  The packer
 **/
static int __must_check make_input_bin(struct packer *packer)
{
	struct input_bin *bin;
	int result = ALLOCATE_EXTENDED(struct input_bin, MAX_COMPRESSION_SLOTS,
				       struct vio *, __func__, &bin);
	if (result != VDO_SUCCESS) {
		return result;
	}

	bin->free_space = packer->bin_data_size;
	INIT_LIST_HEAD(&bin->list);
	list_add_tail(&bin->list, &packer->input_bins);
	return VDO_SUCCESS;
}

/**
 * Push an output bin onto the stack of idle bins.
 *
 * @param packer  The packer
 * @param bin     The output bin
 **/
static void push_output_bin(struct packer *packer, struct output_bin *bin)
{
	ASSERT_LOG_ONLY(!has_waiters(&bin->outgoing),
			"idle output bin has no waiters");
	packer->idle_output_bins[packer->idle_output_bin_count++] = bin;
}

/**
 * Pop an output bin off the end of the stack of idle bins.
 *
 * @param packer  The packer
 *
 * @return an idle output bin, or <code>NULL</code> if there are no idle bins
 **/
static struct output_bin * __must_check pop_output_bin(struct packer *packer)
{
	if (packer->idle_output_bin_count == 0) {
		return NULL;
	}

	size_t index = --packer->idle_output_bin_count;
	struct output_bin *bin = packer->idle_output_bins[index];
	packer->idle_output_bins[index] = NULL;
	return bin;
}

/**
 * Allocate a new output bin and push it onto the packer's stack of idle bins.
 *
 * @param packer  The packer
 * @param layer   The physical layer that will receive the compressed block
 *                writes from the output bin
 *
 * @return VDO_SUCCESS or an error code
 **/
static int __must_check
make_output_bin(struct packer *packer, PhysicalLayer *layer)
{
	struct output_bin *output;
	int result = ALLOCATE(1, struct output_bin, __func__, &output);
	if (result != VDO_SUCCESS) {
		return result;
	}

	// Add the bin to the stack even before it's fully initialized so it
	// will be freed even if we fail to initialize it below.
	INIT_LIST_HEAD(&output->list);
	list_add_tail(&output->list, &packer->output_bins);
	push_output_bin(packer, output);

	result = ALLOCATE_EXTENDED(struct compressed_block,
				   packer->bin_data_size,
				   char, "compressed block",
				   &output->block);
	if (result != VDO_SUCCESS) {
		return result;
	}

	return layer->createCompressedWriteVIO(layer, output,
					       (char *) output->block,
					       &output->writer);
}

/**
 * Free an idle output bin and null out the reference to it.
 *
 * @param bin_ptr  The reference to the output bin to free
 **/
static void free_output_bin(struct output_bin **bin_ptr)
{
	struct output_bin *bin = *bin_ptr;
	if (bin == NULL) {
		return;
	}

	list_del_init(&bin->list);

	struct vio *vio = allocating_vio_as_vio(bin->writer);
	free_vio(&vio);
	FREE(bin->block);
	FREE(bin);
	*bin_ptr = NULL;
}

/**********************************************************************/
int make_packer(PhysicalLayer *layer,
		block_count_t input_bin_count,
		block_count_t output_bin_count,
		const struct thread_config *thread_config,
		struct packer **packer_ptr)
{
	struct packer *packer;
	int result = ALLOCATE_EXTENDED(struct packer, output_bin_count,
				       struct output_bin *, __func__, &packer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	packer->thread_id = get_packer_zone_thread(thread_config);
	packer->bin_data_size = VDO_BLOCK_SIZE - sizeof(compressed_block_header);
	packer->size = input_bin_count;
	packer->max_slots = MAX_COMPRESSION_SLOTS;
	packer->output_bin_count = output_bin_count;
	INIT_LIST_HEAD(&packer->input_bins);
	INIT_LIST_HEAD(&packer->output_bins);

	result = make_allocation_selector(thread_config->physical_zone_count,
					  packer->thread_id, &packer->selector);
	if (result != VDO_SUCCESS) {
		free_packer(&packer);
		return result;
	}

	block_count_t i;
	for (i = 0; i < input_bin_count; i++) {
		int result = make_input_bin(packer);
		if (result != VDO_SUCCESS) {
			free_packer(&packer);
			return result;
		}
	}

	/*
	 * The canceled bin can hold up to half the number of user vios. Every
	 * canceled vio in the bin must have a canceler for which it is waiting,
	 * and any canceler will only have canceled one lock holder at a time.
	 */
	result = ALLOCATE_EXTENDED(struct input_bin, MAXIMUM_USER_VIOS / 2,
				   struct vio *, __func__,
				   &packer->canceled_bin);
	if (result != VDO_SUCCESS) {
		free_packer(&packer);
		return result;
	}

	for (i = 0; i < output_bin_count; i++) {
		int result = make_output_bin(packer, layer);
		if (result != VDO_SUCCESS) {
			free_packer(&packer);
			return result;
		}
	}

	*packer_ptr = packer;
	return VDO_SUCCESS;
}

/**********************************************************************/
void free_packer(struct packer **packer_ptr)
{
	struct packer *packer = *packer_ptr;
	if (packer == NULL) {
		return;
	}

	struct input_bin *input;
	while ((input = get_fullest_bin(packer)) != NULL) {
		list_del_init(&input->list);
		FREE(input);
	}

	FREE(packer->canceled_bin);

	struct output_bin *output;
	while ((output = pop_output_bin(packer)) != NULL) {
		free_output_bin(&output);
	}

	free_allocation_selector(&packer->selector);
	FREE(packer);
	*packer_ptr = NULL;
}

/**
 * Get the packer from a data_vio.
 *
 * @param data_vio  The data_vio
 *
 * @return The packer from the VDO to which the data_vio belongs
 **/
static inline struct packer *get_packer_from_data_vio(struct data_vio *data_vio)
{
	return get_vdo_from_data_vio(data_vio)->packer;
}

/**********************************************************************/
bool is_sufficiently_compressible(struct data_vio *data_vio)
{
	struct packer *packer = get_packer_from_data_vio(data_vio);
	return (data_vio->compression.size < packer->bin_data_size);
}

/**********************************************************************/
thread_id_t get_packer_thread_id(struct packer *packer)
{
	return packer->thread_id;
}

/**********************************************************************/
struct packer_statistics get_packer_statistics(const struct packer *packer)
{
	const struct packer_statistics *stats = &packer->statistics;
	return (struct packer_statistics) {
		.compressed_fragments_written =
			READ_ONCE(stats->compressed_fragments_written),
		.compressed_blocks_written =
			READ_ONCE(stats->compressed_blocks_written),
		.compressed_fragments_in_packer =
			READ_ONCE(stats->compressed_fragments_in_packer),
	};
}

/**
 * Abort packing a data_vio.
 *
 * @param data_vio     The data_vio to abort
 **/
static void abort_packing(struct data_vio *data_vio)
{
	set_compression_done(data_vio);

	struct packer *packer = get_packer_from_data_vio(data_vio);
	WRITE_ONCE(packer->statistics.compressed_fragments_in_packer,
		   packer->statistics.compressed_fragments_in_packer - 1);

	data_vio_add_trace_record(data_vio, THIS_LOCATION(NULL));
	continue_data_vio(data_vio, VDO_SUCCESS);
}

/**
 * This continues the vio completion without packing the vio.
 *
 * @param waiter  The wait queue entry of the vio to continue
 * @param unused  An argument required so this function may be called
 *                from notify_all_waiters
 **/
static void continue_vio_without_packing(struct waiter *waiter,
					 void *unused __always_unused)
{
	abort_packing(waiter_as_data_vio(waiter));
}

/**
 * Check whether the packer has drained.
 *
 * @param packer  The packer
 **/
static void check_for_drain_complete(struct packer *packer)
{
	if (is_draining(&packer->state) &&
	    (packer->canceled_bin->slots_used == 0) &&
	    (packer->idle_output_bin_count == packer->output_bin_count)) {
		finish_draining(&packer->state);
	}
}

/**********************************************************************/
static void write_pending_batches(struct packer *packer);

/**
 * Ensure that a completion is running on the packer thread.
 *
 * @param completion  The compressed write vio
 *
 * @return <code>true</code> if the completion is on the packer thread
 **/
static bool __must_check
switch_to_packer_thread(struct vdo_completion *completion)
{
	struct vio *vio = as_vio(completion);
	thread_id_t thread_id = vio->vdo->packer->thread_id;
	if (completion->callback_thread_id == thread_id) {
		return true;
	}

	completion->callback_thread_id = thread_id;
	invoke_callback(completion);
	return false;
}

/**
 * Finish processing an output bin whose write has completed. If there was
 * an error, any data_vios waiting on the bin write will be notified.
 *
 * @param packer  The packer which owns the bin
 * @param bin     The bin which has finished
 **/
static void finish_output_bin(struct packer *packer, struct output_bin *bin)
{
	if (has_waiters(&bin->outgoing)) {
		notify_all_waiters(&bin->outgoing, continue_vio_without_packing,
				   NULL);
	} else {
		// No waiters implies no error, so the compressed block was
		// written.
		struct packer_statistics *stats = &packer->statistics;
		WRITE_ONCE(stats->compressed_fragments_in_packer,
			   stats->compressed_fragments_in_packer
			   - bin->slots_used);
		WRITE_ONCE(stats->compressed_fragments_written,
			   stats->compressed_fragments_written
			   + bin->slots_used);
		WRITE_ONCE(stats->compressed_blocks_written,
			   stats->compressed_blocks_written + 1);
	}

	bin->slots_used = 0;
	push_output_bin(packer, bin);
}

/**
 * This finishes the bin write process after the bin is written to disk. This
 * is the vio callback function registered by writeOutputBin().
 *
 * @param completion  The compressed write vio
 **/
static void complete_output_bin(struct vdo_completion *completion)
{
	if (!switch_to_packer_thread(completion)) {
		return;
	}

	struct vio *vio = as_vio(completion);
	if (completion->result != VDO_SUCCESS) {
		update_vio_error_stats(vio,
				       "Completing compressed write vio for physical block %llu with error",
				       vio->physical);
	}

	struct packer *packer = vio->vdo->packer;
	finish_output_bin(packer, completion->parent);
	write_pending_batches(packer);
	check_for_drain_complete(packer);
}

/**
 * Implements WaiterCallback. Continues the data_vio waiter.
 **/
static void continue_waiter(struct waiter *waiter,
			    void *context __always_unused)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	continue_data_vio(data_vio, VDO_SUCCESS);
}

/**
 * Implements WaiterCallback. Updates the data_vio waiter to refer to its slot
 * in the compressed block, gives the data_vio a share of the PBN lock on that
 * block, and reserves a reference count increment on the lock.
 **/
static void share_compressed_block(struct waiter *waiter, void *context)
{
	struct data_vio *data_vio = waiter_as_data_vio(waiter);
	struct output_bin *bin = context;

	data_vio->new_mapped = (struct zoned_pbn) {
		.pbn = bin->writer->allocation,
		.zone = bin->writer->zone,
		.state = get_state_for_slot(data_vio->compression.slot),
	};
	data_vio_as_vio(data_vio)->physical = data_vio->new_mapped.pbn;

	share_compressed_write_lock(data_vio, bin->writer->allocation_lock);

	// Wait again for all the waiters to get a share.
	int result = enqueue_waiter(&bin->outgoing, waiter);
	// Cannot fail since this waiter was just dequeued.
	ASSERT_LOG_ONLY(result == VDO_SUCCESS,
			"impossible enqueue_waiter error");
}

/**
 * Finish a compressed block write. This callback is registered in
 * continueAfterAllocation().
 *
 * @param completion  The compressed write completion
 **/
static void finish_compressed_write(struct vdo_completion *completion)
{
	struct output_bin *bin = completion->parent;
	assert_in_physical_zone(bin->writer);

	if (completion->result != VDO_SUCCESS) {
		release_allocation_lock(bin->writer);
		// Invokes complete_output_bin() on the packer thread, which will
		// deal with the waiters.
		vio_done_callback(completion);
		return;
	}

	// First give every data_vio/HashLock a share of the PBN lock to ensure
	// it can't be released until they've all done their incRefs.
	notify_all_waiters(&bin->outgoing, share_compressed_block, bin);

	// The waiters now hold the (downgraded) PBN lock.
	bin->writer->allocation_lock = NULL;

	// Invokes the callbacks registered before entering the packer.
	notify_all_waiters(&bin->outgoing, continue_waiter, NULL);

	// Invokes complete_output_bin() on the packer thread.
	vio_done_callback(completion);
}

/**
 * Continue the write path for a compressed write allocating_vio now that block
 * allocation is complete (the allocating_vio may or may not have actually
 * received an allocation).
 *
 * @param allocating_vio  The allocating_vio which has finished the allocation
 *                       process
 **/
static void continue_after_allocation(struct allocating_vio *allocating_vio)
{
	struct vio *vio = allocating_vio_as_vio(allocating_vio);
	struct vdo_completion *completion = vio_as_completion(vio);
	if (allocating_vio->allocation == ZERO_BLOCK) {
		completion->requeue = true;
		set_completion_result(completion, VDO_NO_SPACE);
		vio_done_callback(completion);
		return;
	}

	set_physical_zone_callback(allocating_vio,
				   finish_compressed_write,
				   THIS_LOCATION("$F(meta);cb=finish_compressed_write"));
	write_compressed_block(allocating_vio);
}

/**
 * Launch an output bin.
 *
 * @param packer  The packer which owns the bin
 * @param bin     The output bin to launch
 **/
static void launch_compressed_write(struct packer *packer,
				    struct output_bin *bin)
{
	if (is_read_only(get_vdo_from_allocating_vio(bin->writer)->read_only_notifier)) {
		finish_output_bin(packer, bin);
		return;
	}

	struct vio *vio = allocating_vio_as_vio(bin->writer);
	reset_completion(vio_as_completion(vio));
	vio->callback = complete_output_bin;
	vio->priority = VIO_PRIORITY_COMPRESSED_DATA;
	allocate_data_block(bin->writer, packer->selector,
			    VIO_COMPRESSED_WRITE_LOCK,
			    continue_after_allocation);
}

/**
 * Consume from the pending queue the next batch of vios that can be packed
 * together in a single compressed block. vios that have been mooted since
 * being placed in the pending queue will not be returned.
 *
 * @param packer  The packer
 * @param batch   The counted array to fill with the next batch of vios
 **/
static void get_next_batch(struct packer *packer, struct output_batch *batch)
{
	block_size_t space_remaining = packer->bin_data_size;
	batch->slots_used = 0;

	struct data_vio *data_vio;
	while ((data_vio =
		waiter_as_data_vio(get_first_waiter(&packer->batched_data_vios)))
	       != NULL) {
		// If there's not enough space for the next data_vio, the batch
		// is done.
		if ((data_vio->compression.size > space_remaining) ||
		    (batch->slots_used == packer->max_slots)) {
			break;
		}

		// Remove the next data_vio from the queue and put it in the
		// output batch.
		dequeue_next_waiter(&packer->batched_data_vios);
		batch->slots[batch->slots_used++] = data_vio;
		space_remaining -= data_vio->compression.size;
	}
}

/**
 * Pack the next batch of compressed vios from the batched queue into an
 * output bin and write the output bin.
 *
 * @param packer  The packer
 * @param output  The output bin to fill
 *
 * @return <code>true</code> if a write was issued for the output bin
 **/
static bool __must_check
write_next_batch(struct packer *packer, struct output_bin *output)
{
	struct output_batch batch;
	get_next_batch(packer, &batch);

	if (batch.slots_used == 0) {
		// The pending queue must now be empty (there may have been
		// mooted vios).
		return false;
	}

	// If the batch contains only a single vio, then we save nothing by
	// saving the compressed form. Continue processing the single vio in the
	// batch.
	if (batch.slots_used == 1) {
		abort_packing(batch.slots[0]);
		return false;
	}

	reset_compressed_block_header(&output->block->header);

	size_t space_used = 0;
	slot_number_t slot;
	for (slot = 0; slot < batch.slots_used; slot++) {
		struct data_vio *data_vio = batch.slots[slot];
		data_vio->compression.slot = slot;
		put_compressed_block_fragment(output->block, slot, space_used,
					      data_vio->compression.data,
					      data_vio->compression.size);
		space_used += data_vio->compression.size;

		int result = enqueue_data_vio(&output->outgoing, data_vio,
					      THIS_LOCATION(NULL));
		if (result != VDO_SUCCESS) {
			abort_packing(data_vio);
			continue;
		}

		output->slots_used += 1;
	}

	launch_compressed_write(packer, output);
	return true;
}

/**
 * Put a data_vio in a specific input_bin in which it will definitely fit.
 *
 * @param bin       The bin in which to put the data_vio
 * @param data_vio  The data_vio to add
 **/
static void add_to_input_bin(struct input_bin *bin, struct data_vio *data_vio)
{
	data_vio->compression.bin = bin;
	data_vio->compression.slot = bin->slots_used;
	bin->incoming[bin->slots_used++] = data_vio;
}

/**
 * Start a new batch of vios in an input_bin, moving the existing batch, if
 * any, to the queue of pending batched vios in the packer.
 *
 * @param packer  The packer
 * @param bin     The bin to prepare
 **/
static void start_new_batch(struct packer *packer, struct input_bin *bin)
{
	// Move all the data_vios in the current batch to the batched queue so
	// they will get packed into the next free output bin.
	slot_number_t slot;
	for (slot = 0; slot < bin->slots_used; slot++) {
		struct data_vio *data_vio = bin->incoming[slot];
		data_vio->compression.bin = NULL;

		if (!may_write_compressed_data_vio(data_vio)) {
			/*
			 * Compression of this data_vio was canceled while it
			 * was waiting; put it in the canceled bin so it can be
			 * rendezvous with the canceling data_vio.
			 */
			add_to_input_bin(packer->canceled_bin, data_vio);
			continue;
		}

		int result = enqueue_data_vio(&packer->batched_data_vios,
					      data_vio,
					      THIS_LOCATION(NULL));
		if (result != VDO_SUCCESS) {
			// Impossible but we're required to check the result
			// from enqueue.
			abort_packing(data_vio);
		}
	}

	// The bin is now empty.
	bin->slots_used = 0;
	bin->free_space = packer->bin_data_size;
}

/**
 * Add a data_vio to a bin's incoming queue, handle logical space change, and
 * call physical space processor.
 *
 * @param packer    The packer
 * @param bin       The bin to which to add the the data_vio
 * @param data_vio  The data_vio to add to the bin's queue
 **/
static void add_data_vio_to_input_bin(struct packer *packer,
				      struct input_bin *bin,
				      struct data_vio *data_vio)
{
	// If the selected bin doesn't have room, start a new batch to make
	// room.
	if (bin->free_space < data_vio->compression.size) {
		start_new_batch(packer, bin);
	}

	add_to_input_bin(bin, data_vio);
	bin->free_space -= data_vio->compression.size;

	// If we happen to exactly fill the bin, start a new input batch.
	if ((bin->slots_used == packer->max_slots) || (bin->free_space == 0)) {
		start_new_batch(packer, bin);
	}

	// Now that we've finished changing the free space, restore the sort
	// order.
	insert_in_sorted_list(packer, bin);
}

/**
 * Move data_vios in pending batches from the batched_data_vios to all free
 * output bins, issuing writes for the output bins as they are packed. This
 * will loop until either the pending queue is drained or all output bins are
 * busy writing a compressed block.
 *
 * @param packer  The packer
 **/
static void write_pending_batches(struct packer *packer)
{
	if (packer->writing_batches) {
		/*
		 * We've attempted to re-enter this function recursively due to
		 * completion handling, which can lead to kernel stack overflow
		 * as in VDO-1340. It's perfectly safe to break the recursion
		 * and do nothing since we know any pending batches will
		 * eventually be handled by the earlier call.
		 */
		return;
	}

	// Record that we are in this function for the above check. IMPORTANT:
	// never return from this function without clearing this flag.
	packer->writing_batches = true;

	struct output_bin *output;
	while (has_waiters(&packer->batched_data_vios)
	       && ((output = pop_output_bin(packer)) != NULL)) {
		if (!write_next_batch(packer, output)) {
			// We didn't use the output bin to write, so push it
			// back on the stack.
			push_output_bin(packer, output);
		}
	}

	packer->writing_batches = false;
}

/**
 * Select the input bin that should be used to pack the compressed data in a
 * data_vio with other data_vios.
 *
 * @param packer    The packer
 * @param data_vio  The data_vio
 **/
static struct input_bin * __must_check
select_input_bin(struct packer *packer, struct data_vio *data_vio)
{
	// First best fit: select the bin with the least free space that has
	// enough room for the compressed data in the data_vio.
	struct input_bin *fullest_bin = get_fullest_bin(packer);
	struct input_bin *bin;
	for (bin = fullest_bin; bin != NULL; bin = next_bin(packer, bin)) {
		if (bin->free_space >= data_vio->compression.size) {
			return bin;
		}
	}

	/*
	 * None of the bins have enough space for the data_vio. We're not
	 * allowed to create new bins, so we have to overflow one of the
	 * existing bins. It's pretty intuitive to select the fullest bin, since
	 * that "wastes" the least amount of free space in the compressed block.
	 * But if the space currently used in the fullest bin is smaller than
	 * the compressed size of the incoming block, it seems wrong to force
	 * that bin to write when giving up on compressing the incoming data_vio
	 * would likewise "waste" the the least amount of free space.
	 */
	if (data_vio->compression.size
	    >= (packer->bin_data_size - fullest_bin->free_space)) {
		return NULL;
	}

	// The fullest bin doesn't have room, but writing it out and starting a
	// new batch with the incoming data_vio will increase the packer's free
	// space.
	return fullest_bin;
}

/**********************************************************************/
void attempt_packing(struct data_vio *data_vio)
{
	struct packer *packer = get_packer_from_data_vio(data_vio);
	assert_on_packer_thread(packer, __func__);

	struct vio_compression_state state = get_compression_state(data_vio);
	int result =
		ASSERT((state.status == VIO_COMPRESSING),
		       "attempt to pack data_vio not ready for packing, state: %u",
		       state.status);
	if (result != VDO_SUCCESS) {
		return;
	}

	/*
	 * Increment whether or not this data_vio will be packed or not since
	 * abort_packing() always decrements the counter.
	 */
	WRITE_ONCE(packer->statistics.compressed_fragments_in_packer,
		   packer->statistics.compressed_fragments_in_packer + 1);

	// If packing of this data_vio is disallowed for administrative reasons,
	// give up before making any state changes.
	if (!is_normal(&packer->state) ||
	    (data_vio->flush_generation < packer->flush_generation)) {
		abort_packing(data_vio);
		return;
	}

	/*
	 * The check of may_block_in_packer() here will set the data_vio's
	 * compression state to VIO_PACKING if the data_vio is allowed to be
	 * compressed (if it has already been canceled, we'll fall out here).
	 * Once the data_vio is in the VIO_PACKING state, it must be guaranteed
	 * to be put in an input bin before any more requests can be processed
	 * by the packer thread. Otherwise, a canceling data_vio could attempt
	 * to remove the canceled data_vio from the packer and fail to
	 * rendezvous with it (VDO-2809). We must also make sure that we will
	 * actually bin the data_vio and not give up on it as being larger than
	 * the space used in the fullest bin. Hence we must call
	 * select_input_bin() before calling may_block_in_packer() (VDO-2826).
	 */
	struct input_bin *bin = select_input_bin(packer, data_vio);
	if ((bin == NULL) || !may_block_in_packer(data_vio)) {
		abort_packing(data_vio);
		return;
	}

	add_data_vio_to_input_bin(packer, bin, data_vio);
	write_pending_batches(packer);
}

/**
 * Force a pending write for all non-empty bins on behalf of a flush or
 * suspend.
 *
 * @param packer  The packer being flushed
 **/
static void write_all_non_empty_bins(struct packer *packer)
{
	struct input_bin *bin;
	for (bin = get_fullest_bin(packer); bin != NULL;
	     bin = next_bin(packer, bin)) {
		start_new_batch(packer, bin);
		// We don't need to re-sort the bin here since this loop will
		// make every bin have the same amount of free space, so every
		// ordering is sorted.
	}

	write_pending_batches(packer);
}

/**********************************************************************/
void flush_packer(struct packer *packer)
{
	assert_on_packer_thread(packer, __func__);
	if (is_normal(&packer->state)) {
		write_all_non_empty_bins(packer);
	}
}

/*
 * This method is only exposed for unit tests and should not normally be called
 * directly; use remove_lock_holder_from_packer() instead.
 */
void remove_from_packer(struct data_vio *data_vio)
{
	struct input_bin *bin = data_vio->compression.bin;
	ASSERT_LOG_ONLY((bin != NULL), "data_vio in packer has an input bin");

	slot_number_t slot = data_vio->compression.slot;
	bin->slots_used--;
	if (slot < bin->slots_used) {
		bin->incoming[slot] = bin->incoming[bin->slots_used];
		bin->incoming[slot]->compression.slot = slot;
	}

	data_vio->compression.bin = NULL;
	data_vio->compression.slot = 0;

	struct packer *packer = get_packer_from_data_vio(data_vio);
	if (bin != packer->canceled_bin) {
		bin->free_space += data_vio->compression.size;
		insert_in_sorted_list(packer, bin);
	}

	abort_packing(data_vio);
	check_for_drain_complete(packer);
}

/**********************************************************************/
void remove_lock_holder_from_packer(struct vdo_completion *completion)
{
	struct data_vio *data_vio = as_data_vio(completion);
	assert_in_packer_zone(data_vio);

	struct data_vio *lock_holder = data_vio->compression.lock_holder;
	data_vio->compression.lock_holder = NULL;
	remove_from_packer(lock_holder);
}

/**********************************************************************/
void increment_packer_flush_generation(struct packer *packer)
{
	assert_on_packer_thread(packer, __func__);
	packer->flush_generation++;
	flush_packer(packer);
}

/**
 * Initiate a drain.
 *
 * Implements AdminInitiator.
 **/
static void initiate_drain(struct admin_state *state)
{
	struct packer *packer = container_of(state, struct packer, state);
	write_all_non_empty_bins(packer);
	check_for_drain_complete(packer);
}

/**********************************************************************/
void drain_packer(struct packer *packer, struct vdo_completion *completion)
{
	assert_on_packer_thread(packer, __func__);
	start_draining(&packer->state, ADMIN_STATE_SUSPENDING, completion,
		       initiate_drain);
}

/**********************************************************************/
void resume_packer(struct packer *packer, struct vdo_completion *parent)
{
	assert_on_packer_thread(packer, __func__);
	finish_completion(parent, resume_if_quiescent(&packer->state));
}

/**********************************************************************/
void reset_slot_count(struct packer *packer, compressed_fragment_count_t slots)
{
	if (slots > MAX_COMPRESSION_SLOTS) {
		return;
	}

	packer->max_slots = slots;
}

/**********************************************************************/
static void dump_input_bin(const struct input_bin *bin, bool canceled)
{
	if (bin->slots_used == 0) {
		// Don't dump empty input bins.
		return;
	}

	log_info("    %sBin slots_used=%u free_space=%zu",
		 (canceled ? "Canceled" : "Input"), bin->slots_used,
		 bin->free_space);

	// XXX dump vios in bin->incoming? The vios should have been dumped from
	// the vio pool. Maybe just dump their addresses so it's clear they're
	// here?
}

/**********************************************************************/
static void dump_output_bin(const struct output_bin *bin)
{
	size_t count = count_waiters(&bin->outgoing);
	if (bin->slots_used == 0) {
		// Don't dump empty output bins.
		return;
	}

	log_info("    struct output_bin contains %zu outgoing waiters", count);

	// XXX dump vios in bin->outgoing? The vios should have been dumped from
	// the vio pool. Maybe just dump their addresses so it's clear they're
	// here?

	// XXX dump writer vio?
}

/**********************************************************************/
void dump_packer(const struct packer *packer)
{
	log_info("packer");
	log_info("  flushGeneration=%llu state %s writing_batches=%s",
		 packer->flush_generation,
		 get_admin_state_name(&packer->state),
		 bool_to_string(packer->writing_batches));

	log_info("  input_bin_count=%llu", packer->size);
	struct input_bin *input;
	for (input = get_fullest_bin(packer); input != NULL;
	     input = next_bin(packer, input)) {
		dump_input_bin(input, false);
	}

	dump_input_bin(packer->canceled_bin, true);

	log_info("  output_bin_count=%zu idle_output_bin_count=%zu",
		 packer->output_bin_count, packer->idle_output_bin_count);
	struct list_head *entry;
	list_for_each(entry, &packer->output_bins) {
		struct output_bin *output
			= container_of(entry, struct output_bin, list);
		dump_output_bin(output);
	}
}
