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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vdoRecovery.h#1 $
 */

#ifndef VDO_RECOVERY_H
#define VDO_RECOVERY_H

#include "completion.h"
#include "vdo.h"

/**
 * Construct a recovery completion and launch it. Apply all valid journal block
 * entries to all VDO structures. This function performs the offline portion of
 * recovering a VDO from a crash.
 *
 * @param vdo     The vdo to recover
 * @param parent  The completion to notify when the offline portion of the
 *                recovery is complete
 **/
void launchRecovery(VDO *vdo, VDOCompletion *parent);

#endif // VDO_RECOVERY_H
