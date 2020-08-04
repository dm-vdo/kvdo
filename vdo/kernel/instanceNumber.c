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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/instanceNumber.c#9 $
 */

#include "instanceNumber.h"

#include <linux/bitops.h>
#include <linux/mutex.h>

#include "memoryAlloc.h"
#include "numUtils.h"
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
 * Return the number of bytes needed to store a bit array of the specified
 * capacity in an array of unsigned longs.
 *
 * @param bit_count  The number of bits the array must hold
 *
 * @return the number of bytes needed for the array reperesentation
 **/
static size_t get_bit_array_size(unsigned int bit_count)
{
	// Round up to a multiple of the word size and convert to a byte count.
	return (compute_bucket_count(bit_count, BITS_PER_LONG) *
		sizeof(unsigned long));
}

/**
 * Re-allocate the bitmap word array so there will more instance numbers that
 * can be allocated. Since the array is initially NULL, this also initializes
 * the array the first time we allocate an instance number.
 *
 * @return UDS_SUCCESS or an error code from the allocation
 **/
static int grow_bit_array(void)
{
	unsigned int new_count = max(bit_count + BIT_COUNT_INCREMENT,
				     (unsigned int) BIT_COUNT_MINIMUM);
	unsigned long *new_words;
	int result = reallocate_memory(words,
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

/**********************************************************************/
static int allocate_kvdo_instance_locked(unsigned int *instance_ptr)
{
	// If there are no unallocated instances, grow the bit array.
	if (instance_count >= bit_count) {
		int result = grow_bit_array();

		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	// There must be a zero bit somewhere now. Find it, starting just after
	// the last instance allocated.
	unsigned int instance =
		find_next_zero_bit(words, bit_count, next_instance);
	if (instance >= bit_count) {
		// Nothing free after next_instance, so wrap around to instance
		// zero.
		instance = find_first_zero_bit(words, bit_count);
		int result = ASSERT(instance < bit_count,
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

/**********************************************************************/
int allocate_kvdo_instance(unsigned int *instance_ptr)
{
	mutex_lock(&instance_number_lock);
	int result = allocate_kvdo_instance_locked(instance_ptr);

	mutex_unlock(&instance_number_lock);
	return result;
}

/**********************************************************************/
void release_kvdo_instance(unsigned int instance)
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

/**********************************************************************/
void initialize_instance_number_tracking(void)
{
	mutex_init(&instance_number_lock);
}

/**********************************************************************/
void clean_up_instance_number_tracking(void)
{
	ASSERT_LOG_ONLY(instance_count == 0,
			"should have no instance numbers still in use, but have %u",
			instance_count);
	FREE(words);
	words = NULL;
	bit_count = 0;
	instance_count = 0;
	next_instance = 0;
	mutex_destroy(&instance_number_lock);
}
