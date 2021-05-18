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
 *
 * $Id: //eng/uds-releases/krusty/src/uds/indexLayoutParser.c#9 $
 */

#include "indexLayoutParser.h"

#include "errors.h"
#include "logger.h"
#include "permassert.h"
#include "stringUtils.h"
#include "typeDefs.h"
#include "uds.h"

/**********************************************************************/
static int __must_check set_parameter_value(struct layout_parameter *lp,
					    char *data)
{
	if ((lp->type & LP_TYPE_MASK) == LP_UINT64) {
		int result = parse_uint64(data, lp->value.num);
		if (result != UDS_SUCCESS) {
			return log_error_strerror(UDS_INDEX_NAME_REQUIRED,
						  "bad numeric value %s",
						  data);
		}
	} else if ((lp->type & LP_TYPE_MASK) == LP_STRING) {
		*lp->value.str = data;
	} else {
		return log_error_strerror(UDS_INVALID_ARGUMENT,
					  "unkown layout parameter type code %x",
					  (lp->type & LP_TYPE_MASK));
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int parse_layout_string(char *info, struct layout_parameter *params)
{
	if (!strchr(info, '=')) {
		struct layout_parameter *lp;
		for (lp = params; lp->type != LP_NULL; ++lp) {
			if (lp->type & LP_DEFAULT) {
				int result = set_parameter_value(lp, info);
				if (result != UDS_SUCCESS) {
					return result;
				}
				break;
			}
		}
	} else {
		char *data = NULL;
		char *token;
		for (token = next_token(info, " ", &data); token;
		     token = next_token(NULL, " ", &data)) {
			int result;
			char *equal = strchr(token, '=');
			struct layout_parameter *lp;
			for (lp = params; lp->type != LP_NULL; ++lp) {
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
			if (lp->type == LP_NULL) {
				return log_error_strerror(UDS_INDEX_NAME_REQUIRED,
							  "unkown index parameter %s",
							  token);
			}
			if (lp->seen) {
				return log_error_strerror(UDS_INDEX_NAME_REQUIRED,
							  "duplicate index parameter %s",
							  token);
			}
			lp->seen = true;
			result = set_parameter_value(
				lp, equal ? equal + 1 : token);
			if (result != UDS_SUCCESS) {
				return result;
			}
		}
	}
	return UDS_SUCCESS;
}
