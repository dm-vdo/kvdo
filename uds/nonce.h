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

#ifndef NONCE_H
#define NONCE_H

#include "typeDefs.h"

enum { NONCE_INFO_SIZE = 32 };

/**
 * Create NONCE_INFO_SIZE (32) bytes of unique data for generating a
 * nonce, using the current time and a pseudorandom number.
 *
 * @param buffer        Where to put the data
 **/
void create_unique_nonce_data(byte *buffer);

/**
 * Generate a primary nonce, using the specified data.
 *
 * @param data          Some arbitrary information.
 * @param len           The length of the information.
 *
 * @return a number which will be fairly unique
 **/
uint64_t generate_primary_nonce(const void *data, size_t len);

/**
 * Deterministically generate a secondary nonce based on an existing
 * nonce and some arbitrary data. Effectively hashes the nonce and
 * the data to produce a new nonce which is deterministic.
 *
 * @param nonce         An existing nonce which is well known.
 * @param data          Some data related to the creation of this nonce.
 * @param len           The length of the data.
 *
 * @return a number which will be fairly unique and depend solely on
 *      the nonce and the data.
 **/
uint64_t
generate_secondary_nonce(uint64_t nonce, const void *data, size_t len);

#endif // NONCE_H
