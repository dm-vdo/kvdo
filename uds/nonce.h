/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/nonce.h#1 $
 */

#ifndef NONCE_H
#define NONCE_H

#include "typeDefs.h"

/**
 * Create unique data for the master nonce, using system-specific
 * methods such as the current time and a random number.
 *
 * @param buffer        A buffer of length specified next.
 * @param length        Length of the buffer.
 *
 * @return the amount of the buffer that has been filled with unique data
 **/
size_t createUniqueNonceData(byte *buffer, size_t length);

/**
 * Generate a master nonce, using the specified data.
 *
 * @param data          Some arbitrary information.
 * @param len           The length of the information.
 *
 * @return a number which will be fairly unique
 **/
uint64_t generateMasterNonce(const void *data, size_t len);

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
uint64_t generateSecondaryNonce(uint64_t    nonce,
                                const void *data,
                                size_t      len);

#endif // NONCE_H
