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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/blockMapPage.c#25 $
 */

#include "blockMapPage.h"

#include "permassert.h"

#include "constants.h"
#include "statusCodes.h"
#include "types.h"

enum {
	PAGE_HEADER_4_1_SIZE = 8 + 8 + 8 + 1 + 1 + 1 + 1,
};

static const struct version_number BLOCK_MAP_4_1 = {
	.major_version = 4,
	.minor_version = 1,
};

/**********************************************************************/
bool is_current_block_map_page(const struct block_map_page *page)
{
	return are_same_version(BLOCK_MAP_4_1,
				unpack_version_number(page->version));
}

/**********************************************************************/
struct block_map_page *format_block_map_page(void *buffer,
					     nonce_t nonce,
					     physical_block_number_t pbn,
					     bool initialized)
{
	memset(buffer, 0, VDO_BLOCK_SIZE);
	struct block_map_page *page = (struct block_map_page *) buffer;
	page->version = pack_version_number(BLOCK_MAP_4_1);
	page->header.nonce = __cpu_to_le64(nonce);
	page->header.pbn = __cpu_to_le64(pbn);
	page->header.initialized = initialized;
	return page;
}

/**********************************************************************/
block_map_page_validity validate_block_map_page(struct block_map_page *page,
						nonce_t nonce,
						physical_block_number_t pbn)
{
	// Make sure the page layout isn't accidentally changed by changing the
	// length of the page header.
	STATIC_ASSERT_SIZEOF(struct block_map_page_header,
			     PAGE_HEADER_4_1_SIZE);

	if (!are_same_version(BLOCK_MAP_4_1,
			      unpack_version_number(page->version)) ||
	    !is_block_map_page_initialized(page) ||
	    (nonce != __le64_to_cpu(page->header.nonce))) {
		return BLOCK_MAP_PAGE_INVALID;
	}

	if (pbn != get_block_map_page_pbn(page)) {
		return BLOCK_MAP_PAGE_BAD;
	}

	return BLOCK_MAP_PAGE_VALID;
}
