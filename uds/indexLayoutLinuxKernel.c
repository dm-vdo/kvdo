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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/indexLayoutLinuxKernel.c#1 $
 */

#include "indexLayout.h"
#include "indexLayoutParser.h"
#include "linuxIORegion.h"
#include "logger.h"
#include "memoryAlloc.h"

/*****************************************************************************/
int makeIndexLayout(const char              *name,
                    bool                     newLayout,
                    const UdsConfiguration   config,
                    IndexLayout            **layoutPtr)
{
  char     *dev    = NULL;
  uint64_t  offset = 0;
  uint64_t  size   = 0;

  LayoutParameter parameterTable[] = {
    { "dev",    LP_STRING | LP_DEFAULT, { .str = &dev    } },
    { "offset", LP_UINT64,              { .num = &offset } },
    { "size",   LP_UINT64,              { .num = &size   } },
  };
  size_t numParameters = sizeof(parameterTable) / sizeof(*parameterTable);

  char *params = NULL;
  int result = duplicateString(name, "makeIndexLayout parameters", &params);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // note dev will be set to memory owned by params
  result = parseLayoutString(params, parameterTable, numParameters);
  if (result != UDS_SUCCESS) {
    FREE(params);
    return result;
  }

  if (size == 0) {
    if (config == NULL) {
      size = 1L << 40;
    } else {
      result = udsComputeIndexSize(config, 0, &size);
      if (result != UDS_SUCCESS) {
        FREE(params);
        return result;
      }
    }
  }

  IOFactory *factory = NULL;
  result = makeIOFactory(dev, &factory);
  FREE(params);
  if (result != UDS_SUCCESS) {
    return result;
  }
  IORegion *region = NULL;
  result = makeIORegion(factory, offset, size, &region);
  int freeResult = putIOFactory(factory);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (freeResult != UDS_SUCCESS) {
    closeIORegion(&region);
    return freeResult;
  }

  if (newLayout) {
    result = makeIndexLayoutForCreate(region, offset, size, config, layoutPtr);
  } else {
    result = makeIndexLayoutForLoad(region, offset, layoutPtr);
  }
  if (result != UDS_SUCCESS) {
    closeIORegion(&region);
  }
  return result;
}
