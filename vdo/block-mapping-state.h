/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef BLOCK_MAPPING_STATE_H
#define BLOCK_MAPPING_STATE_H

#include "type-defs.h"

/*
 * Four bits of each five-byte block map entry contain a mapping state value
 * used to distinguish unmapped or trimmed logical blocks (which are treated
 * as mapped to the zero block) from entries that have been mapped to a
 * physical block, including the zero block.
 *
 * FIXME: these should maybe be defines.
 */
enum block_mapping_state {
	VDO_MAPPING_STATE_UNMAPPED = 0, /* Must be zero to be the default value */
	VDO_MAPPING_STATE_UNCOMPRESSED = 1, /* A normal (uncompressed) block */
	VDO_MAPPING_STATE_COMPRESSED_BASE = 2, /* Compressed in slot 0 */
	VDO_MAPPING_STATE_COMPRESSED_MAX = 15, /* Compressed in slot 13 */
};

enum {
	VDO_MAX_COMPRESSION_SLOTS = (VDO_MAPPING_STATE_COMPRESSED_MAX
				     - VDO_MAPPING_STATE_COMPRESSED_BASE + 1),
};

static inline enum block_mapping_state vdo_get_state_for_slot(byte slot_number)
{
	return (slot_number + VDO_MAPPING_STATE_COMPRESSED_BASE);
}

static inline byte
vdo_get_slot_from_state(enum block_mapping_state mapping_state)
{
	return (mapping_state - VDO_MAPPING_STATE_COMPRESSED_BASE);
}

static inline bool
vdo_is_state_compressed(const enum block_mapping_state mapping_state)
{
	return (mapping_state > VDO_MAPPING_STATE_UNCOMPRESSED);
}

#endif /* BLOCK_MAPPING_STATE_H */
