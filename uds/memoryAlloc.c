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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/memoryAlloc.c#1 $
 */

#include "memoryAlloc.h"

#include "stringUtils.h"

/**********************************************************************/
int uds_duplicate_string(const char *string, const char *what,
			 char **new_string)
{
	return uds_memdup(string, strlen(string) + 1, what, new_string);
}

/**********************************************************************/
int uds_memdup(const void *buffer, size_t size, const char *what,
	       void *dup_ptr)
{
	byte *dup;
	int result = UDS_ALLOCATE(size, byte, what, &dup);
	if (result != UDS_SUCCESS) {
		return result;
	}

	memcpy(dup, buffer, size);
	*((void **) dup_ptr) = dup;
	return UDS_SUCCESS;
}
