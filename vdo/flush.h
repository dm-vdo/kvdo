/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef FLUSH_H
#define FLUSH_H

#include "bio.h"
#include "completion.h"
#include "kernel-types.h"
#include "types.h"
#include "wait-queue.h"
#include "workQueue.h"

/*
 * A marker for tracking which journal entries are affected by a flush request.
 */
struct vdo_flush {
	/* The completion for enqueueing this flush request. */
	struct vdo_completion completion;
	/* The flush bios covered by this request */
	struct bio_list bios;
	/* The wait queue entry for this flush */
	struct waiter waiter;
	/* Which flush this struct represents */
	sequence_number_t flush_generation;
};

int __must_check vdo_make_flusher(struct vdo *vdo);

void vdo_free_flusher(struct flusher *flusher);

thread_id_t __must_check vdo_get_flusher_thread_id(struct flusher *flusher);

void vdo_complete_flushes(struct flusher *flusher);

void vdo_dump_flusher(const struct flusher *flusher);

void vdo_launch_flush(struct vdo *vdo, struct bio *bio);

void vdo_drain_flusher(struct flusher *flusher,
		       struct vdo_completion *completion);

void vdo_resume_flusher(struct flusher *flusher,
			struct vdo_completion *parent);

#endif /* FLUSH_H */
