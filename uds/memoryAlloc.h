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
 * $Id: //eng/uds-releases/gloria/src/uds/memoryAlloc.h#2 $
 */

#ifndef MEMORY_ALLOC_H
#define MEMORY_ALLOC_H 1

#include <stdarg.h>

#include "compiler.h"
#include "cpu.h"
#include "memoryDefs.h"
#include "permassert.h"

/**
 * Allocate storage based on memory size and  alignment, logging an error if
 * the allocation fails. The memory will be zeroed.
 *
 * @param size   The size of an object
 * @param align  The required alignment
 * @param what   What is being allocated (for error logging)
 * @param ptr    A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
int allocateMemory(size_t size, size_t align, const char *what, void *ptr)
  __attribute__((warn_unused_result));

/**
 * Free storage
 *
 * @param ptr  The memory to be freed
 **/
void freeMemory(void *ptr);

/**
 * Allocate storage and do a vsprintf into it.  The memory allocation part of
 * this operation is platform dependent.
 *
 * @param what  A description of what is being allocated.
 * @param strp  The pointer in which to store the allocated string.
 * @param fmt   The sprintf format parameter.
 * @param ap    The format argument list
 *
 * @return UDS_SUCCESS or an error code
 **/
int doPlatformVasprintf(const char  *what,
                        char       **strp,
                        const char  *fmt,
                        va_list      ap)
  __attribute__((format(printf, 3, 0), warn_unused_result));

/**
 * Allocate storage based on element counts, sizes, and alignment.
 *
 * This is a generalized form of our allocation use case: It allocates
 * an array of objects, optionally preceded by one object of another
 * type (i.e., a struct with trailing variable-length array), with the
 * alignment indicated.
 *
 * Why is this inline?  The sizes and alignment will always be
 * constant, when invoked through the macros below, and often the
 * count will be a compile-time constant 1 or the number of extra
 * bytes will be a compile-time constant 0.  So at least some of the
 * arithmetic can usually be optimized away, and the run-time
 * selection between allocation functions always can.  In many cases,
 * it'll boil down to just a function call with a constant size.
 *
 * @param count   The number of objects to allocate
 * @param size    The size of an object
 * @param extra   The number of additional bytes to allocate
 * @param align   The required alignment
 * @param what    What is being allocated (for error logging)
 * @param ptr     A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int doAllocation(size_t      count,
                               size_t      size,
                               size_t      extra,
                               size_t      align,
                               const char *what,
                               void       *ptr)
{
  size_t totalSize = count * size + extra;
  // Overflow check:
  if ((size > 0) && (count > ((SIZE_MAX - extra) / size))) {
    /*
     * This is kind of a hack: We rely on the fact that SIZE_MAX would
     * cover the entire address space (minus one byte) and thus the
     * system can never allocate that much and the call will always
     * fail.  So we can report an overflow as "out of memory" by asking
     * for "merely" SIZE_MAX bytes.
     */
    totalSize = SIZE_MAX;
  }

  return allocateMemory(totalSize, align, what, ptr);
}

/**
 * Reallocate dynamically allocated memory.  There are no alignment guarantees
 * for the reallocated memory.
 *
 * @param ptr      The memory to reallocate.
 * @param oldSize  The old size of the memory
 * @param size     The new size to allocate
 * @param what     What is being allocated (for error logging)
 * @param newPtr   A pointer to hold the reallocated pointer
 *
 * @return UDS_SUCCESS or an error code
 **/
int reallocateMemory(void       *ptr,
                     size_t      oldSize,
                     size_t      size,
                     const char *what,
                     void       *newPtr)
  __attribute__((warn_unused_result));

/**
 * Allocate one or more elements of the indicated type, logging an
 * error if the allocation fails. The memory will be zeroed.
 *
 * @param COUNT  The number of objects to allocate
 * @param TYPE   The type of objects to allocate.  This type determines the
 *               alignment of the allocated memory.
 * @param WHAT   What is being allocated (for error logging)
 * @param PTR    A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
#define ALLOCATE(COUNT, TYPE, WHAT, PTR) \
  doAllocation(COUNT, sizeof(TYPE), 0, __alignof__(TYPE), WHAT, PTR)

/**
 * Allocate one object of an indicated type, followed by one or more
 * elements of a second type, logging an error if the allocation
 * fails. The memory will be zeroed.
 *
 * @param TYPE1  The type of the primary object to allocate.  This type
 *               determines the alignment of the allocated memory.
 * @param COUNT  The number of objects to allocate
 * @param TYPE2  The type of array objects to allocate
 * @param WHAT   What is being allocated (for error logging)
 * @param PTR    A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
#define ALLOCATE_EXTENDED(TYPE1, COUNT, TYPE2, WHAT, PTR)             \
  __extension__ ({                                                    \
      TYPE1 **_ptr = (PTR);                                           \
      STATIC_ASSERT(__alignof__(TYPE1) >= __alignof__(TYPE2));        \
      int _result = doAllocation(COUNT, sizeof(TYPE2), sizeof(TYPE1), \
                                 __alignof__(TYPE1), WHAT, _ptr);     \
      _result;                                                        \
    })

/**
 * Free memory allocated with ALLOCATE().
 *
 * @param ptr    Pointer to the memory to free
 **/
static INLINE void FREE(void *ptr)
{
  freeMemory(ptr);
}

/**
 * Allocate memory starting on a cache line boundary, logging an error if the
 * allocation fails. The memory will be zeroed.
 *
 * @param size  The number of bytes to allocate
 * @param what  What is being allocated (for error logging)
 * @param ptr   A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
__attribute__((warn_unused_result))
static INLINE int allocateCacheAligned(size_t      size,
                                       const char *what,
                                       void       *ptr)
{
  return allocateMemory(size, CACHE_LINE_BYTES, what, ptr);
}

/**
 * Duplicate a string.
 *
 * @param string    The string to duplicate
 * @param what      What is being allocated (for error logging)
 * @param newString A pointer to hold the duplicated string
 *
 * @return UDS_SUCCESS or an error code
 **/
int duplicateString(const char *string, const char *what, char **newString)
  __attribute__((warn_unused_result));

/**
 * Duplicate a buffer, logging an error if the allocation fails.
 *
 * @param ptr     The buffer to copy
 * @param size    The size of the buffer
 * @param what    What is being duplicated (for error logging)
 * @param dupPtr  A pointer to hold the allocated array
 *
 * @return UDS_SUCCESS or ENOMEM
 **/
int memdup(const void *ptr, size_t size, const char *what, void *dupPtr)
  __attribute__((warn_unused_result));

/**
 * Wrapper which permits freeing a const pointer.
 *
 * @param pointer  the pointer to be freed
 **/
static INLINE void freeConst(const void *pointer)
{
  union {
    const void *constP;
    void *notConst;
  } u = { .constP = pointer };
  FREE(u.notConst);
}

/**
 * Wrapper which permits freeing a volatile pointer.
 *
 * @param pointer  the pointer to be freed
 **/
static INLINE void freeVolatile(volatile void *pointer)
{
  union {
    volatile void *volP;
    void *notVol;
  } u = { .volP = pointer };
  FREE(u.notVol);
}

#endif /* MEMORY_ALLOC_H */
