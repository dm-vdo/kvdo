/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef IO_SUBMITTER_H
#define IO_SUBMITTER_H

#include <linux/bio.h>

#include "completion.h"
#include "kernel-types.h"
#include "vio.h"

int vdo_make_io_submitter(unsigned int thread_count,
			  unsigned int rotation_interval,
			  unsigned int max_requests_active,
			  struct vdo *vdo,
			  struct io_submitter **io_submitter);

void vdo_cleanup_io_submitter(struct io_submitter *io_submitter);

void vdo_free_io_submitter(struct io_submitter *io_submitter);

void process_vio_io(struct vdo_completion *completion);

void submit_data_vio_io(struct data_vio *data_vio);

void vdo_submit_metadata_io(struct vio *vio,
			    physical_block_number_t physical,
			    bio_end_io_t callback,
			    vdo_action *error_handler,
			    unsigned int operation,
			    char *data);

static inline void submit_metadata_vio(struct vio *vio,
				       physical_block_number_t physical,
				       bio_end_io_t callback,
				       vdo_action *error_handler,
				       unsigned int operation)
{
	vdo_submit_metadata_io(vio,
			       physical,
			       callback,
			       error_handler,
			       operation,
			       vio->data);
}

static inline void submit_flush_vio(struct vio *vio,
				    bio_end_io_t callback,
				    vdo_action *error_handler)
{
	/* FIXME: Can we just use REQ_OP_FLUSH? */
	vdo_submit_metadata_io(vio,
			       0,
			       callback,
			       error_handler,
			       REQ_OP_WRITE | REQ_PREFLUSH,
			       NULL);
}

#endif /* IO_SUBMITTER_H */
