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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/vdoSuspend.h#1 $
 */

#ifndef VDO_SUSPEND_H
#define VDO_SUSPEND_H

#include "types.h"

/**
 * Ensure that the vdo has no outstanding I/O and will issue none until it is
 * resumed.
 *
 * @param vdo   The vdo to suspend
 * @param save  If <code>true</code>, all dirty metadata will be flushed as
 *              well
 *
 * @return VDO_SUCCESS or an error
 **/
int perform_vdo_suspend(struct vdo *vdo, bool save);

#endif /* VDO_SUSPEND_H */
