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
 * $Id: //eng/vdo-releases/sulfur/src/c++/vdo/base/instanceNumber.h#7 $
 */

#ifndef INSTANCE_NUMBER_H
#define INSTANCE_NUMBER_H

/**
 * Allocate an instance number.
 *
 * @param [out] instance_ptr  An integer to hold the allocated instance number
 *
 * @result  UDS_SUCCESS or an error code
 **/
int allocate_vdo_instance(unsigned int *instance_ptr);

/**
 * Release an instance number previously allocated.
 *
 * @param instance  The instance number to release
 **/
void release_vdo_instance(unsigned int instance);

/**
 * Initialize the instance-number tracking data structures.
 **/
void initialize_vdo_instance_number_tracking(void);

/**
 * Free up the instance-number tracking data structures.
 **/
void clean_up_vdo_instance_number_tracking(void);

#endif // INSTANCE_NUMBER_H
