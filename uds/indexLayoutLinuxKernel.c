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
 * $Id: //eng/uds-releases/lisa/kernelLinux/uds/indexLayoutLinuxKernel.c#1 $
 */

#include "indexLayout.h"
#include "indexLayoutParser.h"
#include "memoryAlloc.h"

/**********************************************************************/
int make_uds_index_layout(const char *name,
			  bool new_layout,
			  const struct uds_configuration *config,
			  struct index_layout **layout_ptr)
{
	char *dev = NULL;
	uint64_t offset = 0;
	uint64_t size = 0;
	char *params = NULL;
	struct io_factory *factory = NULL;
	struct index_layout *layout;
	int result;

	struct layout_parameter parameter_table[] = {
		{ "dev", LP_STRING | LP_DEFAULT, { .str = &dev } },
		{ "offset", LP_UINT64, { .num = &offset } },
		{ "size", LP_UINT64, { .num = &size } },
		LP_NULL_PARAMETER,
	};

	result = uds_duplicate_string(name, "make_uds_index_layout parameters",
				      &params);
	if (result != UDS_SUCCESS) {
		return result;
	}

	// note dev will be set to memory owned by params
	result = parse_layout_string(params, parameter_table);
	if (result != UDS_SUCCESS) {
		UDS_FREE(params);
		return result;
	}

	result = make_uds_io_factory(dev, &factory);
	UDS_FREE(params);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = make_uds_index_layout_from_factory(
		factory, offset, size, new_layout, config, &layout);
	put_uds_io_factory(factory);
	if (result != UDS_SUCCESS) {
		return result;
	}
	*layout_ptr = layout;
	return UDS_SUCCESS;
}
