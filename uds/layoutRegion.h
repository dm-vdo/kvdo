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
 * $Id: //eng/uds-releases/krusty/src/uds/layoutRegion.h#2 $
 */

#ifndef LAYOUT_REGION_H
#define LAYOUT_REGION_H

/**
 * Single file layouts are defined in terms of data regions. Each data region
 * is a sub-section of the available space. Some data regions may contain
 * subsidiary data regions, for example, a checkpoint or index save will
 * contain master index regions (according to the number of zones), an
 * index page map region, and possibly an open chapter region.
 **/

static const uint64_t REGION_MAGIC = 0x416c6252676e3031; // 'AlbRgn01'

struct region_header {
	uint64_t magic;         // REGION_MAGIC
	uint64_t region_blocks; // size of whole region
	uint16_t type;          // RH_TYPE_...
	uint16_t version;       // 1
	uint16_t num_regions;   // number of layouts in the table
	uint16_t payload;       // extra data beyond region table
};

struct layout_region {
	uint64_t start_block;
	uint64_t num_blocks;
	uint32_t checksum; // only used for save regions
	uint16_t kind;
	uint16_t instance;
};

struct region_table {
	struct region_header header;
	struct layout_region regions[];
};

#endif // LAYOUT_REGION_H
