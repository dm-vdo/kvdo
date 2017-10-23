/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/namespaceHash.h#2 $
 */

#ifndef NAMESPACE_HASH_H
#define NAMESPACE_HASH_H

#include "compiler.h"
#include "featureDefs.h"
#include "uds.h"

#if NAMESPACES
/**
 * The hash of a namespace.
 **/
typedef struct namespaceHash {
  unsigned char hash[UDS_CHUNK_NAME_SIZE];
} NamespaceHash;

/**
 * XOR a chunk name with a namespace hash. The name will be modified in
 * place.
 *
 * @param name          The name to which to apply the namespace
 * @param namespaceHash The namespace hash to apply to the chunk name
 **/
static INLINE void xorNamespace(UdsChunkName        *name,
                                const NamespaceHash *namespaceHash)
{
  uint64_t *chunk = (uint64_t *) name->name;
  const uint64_t *space = (const uint64_t *) namespaceHash->hash;
  enum { SIZE = UDS_CHUNK_NAME_SIZE / sizeof(uint64_t) };
  for (unsigned int i = 0; i < SIZE; i++) {
    chunk[i] ^= space[i];
  }
}
#endif /* NAMESPACES */
#endif /* NAMESPACE_HASH_H */
