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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/atomic.h#2 $
 */

#ifndef ATOMIC_H
#define ATOMIC_H

#include "atomicDefs.h"
#include "compiler.h"
#include "typeDefs.h"

#define ATOMIC_INITIALIZER(value) { (value) }

typedef struct {
  atomic_t value;
} __attribute__((aligned(4))) Atomic32;

typedef struct {
  atomic64_t value;
} __attribute__((aligned(8))) Atomic64;

typedef struct {
  Atomic32 value;
} __attribute__((aligned(4))) AtomicBool;

/**
 * Memory load operations that precede this fence will be prevented from
 * changing order with any that follow this fence, by either the compiler or
 * the CPU. This can be used to ensure that the load operations accessing
 * the fields of a structure are not re-ordered so they actually take effect
 * before a pointer to the structure is resolved.
 **/
static INLINE void loadFence(void)
{
  smp_rmb();
}

/**
 * Memory store operations that precede this fence will be prevented from
 * changing order with any that follow this fence, by either the compiler or
 * the CPU. This can be used to ensure that the store operations initializing
 * the fields of a structure are not re-ordered so they actually take effect
 * after a pointer to the structure is published.
 **/
static INLINE void storeFence(void)
{
  smp_wmb();
}

/**
 * Generate a full memory fence for the compiler and CPU. Load and store
 * operations issued before the fence will not be re-ordered with operations
 * issued after the fence.
 **/
static INLINE void memoryFence(void)
{
  smp_mb();
}

/**
 * Access the value of a 32-bit atomic variable, ensuring that the load is not
 * re-ordered by the compiler or CPU with any subsequent load operations.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE uint32_t atomicLoad32(const Atomic32 *atom)
{
  uint32_t value = atomic_read(&atom->value);
  loadFence();
  return value;
}

/**
 * Access the value of a 64-bit atomic variable, ensuring that the memory load
 * is not re-ordered by the compiler or CPU with any subsequent load
 * operations.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE uint64_t atomicLoad64(const Atomic64 *atom)
{
  uint64_t value = atomic64_read(&atom->value);
  loadFence();
  return value;
}

/**
 * Access the value of a boolean atomic variable, ensuring that the load is not
 * re-ordered by the compiler or CPU with any subsequent load operations.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE bool atomicLoadBool(const AtomicBool *atom)
{
  return (atomicLoad32(&atom->value) > 0);
}

/**
 * Set the value of a 32-bit atomic variable, ensuring that the memory store
 * operation is not re-ordered by the compiler or CPU with any preceding store
 * operations.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void atomicStore32(Atomic32 *atom, uint32_t newValue)
{
  storeFence();
  atomic_set(&atom->value, newValue);
}

/**
 * Set the value of a 64-bit atomic variable, ensuring that the memory store
 * operation is not re-ordered by the compiler or CPU with any preceding store
 * operations.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void atomicStore64(Atomic64 *atom, uint64_t newValue)
{
  storeFence();
  atomic64_set(&atom->value, newValue);
}

/**
 * Set the value of a boolean atomic variable, ensuring that the memory store
 * operation is not re-ordered by the compiler or CPU with any preceding store
 * operations.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void atomicStoreBool(AtomicBool *atom, bool newValue)
{
  atomicStore32(&atom->value, (newValue ? 1 : 0));
}

/**
 * Add a 32-bit signed delta to a 32-bit atomic variable.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return       the new value of the atom after the add operation
 **/
static INLINE uint32_t atomicAdd32(Atomic32 *atom, int32_t delta)
{
  return atomic_add_return(delta, &atom->value);
}

/**
 * Add a 64-bit signed delta to a 64-bit atomic variable.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return       the new value of the atom after the add operation
 **/
static INLINE uint64_t atomicAdd64(Atomic64 *atom, int64_t delta)
{
  return atomic64_add_return(delta, &atom->value);
}

/**
 * Atomic 32-bit compare-and-swap. If the atom is identical to a required
 * value, atomically replace it with the new value and return true, otherwise
 * do nothing and return false.
 *
 * @param atom           a pointer to the atomic variable
 * @param requiredValue  the value that must be present to perform the swap
 * @param newValue       the value to be swapped for the required value
 *
 * @return               true if the atom was changed, false otherwise
 **/
static INLINE bool compareAndSwap32(Atomic32 *atom,
                                    uint32_t  requiredValue,
                                    uint32_t  newValue)
{
  /*
   * Our initial implementation, for x86, effectively got a full
   * memory barrier because of how "lock cmpxchg" operates. The
   * atomic_cmpxchg interface provides for a full barrier *if* the
   * exchange is done, but not necessarily if it is not.
   *
   * Do we need the full barrier always? We need to investigate that,
   * as part of (eventually) converting to using that API directly.
   * For now, play it safe, and ensure the same behavior on other
   * architectures too.
   */
#ifndef __x86_64__
  smp_mb();
#endif
  int oldValue = atomic_cmpxchg(&atom->value, requiredValue, newValue);
#ifndef __x86_64__
  smp_mb();
#endif
  return requiredValue == (uint32_t) oldValue;
}

/**
 * Atomic 64-bit compare-and-swap. If the atom is identical to a required
 * value, atomically replace it with the new value and return true, otherwise
 * do nothing and return false.
 *
 * @param atom           a pointer to the atomic variable
 * @param requiredValue  the value that must be present to perform the swap
 * @param newValue       the value to be swapped for the required value
 *
 * @return               true if the atom was changed, false otherwise
 **/
static INLINE bool compareAndSwap64(Atomic64 *atom,
                                    uint64_t  requiredValue,
                                    uint64_t  newValue)
{
#ifndef __x86_64__
  smp_mb();
#endif
  long oldValue = atomic64_cmpxchg(&atom->value, requiredValue, newValue);
#ifndef __x86_64__
  smp_mb();
#endif
  return requiredValue == (uint64_t) oldValue;
}

/**
 * Atomic boolean compare-and-swap. If the atom is identical to a required
 * value, atomically replace it with the new value and return true, otherwise
 * do nothing and return false.
 *
 * @param atom           a pointer to the atomic variable
 * @param requiredValue  the value that must be present to perform the swap
 * @param newValue       the value to be swapped for the required value
 *
 * @return               true if the atom was changed, false otherwise
 **/
static INLINE bool compareAndSwapBool(AtomicBool *atom,
                                      bool        requiredValue,
                                      bool        newValue)
{
  return compareAndSwap32(&atom->value, (requiredValue ? 1 : 0),
                          (newValue ? 1 : 0));
}

/**
 * Access the value of a 32-bit atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE uint32_t relaxedLoad32(const Atomic32 *atom)
{
  return atomic_read(&atom->value);
}

/**
 * Access the value of a 64-bit atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE uint64_t relaxedLoad64(const Atomic64 *atom)
{
  return atomic64_read(&atom->value);
}

/**
 * Access the value of a boolean atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom  a pointer to the atomic variable to access
 *
 * @return the value that was in the atom at the moment it was accessed
 **/
static INLINE bool relaxedLoadBool(const AtomicBool *atom)
{
  return (relaxedLoad32(&atom->value) > 0);
}

/**
 * Set the value of a 32-bit atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void relaxedStore32(Atomic32 *atom, uint32_t newValue)
{
  atomic_set(&atom->value, newValue);
}

/**
 * Set the value of a 64-bit atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void relaxedStore64(Atomic64 *atom, uint64_t newValue)
{
  atomic64_set(&atom->value, newValue);
}

/**
 * Set the value of a boolean atomic variable using relaxed memory order,
 * without any compiler or CPU fences.
 *
 * @param atom      a pointer to the atomic variable to modify
 * @param newValue  the value to assign to the atomic variable
 **/
static INLINE void relaxedStoreBool(AtomicBool *atom, bool newValue)
{
  relaxedStore32(&atom->value, (newValue ? 1 : 0));
}

/**
 * Non-atomically add a 32-bit signed delta to a 32-bit atomic variable,
 * without any compiler or CPU fences.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return       the new value of the atom after the add operation
 **/
static INLINE uint32_t relaxedAdd32(Atomic32 *atom, int32_t delta)
{
  uint32_t newValue = (relaxedLoad32(atom) + delta);
  relaxedStore32(atom, newValue);
  return newValue;
}

/**
 * Non-atomically add a 64-bit signed delta to a 64-bit atomic variable,
 * without any compiler or CPU fences.
 *
 * @param atom   a pointer to the atomic variable
 * @param delta  the value to be added (or subtracted) from the variable
 *
 * @return       the new value of the atom after the add operation
 **/
static INLINE uint64_t relaxedAdd64(Atomic64 *atom, int64_t delta)
{
  uint64_t newValue = (relaxedLoad64(atom) + delta);
  relaxedStore64(atom, newValue);
  return newValue;
}

#endif /* ATOMIC_H */
