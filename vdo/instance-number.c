// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "instance-number.h"

#include <linux/bitops.h>
#include <linux/mutex.h>

#include "memory-alloc.h"
#include "num-utils.h"
#include "permassert.h"

/*
 * Track in-use instance numbers using a flat bit array.
 *
 * O(n) run time isn't ideal, but if we have 1000 VDO devices in use
 * simultaneously we still only need to scan 16 words, so it's not
 * likely to be a big deal compared to other resource usage.
 */

enum {
	/**
	 * This minimum size for the bit array creates a numbering space of
	 * 0-999, which allows successive starts of the same volume to have
	 * different instance numbers in any reasonably-sized test. Changing
	 * instances on restart allows vdoMonReport to detect that the
	 * ephemeral stats have reset to zero.
	 **/
	BIT_COUNT_MINIMUM = 1000,
	/** Grow the bit array by this many bits when needed */
	BIT_COUNT_INCREMENT = 100,
};

static struct mutex instance_number_lock;
static unsigned int bit_count;
static unsigned long *words;
static unsigned int instance_count;
static unsigned int next_instance;

/**
 * get_bit_array_size() - Return the number of bytes needed to store a
 *                        bit array of the specified capacity in an
 *                        array of unsigned longs.
 * @bit_count: The number of bits the array must hold.
 *
 * Return: the number of bytes needed for the array reperesentation.
 */
static size_t get_bit_array_size(unsigned int bit_count)
{
	/* Round up to a multiple of the word size and convert to a byte count. */
	return (DIV_ROUND_UP(bit_count, BITS_PER_LONG) *
		sizeof(unsigned long));
}

/**
 * grow_bit_array() - Re-allocate the bitmap word array so there will
 *                    more instance numbers that can be allocated.
 *
 * Since the array is initially NULL, this also initializes the array
 * the first time we allocate an instance number.
 *
 * Return: UDS_SUCCESS or an error code from the allocation
 */
static int grow_bit_array(void)
{
	unsigned int new_count = max(bit_count + BIT_COUNT_INCREMENT,
				     (unsigned int) BIT_COUNT_MINIMUM);
	unsigned long *new_words;
	int result = uds_reallocate_memory(words,
					   get_bit_array_size(bit_count),
					   get_bit_array_size(new_count),
					   "instance number bit array",
					   &new_words);
	if (result != UDS_SUCCESS) {
		return result;
	}

	bit_count = new_count;
	words = new_words;
	return UDS_SUCCESS;
}

static int vdo_allocate_instance_locked(unsigned int *instance_ptr)
{
	unsigned int instance;
	/* If there are no unallocated instances, grow the bit array. */
	if (instance_count >= bit_count) {
		int result = grow_bit_array();

		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	/*
	 * There must be a zero bit somewhere now. Find it, starting just after
	 * the last instance allocated.
	 */
	instance = find_next_zero_bit(words, bit_count, next_instance);
	if (instance >= bit_count) {
		int result;
		/*
		 * Nothing free after next_instance, so wrap around to instance
		 * zero.
		 */
		instance = find_first_zero_bit(words, bit_count);
		result = ASSERT(instance < bit_count,
				"impossibly, no zero bit found");
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	__set_bit(instance, words);
	instance_count += 1;
	next_instance = instance + 1;
	*instance_ptr = instance;
	return UDS_SUCCESS;
}

/**
 * vdo_allocate_instance() - Allocate an instance number.
 * @instance_ptr: An integer to hold the allocated instance number.
 *
 * Return: UDS_SUCCESS or an error code.
 */
int vdo_allocate_instance(unsigned int *instance_ptr)
{
	int result;

	mutex_lock(&instance_number_lock);
	result = vdo_allocate_instance_locked(instance_ptr);

	mutex_unlock(&instance_number_lock);
	return result;
}

/**
 * vdo_release_instance() - Release an instance number previously allocated.
 * @instance: The instance number to release.
 */
void vdo_release_instance(unsigned int instance)
{
	mutex_lock(&instance_number_lock);
	if (instance >= bit_count) {
		ASSERT_LOG_ONLY(false,
				"instance number %u must be less than bit count %u",
				instance,
				bit_count);
	} else if (test_bit(instance, words) == 0) {
		ASSERT_LOG_ONLY(false,
				"instance number %u must be allocated",
				instance);
	} else {
		__clear_bit(instance, words);
		instance_count -= 1;
	}
	mutex_unlock(&instance_number_lock);
}

/**
 * vdo_initialize_instance_number_tracking() - Initialize the instance-number
 *                                             tracking data structures.
 */
void vdo_initialize_instance_number_tracking(void)
{
	mutex_init(&instance_number_lock);
}

/**
 * vdo_clean_up_instance_number_tracking() - Free up the instance-number
 *                                           tracking data structures.
 */
void vdo_clean_up_instance_number_tracking(void)
{
	ASSERT_LOG_ONLY(instance_count == 0,
			"should have no instance numbers still in use, but have %u",
			instance_count);
	UDS_FREE(words);
	words = NULL;
	bit_count = 0;
	instance_count = 0;
	next_instance = 0;
	mutex_destroy(&instance_number_lock);
}
