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
 * $Id: //eng/uds-releases/homer/kernelLinux/uds/indexLayoutLinuxKernel.c#1 $
 */

#include "indexLayout.h"
#include "indexLayoutParser.h"
#include "linuxIORegion.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "singleFileLayout.h"

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

  IORegion *region = NULL;
  if (dev != NULL) {
    result = openLinuxRegion(dev, offset + size, &region);
  } else {
    FREE(params);
    return logErrorWithStringError(UDS_INDEX_NAME_REQUIRED,
                                   "no index specified");
  }
  FREE(params);
  if (result != UDS_SUCCESS) {
    closeIORegion(&region);
    return result;
  }


  if (newLayout) {
    result = createSingleFileLayout(region, offset, size, config, layoutPtr);
  } else {
    result = loadSingleFileLayout(region, offset, layoutPtr);
  }
  if (result != UDS_SUCCESS) {
    closeIORegion(&region);
  }
  return result;
}
