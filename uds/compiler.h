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
 * $Id: //eng/uds-releases/gloria/src/uds/compiler.h#1 $
 */

#ifndef COMMON_COMPILER_H
#define COMMON_COMPILER_H

#include "compilerDefs.h"

// Count the elements in a static array while attempting to catch some type
// errors. (See http://stackoverflow.com/a/1598827 for an explanation.)
#define COUNT_OF(x) ((sizeof(x) / sizeof(0[x])) \
                     / ((size_t) (!(sizeof(x) % sizeof(0[x])))))

#define const_container_of(ptr, type, member)                     \
  __extension__ ({                                                \
    const __typeof__(((type *)0)->member) *__mptr = (ptr);        \
    (const type *)((const char *)__mptr - offsetof(type,member)); \
  })

// The "inline" keyword alone takes affect only when the optimization level
// is high enough.  Define INLINE to force the gcc to "always inline".
#define INLINE __attribute__((always_inline)) inline

#endif /* COMMON_COMPILER_H */
