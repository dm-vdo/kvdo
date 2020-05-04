/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/krusty/src/uds/indexLayoutParser.c#3 $
 */

#include "indexLayoutParser.h"

#include "errors.h"
#include "logger.h"
#include "permassert.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"

/*****************************************************************************/
static int __must_check setParameterValue(LayoutParameter *lp, char *data)
{
	if ((lp->type & LP_TYPE_MASK) == LP_UINT64) {
		int result = parseUint64(data, lp->value.num);
		if (result != UDS_SUCCESS) {
			return logErrorWithStringError(UDS_INDEX_NAME_REQUIRED,
						       "bad numeric value %s",
						       data);
		}
	} else if ((lp->type & LP_TYPE_MASK) == LP_STRING) {
		*lp->value.str = data;
	} else {
		return logErrorWithStringError(
			UDS_INVALID_ARGUMENT,
			"unkown LayoutParameter type code %x",
			(lp->type & LP_TYPE_MASK));
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int parseLayoutString(char *info, LayoutParameter *params, size_t count)
{
	if (!strchr(info, '=')) {
		LayoutParameter *lp;
		for (lp = params; lp < params + count; ++lp) {
			if (lp->type & LP_DEFAULT) {
				int result = setParameterValue(lp, info);
				if (result != UDS_SUCCESS) {
					return result;
				}
				break;
			}
		}
	} else {
		char *data = NULL;
		char *token;
		for (token = nextToken(info, " ", &data); token;
		     token = nextToken(NULL, " ", &data)) {
			char *equal = strchr(token, '=');
			LayoutParameter *lp;
			for (lp = params; lp < params + count; ++lp) {
				if (!equal && (lp->type & LP_DEFAULT)) {
					break;
				} else if (strncmp(token,
						   lp->name,
						   equal - token) == 0 &&
					   strlen(lp->name) ==
						   (size_t)(equal - token)) {
					break;
				}
			}
			if (lp == NULL) {
				return logErrorWithStringError(
					UDS_INDEX_NAME_REQUIRED,
					"unkown index parameter %s",
					token);
			}
			if (lp->seen) {
				return logErrorWithStringError(
					UDS_INDEX_NAME_REQUIRED,
					"duplicate index parameter %s",
					token);
			}
			lp->seen = true;
			int result = setParameterValue(
				lp, equal ? equal + 1 : token);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	}
	return UDS_SUCCESS;
}
