// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "super-block.h"

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "completion.h"
#include "kernel-types.h"
#include "status-codes.h"
#include "super-block-codec.h"
#include "types.h"
#include "vio.h"

struct vdo_super_block {
	/** The parent for asynchronous load and save operations */
	struct vdo_completion *parent;
	/** The vio for reading and writing the super block to disk */
	struct vio *vio;
	/** The super block codec */
	struct super_block_codec codec;
	/** Whether this super block may not be written */
	bool unwriteable;
};

/**
 * Allocate a super block. Callers must free the allocated super block even
 * on error.
 *
 * @param [in]  vdo              The vdo containing the super block on disk
 * @param [out] super_block_ptr  A pointer to hold the new super block
 *
 * @return VDO_SUCCESS or an error
 **/
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
 * Free a super block.
 *
 * @param super_block  The super block to free
 **/
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
 * Finish the parent of a super block load or save operation. This
 * callback is registered in vdo_save_super_block() and
 * vdo_load_super_block().
 *
 * @param completion  The super block vio
 **/
static void finish_super_block_parent(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;

	super_block->parent = NULL;
	vdo_finish_completion(parent, completion->result);
}

/**
 * Log a super block save error. This error handler is registered in
 * vdo_save_super_block().
 *
 * @param completion  The super block vio
 **/
static void handle_save_error(struct vdo_completion *completion)
{
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
	((struct vdo_super_block *) completion->parent)->unwriteable = true;
	completion->callback(completion);
}

/**
 * Save a super block.
 *
 * @param super_block         The super block to save
 * @param super_block_offset  The location at which to write the super block
 * @param parent              The object to notify when the save is complete
 **/
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
	launch_write_metadata_vio_with_flush(super_block->vio,
					     super_block_offset,
					     finish_super_block_parent,
					     handle_save_error,
					     true, true);
}

/**
 * Continue after loading the super block. This callback is registered
 * in vdo_load_super_block().
 *
 * @param completion  The super block vio
 **/
static void finish_reading_super_block(struct vdo_completion *completion)
{
	struct vdo_super_block *super_block = completion->parent;
	struct vdo_completion *parent = super_block->parent;

	super_block->parent = NULL;
	vdo_finish_completion(parent,
			      vdo_decode_super_block(&super_block->codec));
}

/**
 * Allocate a super block and read its contents from storage. If a load error
 * occurs before the super block's own completion can be allocated, the parent
 * will be finished with the error.
 *
 * @param [in]  vdo                 The vdo containing the super block on disk
 * @param [in]  parent              The completion to finish after loading the
 *                                  super block
 * @param [in]  super_block_offset  The location from which to read the super
 *                                  block
 * @param [out] super_block_ptr     A pointer to hold the super block
 **/
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
	super_block->vio->completion.callback_thread_id =
		parent->callback_thread_id;
	launch_read_metadata_vio(super_block->vio,
				 super_block_offset,
				 finish_reading_super_block,
				 finish_super_block_parent);
}

/**
 * Get the super block codec from a super block.
 *
 * @param super_block  The super block from which to get the component data
 *
 * @return the codec
 **/
struct super_block_codec *
vdo_get_super_block_codec(struct vdo_super_block *super_block)
{
	return &super_block->codec;
}
