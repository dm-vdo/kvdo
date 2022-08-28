/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PBN_LOCK_POOL_H
#define PBN_LOCK_POOL_H

#include "pbn-lock.h"
#include "types.h"

struct pbn_lock_pool;

int __must_check
vdo_make_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr);

void vdo_free_pbn_lock_pool(struct pbn_lock_pool *pool);

int __must_check
vdo_borrow_pbn_lock_from_pool(struct pbn_lock_pool *pool,
			      enum pbn_lock_type type,
			      struct pbn_lock **lock_ptr);

void vdo_return_pbn_lock_to_pool(struct pbn_lock_pool *pool,
				 struct pbn_lock *lock);

#endif /* PBN_LOCK_POOL_H */
