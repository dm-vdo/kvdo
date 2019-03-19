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
 * $Id: //eng/uds-releases/gloria/src/uds/indexLayoutParser.c#1 $
 */

#include "indexLayoutParser.h"

#include "errors.h"
#include "logger.h"
#include "permassert.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"

/*****************************************************************************/
__attribute__((warn_unused_result))
static int setParameterValue(const LayoutParameter *lp,
                             char                  *data)
{
  if ((lp->type & LP_TYPE_MASK) == LP_UINT64) {
    int result = parseUint64(data, lp->value.num);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(UDS_INDEX_NAME_REQUIRED,
                                     "bad numeric value %s", data);
    }
  } else if ((lp->type & LP_TYPE_MASK) == LP_STRING) {
    *lp->value.str = data;
  } else {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "unkown LayoutParameter type code %x",
                                   (lp->type & LP_TYPE_MASK));
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int parseLayoutString(char                  *info,
                      const LayoutParameter *params,
                      size_t                 count)
{
  if (!strchr(info, '=')) {
    for (const LayoutParameter *lp = params; lp < params + count; ++lp) {
      if (lp->type & LP_DEFAULT) {
        int result = setParameterValue(lp, info);
        if (result != UDS_SUCCESS) {
          return result;
        }
        break;
      }
    }
  } else {
    bool seen[count];
    memset(seen, 0, sizeof(seen));

    for (char *data = NULL, *token = nextToken(info, " ", &data);
         token;
         token = nextToken(NULL, " ", &data))
    {
      char *equal = strchr(token, '=');
      const LayoutParameter *lp = NULL;
      for (lp = params; lp < params + count; ++lp) {
        if (!equal && (lp->type & LP_DEFAULT)) {
          break;
        } else if (strncmp(token, lp->name, equal - token) == 0 &&
                   strlen(lp->name) == (size_t) (equal - token)) {
          break;
        }
      }
      if (lp == NULL) {
        return logErrorWithStringError(UDS_INDEX_NAME_REQUIRED,
                                       "unkown index parameter %s",
                                       token);
      }
      if (seen[lp - params]) {
        return logErrorWithStringError(UDS_INDEX_NAME_REQUIRED,
                                       "duplicate index parameter %s",
                                       token);
      }
      int result = setParameterValue(lp, equal ? equal + 1 : token);
      if (result != UDS_SUCCESS) {
        return result;
      }
      seen[lp - params] = true;
    }
  }
  return UDS_SUCCESS;
}
