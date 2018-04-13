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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/slabJournalEraser.h#1 $
 */

#ifndef SLAB_JOURNAL_ERASER_H
#define SLAB_JOURNAL_ERASER_H

#include "slabIterator.h"
#include "types.h"

/**
 * Begin erasing slab journals, one at a time.
 *
 * @param depot         The depot from which to erase
 * @param slabs         The slabs whose journals need erasing
 * @param parent        The object to notify when complete
 **/
void eraseSlabJournals(SlabDepot     *depot,
                       SlabIterator   slabs,
                       VDOCompletion *parent);

#endif // SLAB_JOURNAL_ERASER_H
