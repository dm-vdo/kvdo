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
 * $Id: //eng/uds-releases/homer/kernelLinux/uds/atomicDefs.h#1 $
 */

#ifndef LINUX_KERNEL_ATOMIC_DEFS_H
#define LINUX_KERNEL_ATOMIC_DEFS_H

#include <linux/atomic.h>

// Some versions of RHEL7.4 do not define atomic_read_acquire.
// Borrow this definition so our older test systems will work.
#ifndef atomic_read_acquire
#define atomic_read_acquire(v)         smp_load_acquire(&(v)->counter)
#endif

// Some versions of RHEL7.4 do not define atomic_set_release.
// Borrow this definition so our older test systems will work.
#ifndef atomic_set_release
#define atomic_set_release(v, i)       smp_store_release(&(v)->counter, (i))
#endif

// Some older kernels didn't have ACCESS_ONCE.
// Very new kernels have dropped it for {READ,WRITE}_ONCE.
// So should we, eventually.
// Borrow this definition from our user-mode code for now.
#ifndef ACCESS_ONCE
#define ACCESS_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#endif

#endif /* LINUX_KERNEL_ATOMIC_DEFS_H */
