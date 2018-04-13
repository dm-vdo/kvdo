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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/blockMapRecovery.h#1 $
 */

#ifndef BLOCK_MAP_RECOVERY_H
#define BLOCK_MAP_RECOVERY_H

#include "blockMap.h"
#include "blockMappingState.h"
#include "types.h"

/**
 * An explicitly numbered block mapping. Numbering the mappings allows them to
 * be sorted by logical block number during recovery while still preserving
 * the relative order of journal entries with the same logical block number.
 **/
typedef struct {
  BlockMapSlot       blockMapSlot;   // Block map slot to map
  BlockMapEntry      blockMapEntry;  // The encoded block map entry for the LBN
  uint32_t           number;         // The serial number to use during replay
} __attribute__((packed)) NumberedBlockMapping;

/**
 * Recover the block map (normal rebuild).
 *
 * @param vdo             The VDO
 * @param entryCount      The number of journal entries
 * @param journalEntries  An array of journal entries to process
 * @param parent          The completion to notify when the rebuild is complete
 **/
void recoverBlockMap(VDO                   *vdo,
                     BlockCount             entryCount,
                     NumberedBlockMapping  *journalEntries,
                     VDOCompletion         *parent);

#endif // BLOCK_MAP_RECOVERY_H
