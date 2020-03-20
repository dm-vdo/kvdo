/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoResume.h#3 $
 */

#ifndef VDO_RESUME_H
#define VDO_RESUME_H

#include "types.h"

/**
 * Resume a suspended vdo.
 *
 * @param vdo   The vdo to resume
 *
 * @return VDO_SUCCESS or an error
 **/
int perform_vdo_resume(struct vdo *vdo);

#endif /* VDO_RESUME_H */
