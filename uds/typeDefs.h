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
 */

#ifndef TYPE_DEFS_H
#define TYPE_DEFS_H

/*
 * General system type definitions.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/types.h>

typedef unsigned char byte;

#define CHAR_BIT 8

#define INT64_MAX  (9223372036854775807L)
#define UCHAR_MAX  ((unsigned char)~0ul)
#define UINT8_MAX  ((uint8_t)~0ul)
#define UINT16_MAX ((uint16_t)~0ul)
#define UINT64_MAX ((uint64_t)~0ul)

// Some recent versions of <linux/kernel.h> define this for us
#ifndef SIZE_MAX
#define SIZE_MAX   ((size_t)~0ul)
#endif /* SIZE_MAX */


#endif /* TYPE_DEFS_H */
