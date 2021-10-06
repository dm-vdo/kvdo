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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdo-resume.h#1 $
 */

#ifndef VDO_RESUME_H
#define VDO_RESUME_H

#include "kernel-types.h"
#include "types.h"

/**
 * Resume a suspended vdo (technically preresume because resume can't fail).
 *
 * @param vdo          The vdo being resumed
 * @param config       The device config derived from the table with which the
 *                     vdo is being resumed
 * @param device_name  The vdo device name (for logging)
 *
 * @return VDO_SUCCESS or an error
 **/
int preresume_vdo(struct vdo *vdo,
		  struct device_config *config,
		  const char *device_name);

#endif /* VDO_RESUME_H */
