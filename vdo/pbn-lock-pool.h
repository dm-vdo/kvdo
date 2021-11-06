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

#ifndef PBN_LOCK_POOL_H
#define PBN_LOCK_POOL_H

#include "pbn-lock.h"
#include "types.h"

struct pbn_lock_pool;

int __must_check
make_vdo_pbn_lock_pool(size_t capacity, struct pbn_lock_pool **pool_ptr);

void free_vdo_pbn_lock_pool(struct pbn_lock_pool *pool);

int __must_check
borrow_vdo_pbn_lock_from_pool(struct pbn_lock_pool *pool,
			      enum pbn_lock_type type,
			      struct pbn_lock **lock_ptr);

void return_vdo_pbn_lock_to_pool(struct pbn_lock_pool *pool,
				 struct pbn_lock *lock);

#endif /* PBN_LOCK_POOL_H */
