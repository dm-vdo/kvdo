// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "super-block.h"

#include <linux/bio.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "completion.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "status-codes.h"
#include "super-block-codec.h"
#include "types.h"
#include "vio.h"

struct vdo_super_block {
	/* The parent for asynchronous load and save operations */
	struct vdo_completion *parent;
	/* The vio for reading and writing the super block to disk */
	struct vio *vio;
	/* The super block codec */
	struct super_block_codec codec;
	/* Whether this super block may not be written */
	bool unwriteable;
};

/**
 * allocate_super_block() - Allocate a super block.
 * @vdo: The vdo containing the super block on disk.
 * @super_block_ptr: A pointer to hold the new super block.
 *
 * Callers must free the allocated super block even on error.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
allocate_super_block(struct vdo *vdo,
		     struct vdo_super_block **super_block_ptr)
{
	struct vdo_super_block *super_block;
	char *buffer;
	int result = UDS_ALLOCATE(1, struct vdo_super_block, __func__,
				  super_block_ptr);
	if (result != UDS_SUCCESS) {
		return result;
	}

	super_block = *super_block_ptr;
	result = vdo_initialize_super_block_codec(&super_block->codec);
	if (result != UDS_SUCCESS) {
		return result;
	}

	buffer = (char *) super_block->codec.encoded_super_block;
	return create_metadata_vio(vdo,
				   VIO_TYPE_SUPER_BLOCK,
				   VIO_PRIORITY_METADATA,
				   super_block,
				   buffer,
				   &super_block->vio);
}


/**
 * vdo_free_super_block() - Free a super block.
 * @super_block: The super block to free.
 */
void vdo_free_super_block(struct vdo_super_block *super_block)
{
	if (super_block == NULL) {
		return;
	}

	free_vio(UDS_FORGET(super_block->vio));
	vdo_destroy_super_block_codec(&super_block->codec);
	UDS_FREE(super_block);
}

/**
 * finish_super_block_parent() - Finish the parent of a super block
 *                               load or save operation.
 * @completion: The super block vio.
 *
 * This callback is registered in vdo_save_super_block() and
 * vdo_load_super_block().
 */
static void finish_super_block_parent(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;

	super_block->parent = NULL;
	vdo_finish_completion(parent, completion->result);
}

/**
 * handle_save_error() - Log a super block save error.
 * @completion: The super block vio.
 *
 * This error handler is registered in vdo_save_super_block().
 */
static void handle_save_error(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;

	record_metadata_io_error(as_vio(completion));
	uds_log_error_strerror(completion->result, "super block save failed");
	/*
	 * Mark the super block as unwritable so that we won't attempt to write
	 * it again. This avoids the case where a growth attempt fails writing
	 * the super block with the new size, but the subsequent attempt to
	 * write out the read-only state succeeds. In this case, writes which
	 * happened just before the suspend would not be visible if the VDO is
	 * restarted without rebuilding, but, after a read-only rebuild, the
	 * effects of those writes would reappear.
	 */
	super_block->unwriteable = true;
	completion->callback(completion);
}

static void super_block_write_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vdo_super_block *super_block = vio_as_completion(vio)->parent;
	struct vdo_completion *parent = super_block->parent;

	continue_vio_after_io(vio,
			      finish_super_block_parent,
			      parent->callback_thread_id);
}

/**
 * vdo_save_super_block() - Save a super block.
 * @super_block: The super block to save.
 * @super_block_offset: The location at which to write the super block.
 * @parent: The object to notify when the save is complete.
 */
void vdo_save_super_block(struct vdo_super_block *super_block,
			  physical_block_number_t super_block_offset,
			  struct vdo_completion *parent)
{
	int result;

	if (super_block->unwriteable) {
		vdo_finish_completion(parent, VDO_READ_ONLY);
		return;
	}

	if (super_block->parent != NULL) {
		vdo_finish_completion(parent, VDO_COMPONENT_BUSY);
		return;
	}

	result = vdo_encode_super_block(&super_block->codec);
	if (result != VDO_SUCCESS) {
		vdo_finish_completion(parent, result);
		return;
	}

	super_block->parent = parent;
	super_block->vio->completion.callback_thread_id =
		parent->callback_thread_id;
	submit_metadata_vio(super_block->vio,
			    super_block_offset,
			    super_block_write_endio,
			    handle_save_error,
			    REQ_OP_WRITE | REQ_PREFLUSH | REQ_FUA);
}

/**
 * finish_reading_super_block() - Continue after loading the super block.
 * @completion: The super block vio.
 *
 * This callback is registered in vdo_load_super_block().
 */
static void finish_reading_super_block(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;

	super_block->parent = NULL;
	vdo_finish_completion(parent,
			      vdo_decode_super_block(&super_block->codec));
}

/**
 * handle_super_block_read_error() - Handle an error reading the super block.
 * @completion: The super block vio.
 *
 * This error handler is registered in vdo_load_super_block().
 */
static void handle_super_block_read_error(struct vdo_completion *completion)
{
	record_metadata_io_error(as_vio(completion));
	finish_reading_super_block(completion);
}

static void read_super_block_endio(struct bio *bio)
{
	struct vio *vio = bio->bi_private;
	struct vdo_super_block *super_block = vio_as_completion(vio)->parent;
	struct vdo_completion *parent = super_block->parent;

	continue_vio_after_io(vio,
			      finish_reading_super_block,
			      parent->callback_thread_id);
}

/**
 * vdo_load_super_block() - Allocate a super block and read its contents from
 *                          storage.
 * @vdo: The vdo containing the super block on disk.
 * @parent: The completion to finish after loading the super block.
 * @super_block_offset: The location from which to read the super block.
 * @super_block_ptr: A pointer to hold the super block.
 *
 * If a load error occurs before the super block's own completion can be
 * allocated, the parent will be finished with the error.
 */
void vdo_load_super_block(struct vdo *vdo,
			  struct vdo_completion *parent,
			  physical_block_number_t super_block_offset,
			  struct vdo_super_block **super_block_ptr)
{
	struct vdo_super_block *super_block = NULL;
	int result = allocate_super_block(vdo, &super_block);

	if (result != VDO_SUCCESS) {
		vdo_free_super_block(super_block);
		vdo_finish_completion(parent, result);
		return;
	}

	*super_block_ptr = super_block;

	super_block->parent = parent;
	submit_metadata_vio(super_block->vio,
			    super_block_offset,
			    read_super_block_endio,
			    handle_super_block_read_error,
			    REQ_OP_READ);
}

/**
 * vdo_get_super_block_codec() - Get the super block codec from a super block.
 * @super_block: The super block from which to get the component data.
 *
 * Return: The codec.
 */
struct super_block_codec *
vdo_get_super_block_codec(struct vdo_super_block *super_block)
{
	return &super_block->codec;
}
