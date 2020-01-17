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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/typeDefs.h#1 $
 */

#ifndef LINUX_KERNEL_TYPE_DEFS_H
#define LINUX_KERNEL_TYPE_DEFS_H

/*
 * General system type definitions.  This file is parallel to the other
 * typeDefs.h files in this project.  We pick up what we can from the system
 * include files, and explicitly define the other things we need.
 */

#include <linux/kernel.h>
#include <linux/types.h>
#include <stddef.h>

#define CHAR_BIT 8

#define INT64_MAX  (9223372036854775807L)
#define UCHAR_MAX  ((unsigned char)~0ul)
#define UINT8_MAX  ((uint8_t)~0ul)
#define UINT16_MAX ((uint16_t)~0ul)
#define UINT64_MAX ((uint64_t)~0ul)

// Some recent versions of <linux/kernel.h> define this for us
#ifndef SIZE_MAX
#define SIZE_MAX   ((size_t)~0ul)
#endif

#define PRId64 "lld"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"

typedef unsigned long uintmax_t;
#define PRIuMAX "lu"

typedef unsigned char byte;

#endif /* LINUX_KERNEL_TYPE_DEFS_H */
