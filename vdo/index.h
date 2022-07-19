/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef INDEX_H
#define INDEX_H

#include "index-layout.h"
#include "index-session.h"
#include "open-chapter.h"
#include "volume.h"
#include "volume-index-ops.h"


typedef void (*index_callback_t)(struct uds_request *request);

struct index_zone {
	struct uds_index *index;
	struct open_chapter_zone *open_chapter;
	struct open_chapter_zone *writing_chapter;
	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;
	unsigned int id;
};

struct uds_index {
	bool has_saved_open_chapter;
	bool need_to_save;
	struct index_load_context *load_context;
	struct index_layout *layout;
	struct volume_index *volume_index;
	struct volume *volume;
	unsigned int zone_count;
	struct index_zone **zones;

	uint64_t oldest_virtual_chapter;
	uint64_t newest_virtual_chapter;

	uint64_t last_save;
	uint64_t prev_save;
	struct chapter_writer *chapter_writer;

	index_callback_t callback;
	struct uds_request_queue *triage_queue;
	struct uds_request_queue *zone_queues[];
};

enum request_stage {
	STAGE_TRIAGE,
	STAGE_INDEX,
	STAGE_MESSAGE,
};

int __must_check make_index(struct configuration *config,
			    enum uds_open_index_type open_type,
			    struct index_load_context *load_context,
			    index_callback_t callback,
			    struct uds_index **new_index);

int __must_check save_index(struct uds_index *index);

void free_index(struct uds_index *index);

int __must_check replace_index_storage(struct uds_index *index,
				       const char *path);

void get_index_stats(struct uds_index *index,
		     struct uds_index_stats *counters);

void enqueue_request(struct uds_request *request, enum request_stage stage);

void wait_for_idle_index(struct uds_index *index);

#endif /* INDEX_H */
