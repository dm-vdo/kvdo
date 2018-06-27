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
 * $Id: //eng/uds-releases/gloria/kernelLinux/uds/numericDefs.h#2 $
 */

#ifndef LINUX_KERNEL_NUMERIC_DEFS_H
#define LINUX_KERNEL_NUMERIC_DEFS_H 1

#ifdef __x86_64__
/*
 * __builtin_bswap16 should work fine here too, but check for a
 * performance impact before changing it, just to be safe.
 */
#define bswap_16(x) \
  (__extension__                                                        \
   ({ register unsigned short int __v, __x = (unsigned short int) (x);  \
     __asm__ ("rorw $8, %w0"                                            \
              : "=r" (__v)                                              \
              : "0" (__x)                                               \
              : "cc");                                                  \
     __v; }))
#else
#define bswap_16(x) __builtin_bswap16(x)
#endif

#endif /* LINUX_KERNEL_NUMERIC_DEFS_H */
