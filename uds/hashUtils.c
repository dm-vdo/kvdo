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
 * $Id: //eng/uds-releases/gloria/src/uds/hashUtils.c#1 $
 */

#include "hashUtils.h"

#include "errors.h"
#include "logger.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds.h"

/**
 * Convert a byte string to the hex representation.
 *
 * @param data          binary data to convert
 * @param dataLen       length of binary data
 * @param hex           target to write hex string into
 * @param hexLen        capacity of target string
 *
 * @return              UDS_SUCCESS,
 *                      or UDS_INVALID_ARGUMENT if hexLen
 *                      is too short.
 **/
static int dataToHex(const unsigned char *data, size_t dataLen,
                     char *hex, size_t hexLen)
{
  if (hexLen < 2 * dataLen + 1) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "hex data incorrect size");
  }
  for (size_t i = 0; i < dataLen; ++i) {
    int rc = fixedSprintf(__func__, &hex[2 * i], hexLen - (2 * i),
                          UDS_INVALID_ARGUMENT, "%02X", data[i]);

    if (rc != UDS_SUCCESS) {
      return rc;
    }
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int chunkNameToHex(const UdsChunkName *chunkName,
                   char *hexData, size_t hexDataLen)
{
  return dataToHex(chunkName->name, UDS_CHUNK_NAME_SIZE,
                   hexData, hexDataLen);
}

/**********************************************************************/
int chunkDataToHex(const UdsChunkData *chunkData,
                   char *hexData, size_t hexDataLen)
{
  return dataToHex(chunkData->data, UDS_MAX_BLOCK_DATA_SIZE,
                   hexData, hexDataLen);
}

/**********************************************************************/
unsigned int computeBits(unsigned int maxValue)
{
  // __builtin_clz() counts leading (high-order) zero bits, so if
  // we ever need this to be fast, under GCC we can do:
  // return ((maxValue == 0) ? 0 : (32 - __builtin_clz(maxValue)));

  unsigned int bits = 0;
  while (maxValue > 0) {
    maxValue >>= 1;
    bits++;
  }
  return bits;
}

/**********************************************************************/
void hashUtilsCompileTimeAssertions(void)
{
  STATIC_ASSERT((UDS_CHUNK_NAME_SIZE % sizeof(uint64_t)) == 0);
  STATIC_ASSERT((UDS_CHUNK_NAME_SIZE == 32) || (UDS_CHUNK_NAME_SIZE == 16));
}
