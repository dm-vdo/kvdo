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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/randomLinuxKernel.c#1 $
 */

#include <linux/random.h>

#include "hashUtils.h"
#include "random.h"

/*****************************************************************************/
void fillRandomly(void *ptr, size_t len)
{
  /*
   * get_random_bytes(ptr, len) works here, but calling it too often for
   * large blocks causes soft lockups.  64KB is large.  So we use
   * get_random_bytes() to seed a counter and use murmurHashChunkName to
   * generate most of the bytes we need.
   */
  if (len > sizeof(UdsChunkName)) {
    UdsChunkName *namePtr = ptr;
    unsigned long counter;
    unsigned long increment = 1;
    get_random_bytes(&counter, sizeof(counter));
    do {
      *namePtr = murmurHashChunkName(&counter, sizeof(counter), 0);
      counter += increment;
      increment++;
      namePtr++;
      len -= sizeof(UdsChunkName);
    } while (len >= sizeof(UdsChunkName));
    ptr = namePtr;
  }
  if (len > 0) {
    get_random_bytes(ptr, len);
  }
}
