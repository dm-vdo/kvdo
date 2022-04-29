// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "block-map-page.h"

#include "permassert.h"

#include "constants.h"
#include "status-codes.h"
#include "types.h"

enum {
	PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const struct version_number BLOCK_MAP_4_1 = {
	.major_version = 4,
	.minor_version = 1,
};

struct block_map_page *vdo_format_block_map_page(void *buffer,
						 nonce_t nonce,
						 physical_block_number_t pbn,
						 bool initialized)
{
	struct block_map_page *page = (struct block_map_page *) buffer;

	memset(buffer, 0, VDO_BLOCK_SIZE);
	page->version = vdo_pack_version_number(BLOCK_MAP_4_1);
	page->header.nonce = __cpu_to_le64(nonce);
	page->header.pbn = __cpu_to_le64(pbn);
	page->header.initialized = initialized;
	return page;
}

enum block_map_page_validity
vdo_validate_block_map_page(struct block_map_page *page,
			    nonce_t nonce,
			    physical_block_number_t pbn)
{
	STATIC_ASSERT_SIZEOF(struct block_map_page_header,
			     PAGE_HEADER_4_1_SIZE);

	if (!vdo_are_same_version(BLOCK_MAP_4_1,
				  vdo_unpack_version_number(page->version)) ||
	    !vdo_is_block_map_page_initialized(page) ||
	    (nonce != __le64_to_cpu(page->header.nonce))) {
		return VDO_BLOCK_MAP_PAGE_INVALID;
	}

	if (pbn != vdo_get_block_map_page_pbn(page)) {
		return VDO_BLOCK_MAP_PAGE_BAD;
	}

	return VDO_BLOCK_MAP_PAGE_VALID;
}
