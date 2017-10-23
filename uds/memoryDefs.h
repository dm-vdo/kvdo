/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/memoryDefs.h#3 $
 */

#ifndef LINUX_KERNEL_MEMORY_DEFS_H
#define LINUX_KERNEL_MEMORY_DEFS_H 1

#include <linux/io.h>  // for PAGE_SIZE

#include "threadRegistry.h"
#include "typeDefs.h"

/**
 * Allocate one or more elements of the indicated type, aligning them
 * on the boundary that will allow them to be used in io, logging an
 * error if the allocation fails. The memory will be zeroed.
 *
 * @param COUNT  The number of objects to allocate
 * @param TYPE   The type of objects to allocate
 * @param WHAT   What is being allocated (for error logging)
 * @param PTR    A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
#define ALLOCATE_IO_ALIGNED(COUNT, TYPE, WHAT, PTR) \
  doAllocation(COUNT, sizeof(TYPE), 0, PAGE_SIZE, WHAT, PTR)

/**
 * Perform termination of the memory allocation subsystem.
 **/
void memoryExit(void);

/**
 * Perform initialization of the memory allocation subsystem.
 **/
void memoryInit(void);

/**
 * Register the current thread as an allocating thread.
 *
 * An optional flag location can be supplied indicating whether, at
 * any given point in time, the threads associated with that flag
 * should be allocating storage.  If the flag is false, a message will
 * be logged.
 *
 * If no flag is supplied, the thread is always allowed to allocate
 * storage without complaint.
 *
 * @param newThread  RegisteredThread structure to use for the current thread
 * @param flagPtr    Location of the allocation-allowed flag
 **/
void registerAllocatingThread(RegisteredThread *newThread,
                              const bool       *flagPtr);

/**
 * Unregister the current thread as an allocating thread.
 **/
void unregisterAllocatingThread(void);

/**
 * Track an allocation of a bio structure via bio_alloc_bioset.
 *
 * Allocations should be paired with frees. If the calls to
 * recordBioAlloc aren't balanced with calls to recordBioFree by the
 * time the driver is unloaded, the driver will report an assertion
 * failure.
 **/
void recordBioAlloc(void);

/**
 * Track the freeing of a bio structure via bio_free.
 *
 * Allocations should be paired with frees. If the calls to
 * recordBioAlloc aren't balanced with calls to recordBioFree by the
 * time the driver is unloaded, the driver will report an assertion
 * failure.
 **/
void recordBioFree(void);

/**
 * Get the memory statistics.
 *
 * @param bytesUsed     A pointer to hold the number of bytes in use
 * @param peakBioCount  A pointer to hold the maximum value bytes used has
 *                      attained
 * @param biosUsed      A pointer to hold the number of bios in use
 * @param peakBioCount  A pointer to hold the maximum value biosUsed has
 *                      attained
 **/
void getMemoryStats(uint64_t *bytesUsed,
                    uint64_t *peakBytesUsed,
                    uint64_t *biosUsed,
                    uint64_t *peakBioCount);

/**
 * Report stats on any allocated memory that we're tracking.
 *
 * Not all allocation types are guaranteed to be tracked in bytes
 * (e.g., bios).
 **/
void reportMemoryUsage(void);

#endif /* LINUX_KERNEL_MEMORY_DEFS_H */
