/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_LAYOUT_H
#define INDEX_LAYOUT_H

#include "buffer.h"
#include "config.h"
#include "io-factory.h"
#include "uds.h"

struct index_layout;

int __must_check make_uds_index_layout(struct configuration *config,
				       bool new_layout,
				       struct index_layout **layout_ptr);

void free_uds_index_layout(struct index_layout *layout);

int __must_check replace_index_layout_storage(struct index_layout *layout,
					      const char *name);

int __must_check load_index_state(struct index_layout *layout,
				  struct uds_index *index);

int __must_check save_index_state(struct index_layout *layout,
				  struct uds_index *index);

int discard_index_state_data(struct index_layout *layout);

int __must_check discard_open_chapter(struct index_layout *layout);

uint64_t __must_check get_uds_volume_nonce(struct index_layout *layout);

int __must_check open_uds_volume_bufio(struct index_layout *layout,
				       size_t block_size,
				       unsigned int reserved_buffers,
				       struct dm_bufio_client **client_ptr);

#endif /* INDEX_LAYOUT_H */
