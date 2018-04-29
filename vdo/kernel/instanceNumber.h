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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/instanceNumber.h#1 $
 */

/**
 * Allocate an instance number.
 *
 * @param [out] instancePtr  An integer to hold the allocated instance number
 *
 * @result  UDS_SUCCESS or an error code
 **/
int allocateKVDOInstance(unsigned int *instancePtr);

/**
 * Release an instance number previously allocated.
 *
 * @param instance  The instance number to release
 **/
void releaseKVDOInstance(unsigned int instance);

/**
 * Initialize the instance-number tracking data structures.
 **/
void initializeInstanceNumberTracking(void);

/**
 * Free up the instance-number tracking data structures.
 **/
void cleanUpInstanceNumberTracking(void);
