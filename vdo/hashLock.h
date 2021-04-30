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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/hashLock.h#8 $
 */

#ifndef HASH_LOCK_H
#define HASH_LOCK_H

#include "types.h"

/**
 * Get the PBN lock on the duplicate data location for a data_vio from the
 * hash_lock the data_vio holds (if there is one).
 *
 * @param data_vio  The data_vio to query
 *
 * @return The PBN lock on the data_vio's duplicate location
 **/
struct pbn_lock * __must_check
get_vdo_duplicate_lock(struct data_vio *data_vio);

/**
 * Acquire or share a lock on the hash (chunk name) of the data in a data_vio,
 * updating the data_vio to reference the lock. This must only be called in the
 * correct thread for the zone. In the unlikely case of a hash collision, this
 * function will succeed, but the data_vio will not get a lock reference.
 *
 * @param data_vio  The data_vio acquiring a lock on its chunk name
 **/
int __must_check acquire_vdo_hash_lock(struct data_vio *data_vio);

/**
 * Asynchronously process a data_vio that has just acquired its reference to a
 * hash lock. This may place the data_vio on a wait queue, or it may use the
 * data_vio to perform operations on the lock's behalf.
 *
 * @param data_vio  The data_vio that has just acquired a lock on its chunk
 *                  name
 **/
void enter_vdo_hash_lock(struct data_vio *data_vio);

/**
 * Asynchronously continue processing a data_vio in its hash lock after it has
 * finished writing, compressing, or deduplicating, so it can share the result
 * with any data_vios waiting in the hash lock, or update the UDS index, or
 * simply release its share of the lock. This must only be called in the
 * correct thread for the hash zone.
 *
 * @param data_vio  The data_vio to continue processing in its hash lock
 **/
void continue_vdo_hash_lock(struct data_vio *data_vio);

/**
 * Re-enter the hash lock after encountering an error, to clean up the hash
 * lock.
 *
 * @param data_vio  The data_vio with an error
 **/
void continue_vdo_hash_lock_on_error(struct data_vio *data_vio);

/**
 * Release a data_vio's share of a hash lock, if held, and null out the
 * data_vio's reference to it. This must only be called in the correct thread
 * for the hash zone.
 *
 * If the data_vio is the only one holding the lock, this also releases any
 * resources or locks used by the hash lock (such as a PBN read lock on a
 * block containing data with the same hash) and returns the lock to the hash
 * zone's lock pool.
 *
 * @param data_vio  The data_vio releasing its hash lock
 **/
void release_vdo_hash_lock(struct data_vio *data_vio);

/**
 * Make a data_vio's hash lock a shared holder of the PBN lock on the
 * compressed block to which its data was just written. If the lock is still a
 * write lock (as it will be for the first share), it will be converted to a
 * read lock. This also reserves a reference count increment for the data_vio.
 *
 * @param data_vio  The data_vio which was just compressed
 * @param pbn_lock  The PBN lock on the compressed block
 **/
void share_compressed_vdo_write_lock(struct data_vio *data_vio,
				     struct pbn_lock *pbn_lock);

#endif // HASH_LOCK_H
