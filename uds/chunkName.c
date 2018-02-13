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
 * $Id: //eng/uds-releases/flanders/src/uds/chunkName.c#2 $
 */

#include "hashUtils.h"
#include "sha256.h"
#include "stringUtils.h"
#include "uds.h"

/**********************************************************************/
UdsChunkName udsCalculateSHA256ChunkName(const void *data, size_t size)
{
  UdsChunkName ret;
  if ((int) UDS_CHUNK_NAME_SIZE == (int) SHA256_HASH_LEN) {
    sha256(data, size, ret.name);
  } else {
    unsigned char hash[SHA256_HASH_LEN];
    sha256(data, size, hash);
    memcpy(ret.name, hash, UDS_CHUNK_NAME_SIZE);
  }
  return ret;
}

/**********************************************************************/
UdsChunkName udsCalculateMurmur3ChunkName(const void *data, size_t size)
{
  return murmurGenerator(data, size);
}

/**********************************************************************/
bool udsEqualChunkName(const UdsChunkName *name0, const UdsChunkName *name1)
{
  return memcmp(name0->name, name1->name, UDS_CHUNK_NAME_SIZE) == 0;
}
