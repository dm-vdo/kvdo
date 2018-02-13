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
 * $Id: //eng/uds-releases/flanders/src/uds/sha256.h#2 $
 */

#ifndef SHA256_H
#define SHA256_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SHA256_HASH_LEN = 32 };

void sha256(const void *data, size_t len, unsigned char ret_hash[SHA256_HASH_LEN]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
