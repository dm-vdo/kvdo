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
 * $Id: //eng/uds-releases/gloria/src/uds/memoryAlloc.c#1 $
 */

#include "memoryAlloc.h"

#include "stringUtils.h"

/**********************************************************************/
int duplicateString(const char *string, const char *what, char **newString)
{
  return memdup(string, strlen(string) + 1, what, newString);
}

/**********************************************************************/
int memdup(const void *buffer, size_t size, const char *what, void *dupPtr)
{
  byte *dup;
  int result = ALLOCATE(size, byte, what, &dup);
  if (result != UDS_SUCCESS) {
    return result;
  }

  memcpy(dup, buffer, size);
  *((void **) dupPtr) = dup;
  return UDS_SUCCESS;
}
