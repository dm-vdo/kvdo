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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/randomDefs.h#3 $
 */

#ifndef LINUX_KERNEL_RANDOM_DEFS_H
#define LINUX_KERNEL_RANDOM_DEFS_H 1

#include <linux/random.h>

#include "compiler.h"

#define RAND_MAX 2147483647

/**
 * Fill bytes with random data.
 *
 * @param ptr   where to store bytes
 * @param len   number of bytes to write
 **/
static INLINE void fillRandomly(void *ptr, size_t len)
{
  prandom_bytes(ptr, len);
}

/**
 * Random number generator
 *
 * @return a random number in the rand 0 to RAND_MAX
 **/
static INLINE long random(void)
{
  long value;
  fillRandomly(&value, sizeof(value));
  return value & RAND_MAX;
}

#endif /* LINUX_KERNEL_RANDOM_DEFS_H */
