/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef MEMORY_ALLOC_H
#define MEMORY_ALLOC_H 1

#include "compiler.h"
#include "cpu.h"
#include "permassert.h"
#include "type-defs.h"

#include <linux/io.h> /* for PAGE_SIZE */
#include "thread-registry.h"

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
int __must_check uds_allocate_memory(size_t size,
				     size_t align,
				     const char *what,
				     void *ptr);

/**
 * Free storage
 *
 * @param ptr  The memory to be freed
 **/
void uds_free_memory(void *ptr);

/**
 * Free memory allocated with UDS_ALLOCATE().
 *
 * @param PTR  Pointer to the memory to free
 **/
#define UDS_FREE(PTR) uds_free_memory(PTR)

/**
 * Null out a reference and return a copy of the referenced object.
 *
 * @param ptr_ptr  A pointer to the reference to NULL out
 *
 * @return A copy of the reference
 **/
static INLINE void *uds_forget(void **ptr_ptr)
{
        void *ptr = *ptr_ptr;

        *ptr_ptr = NULL;
        return ptr;
}

/**
 * Null out a pointer and return a copy to it. This macro should be used when
 * passing a pointer to a function for which it is not safe to access the
 * pointer once the function returns.
 *
 * @param ptr  The pointer to NULL out
 *
 * @return A copy of the NULLed out pointer
 **/
#define UDS_FORGET(ptr) uds_forget((void **) &(ptr))

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
static INLINE int uds_do_allocation(size_t count,
				    size_t size,
				    size_t extra,
				    size_t align,
				    const char *what,
				    void *ptr)
{
	size_t total_size = count * size + extra;
	/* Overflow check: */
	if ((size > 0) && (count > ((SIZE_MAX - extra) / size))) {
		/*
		 * This is kind of a hack: We rely on the fact that SIZE_MAX
		 * would cover the entire address space (minus one byte) and
		 * thus the system can never allocate that much and the call
		 * will always fail.  So we can report an overflow as "out of
		 * memory" by asking for "merely" SIZE_MAX bytes.
		 */
		total_size = SIZE_MAX;
	}

	return uds_allocate_memory(total_size, align, what, ptr);
}

/**
 * Reallocate dynamically allocated memory.  There are no alignment guarantees
 * for the reallocated memory. If the new memory is larger than the old memory,
 * the new space will be zeroed.
 *
 * @param ptr       The memory to reallocate.
 * @param old_size  The old size of the memory
 * @param size      The new size to allocate
 * @param what      What is being allocated (for error logging)
 * @param new_ptr   A pointer to hold the reallocated pointer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check uds_reallocate_memory(void *ptr,
				       size_t old_size,
				       size_t size,
				       const char *what,
				       void *new_ptr);

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
#define UDS_ALLOCATE(COUNT, TYPE, WHAT, PTR) \
	uds_do_allocation(COUNT, sizeof(TYPE), 0, __alignof__(TYPE), WHAT, PTR)

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
#define UDS_ALLOCATE_EXTENDED(TYPE1, COUNT, TYPE2, WHAT, PTR)            \
	__extension__({                                                  \
		int _result;						 \
		TYPE1 **_ptr = (PTR);                                    \
		STATIC_ASSERT(__alignof__(TYPE1) >= __alignof__(TYPE2)); \
		_result = uds_do_allocation(COUNT,                       \
					    sizeof(TYPE2),               \
					    sizeof(TYPE1),               \
					    __alignof__(TYPE1),          \
					    WHAT,                        \
					    _ptr);                       \
		_result;                                                 \
	})

/**
 * Allocate one or more elements of the indicated type, aligning them
 * on the boundary that will allow them to be used in I/O, logging an
 * error if the allocation fails. The memory will be zeroed.
 *
 * @param COUNT  The number of objects to allocate
 * @param TYPE   The type of objects to allocate
 * @param WHAT   What is being allocated (for error logging)
 * @param PTR    A pointer to hold the allocated memory
 *
 * @return UDS_SUCCESS or an error code
 **/
#define UDS_ALLOCATE_IO_ALIGNED(COUNT, TYPE, WHAT, PTR) \
	uds_do_allocation(COUNT, sizeof(TYPE), 0, PAGE_SIZE, WHAT, PTR)

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
static INLINE int __must_check uds_allocate_cache_aligned(size_t size,
							  const char *what,
							  void *ptr)
{
	return uds_allocate_memory(size, CACHE_LINE_BYTES, what, ptr);
}

/**
 * Allocate storage based on memory size, failing immediately if the required
 * memory is not available.  The memory will be zeroed.
 *
 * @param size  The size of an object.
 * @param what  What is being allocated (for error logging)
 *
 * @return pointer to the allocated memory, or NULL if the required space is
 *         not available.
 **/
void *__must_check uds_allocate_memory_nowait(size_t size, const char *what);

/**
 * Allocate one element of the indicated type immediately, failing if the
 * required memory is not immediately available.
 *
 * @param TYPE   The type of objects to allocate
 * @param WHAT   What is being allocated (for error logging)
 *
 * @return pointer to the memory, or NULL if the memory is not available.
 **/
#define UDS_ALLOCATE_NOWAIT(TYPE, WHAT) \
	uds_allocate_memory_nowait(sizeof(TYPE), WHAT)

/**
 * Duplicate a string.
 *
 * @param string     The string to duplicate
 * @param what       What is being allocated (for error logging)
 * @param new_string A pointer to hold the duplicated string
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check uds_duplicate_string(const char *string,
				      const char *what,
				      char **new_string);

/**
 * Wrapper which permits freeing a const pointer.
 *
 * @param pointer  the pointer to be freed
 **/
static INLINE void uds_free_const(const void *pointer)
{
	union {
		const void *const_p;
		void *not_const;
	} u = { .const_p = pointer };
	UDS_FREE(u.not_const);
}

/**
 * Perform termination of the memory allocation subsystem.
 **/
void uds_memory_exit(void);

/**
 * Perform initialization of the memory allocation subsystem.
 **/
void uds_memory_init(void);

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
 * @param new_thread  registered_thread structure to use for the current thread
 * @param flag_ptr    Location of the allocation-allowed flag
 **/
void uds_register_allocating_thread(struct registered_thread *new_thread,
				    const bool *flag_ptr);

/**
 * Unregister the current thread as an allocating thread.
 **/
void uds_unregister_allocating_thread(void);

/**
 * Get the memory statistics.
 *
 * @param bytes_used      A pointer to hold the number of bytes in use
 * @param peak_bytes_used A pointer to hold the maximum value bytes_used has
 *                      attained
 **/
void get_uds_memory_stats(uint64_t *bytes_used, uint64_t *peak_bytes_used);

/**
 * Report stats on any allocated memory that we're tracking.
 *
 * Not all allocation types are guaranteed to be tracked in bytes
 * (e.g., bios).
 **/
void report_uds_memory_usage(void);


#endif /* MEMORY_ALLOC_H */
