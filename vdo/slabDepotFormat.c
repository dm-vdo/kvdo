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
 *
 * $Id: //eng/linux-vdo/src/c++/vdo/base/slabDepotFormat.c#17 $
 */

#include "slabDepotFormat.h"

#include "buffer.h"
#include "logger.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "numUtils.h"
#include "packedReferenceBlock.h"
#include "slabJournalFormat.h"
#include "statusCodes.h"
#include "types.h"

const struct header VDO_SLAB_DEPOT_HEADER_2_0 = {
	.id = VDO_SLAB_DEPOT,
	.version = {
		.major_version = 2,
		.minor_version = 0,
	},
	.size = sizeof(struct slab_depot_state_2_0),
};

/**********************************************************************/
slab_count_t __must_check
compute_vdo_slab_count(physical_block_number_t first_block,
		       physical_block_number_t last_block,
		       unsigned int slab_size_shift)
{
	block_count_t data_blocks = last_block - first_block;
	return (slab_count_t) (data_blocks >> slab_size_shift);
}

/**********************************************************************/
size_t get_vdo_slab_depot_encoded_size(void)
{
	return VDO_ENCODED_HEADER_SIZE + sizeof(struct slab_depot_state_2_0);
}

/**
 * Encode a slab config into a buffer.
 *
 * @param config  The config structure to encode
 * @param buffer  A buffer positioned at the start of the encoding
 *
 * @return UDS_SUCCESS or an error code
 **/
static int encode_slab_config(const struct slab_config *config,
			      struct buffer *buffer)
{
	int result = put_uint64_le_into_buffer(buffer, config->slab_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->data_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->reference_count_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, config->slab_journal_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
				           config->slab_journal_flushing_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer,
				           config->slab_journal_blocking_threshold);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return put_uint64_le_into_buffer(buffer,
				         config->slab_journal_scrubbing_threshold);
}

/**********************************************************************/
int encode_vdo_slab_depot_state_2_0(struct slab_depot_state_2_0 state,
				    struct buffer *buffer)
{
	size_t initial_length, encoded_size;

	int result = encode_vdo_header(&VDO_SLAB_DEPOT_HEADER_2_0, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = encode_slab_config(&state.slab_config, buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_uint64_le_into_buffer(buffer, state.last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = put_byte(buffer, state.zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	encoded_size = content_length(buffer) - initial_length;
	return ASSERT(VDO_SLAB_DEPOT_HEADER_2_0.size == encoded_size,
		      "encoded block map component size must match header size");
}

/**
 * Decode a slab config from a buffer.
 *
 * @param buffer  A buffer positioned at the start of the encoding
 * @param config  The config structure to receive the decoded values
 *
 * @return UDS_SUCCESS or an error code
 **/
static int decode_slab_config(struct buffer *buffer,
			      struct slab_config *config)
{
	block_count_t count;
	int result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->data_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->reference_count_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocks = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_flushing_threshold = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_blocking_threshold = count;

	result = get_uint64_le_from_buffer(buffer, &count);
	if (result != UDS_SUCCESS) {
		return result;
	}
	config->slab_journal_scrubbing_threshold = count;

	return UDS_SUCCESS;
}

/**********************************************************************/
int decode_vdo_slab_depot_state_2_0(struct buffer *buffer,
				    struct slab_depot_state_2_0 *state)
{
	struct header header;
	int result;
	size_t initial_length, decoded_size;
	struct slab_config slab_config;
	physical_block_number_t first_block, last_block;
	zone_count_t zone_count;

	result = decode_vdo_header(buffer, &header);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = validate_vdo_header(&VDO_SLAB_DEPOT_HEADER_2_0, &header, true,
				     __func__);
	if (result != VDO_SUCCESS) {
		return result;
	}

	initial_length = content_length(buffer);

	result = decode_slab_config(buffer, &slab_config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &first_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_uint64_le_from_buffer(buffer, &last_block);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_byte(buffer, &zone_count);
	if (result != UDS_SUCCESS) {
		return result;
	}

	decoded_size = initial_length - content_length(buffer);
	result = ASSERT(VDO_SLAB_DEPOT_HEADER_2_0.size == decoded_size,
			"decoded slab depot component size must match header size");
	if (result != UDS_SUCCESS) {
		return result;
	}

	*state = (struct slab_depot_state_2_0) {
		.slab_config = slab_config,
		.first_block = first_block,
		.last_block = last_block,
		.zone_count = zone_count,
	};

	return VDO_SUCCESS;
}

/**********************************************************************/
int configure_vdo_slab_depot(block_count_t block_count,
			     physical_block_number_t first_block,
			     struct slab_config slab_config,
			     zone_count_t zone_count,
			     struct slab_depot_state_2_0 *state)
{
	block_count_t total_slab_blocks, total_data_blocks;
	size_t slab_count;
	physical_block_number_t last_block;
	block_count_t slab_size = slab_config.slab_blocks;

	uds_log_debug("slabDepot configure_vdo_slab_depot(block_count=%llu, first_block=%llu, slab_size=%llu, zone_count=%u)",
		      (unsigned long long) block_count,
		      (unsigned long long) first_block,
		      (unsigned long long) slab_size,
		      zone_count);

	// We do not allow runt slabs, so we waste up to a slab's worth.
	slab_count = (block_count / slab_size);
	if (slab_count == 0) {
		return VDO_NO_SPACE;
	}

	if (slab_count > MAX_VDO_SLABS) {
		return VDO_TOO_MANY_SLABS;
	}

	total_slab_blocks = slab_count * slab_config.slab_blocks;
	total_data_blocks = slab_count * slab_config.data_blocks;
	last_block = first_block + total_slab_blocks;

	*state = (struct slab_depot_state_2_0) {
		.slab_config = slab_config,
		.first_block = first_block,
		.last_block = last_block,
		.zone_count = zone_count,
	};

	uds_log_debug("slab_depot last_block=%llu, total_data_blocks=%llu, slab_count=%zu, left_over=%llu",
		      (unsigned long long) last_block,
		      (unsigned long long) total_data_blocks,
		      slab_count,
		      (unsigned long long) (block_count - (last_block - first_block)));

	return VDO_SUCCESS;
}

/**********************************************************************/
int configure_vdo_slab(block_count_t slab_size,
		       block_count_t slab_journal_blocks,
		       struct slab_config *slab_config)
{
	block_count_t ref_blocks, meta_blocks, data_blocks;
	block_count_t flushing_threshold, remaining, blocking_threshold;
	block_count_t minimal_extra_space, scrubbing_threshold;

	if (slab_journal_blocks >= slab_size) {
		return VDO_BAD_CONFIGURATION;
	}

	/*
	 * This calculation should technically be a recurrence, but the total
	 * number of metadata blocks is currently less than a single block of
	 * ref_counts, so we'd gain at most one data block in each slab with
	 * more iteration.
	 */
	ref_blocks =
		vdo_get_saved_reference_count_size(slab_size - slab_journal_blocks);
	meta_blocks = (ref_blocks + slab_journal_blocks);

	// Make sure test code hasn't configured slabs to be too small.
	if (meta_blocks >= slab_size) {
		return VDO_BAD_CONFIGURATION;
	}

	/*
	 * If the slab size is very small, assume this must be a unit test and
	 * override the number of data blocks to be a power of two (wasting
	 * blocks in the slab). Many tests need their data_blocks fields to be
	 * the exact capacity of the configured volume, and that used to fall
	 * out since they use a power of two for the number of data blocks, the
	 * slab size was a power of two, and every block in a slab was a data
	 * block.
	 *
	 * XXX Try to figure out some way of structuring testParameters and
	 * unit tests so this hack isn't needed without having to edit several
	 * unit tests every time the metadata size changes by one block.
	 */
	data_blocks = slab_size - meta_blocks;
	if ((slab_size < 1024) && !is_power_of_2(data_blocks)) {
		data_blocks = ((block_count_t) 1 << log_base_two(data_blocks));
	}

	/*
	 * Configure the slab journal thresholds. The flush threshold is 168 of
	 * 224 blocks in production, or 3/4ths, so we use this ratio for all
	 * sizes.
	 */
	flushing_threshold = ((slab_journal_blocks * 3) + 3) / 4;
	/*
	 * The blocking threshold should be far enough from the the flushing
	 * threshold to not produce delays, but far enough from the end of the
	 * journal to allow multiple successive recovery failures.
	 */
	remaining = slab_journal_blocks - flushing_threshold;
	blocking_threshold =
		flushing_threshold + ((remaining * 5) / 7);
	/*
	 * The scrubbing threshold should be at least 2048 entries before the
	 * end of the journal.
	 */
	minimal_extra_space =
		1 + (MAXIMUM_VDO_USER_VIOS / VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK);
	scrubbing_threshold = blocking_threshold;
	if (slab_journal_blocks > minimal_extra_space) {
		scrubbing_threshold = slab_journal_blocks - minimal_extra_space;
	}
	if (blocking_threshold > scrubbing_threshold) {
		blocking_threshold = scrubbing_threshold;
	}

	*slab_config = (struct slab_config) {
		.slab_blocks = slab_size,
		.data_blocks = data_blocks,
		.reference_count_blocks = ref_blocks,
		.slab_journal_blocks = slab_journal_blocks,
		.slab_journal_flushing_threshold = flushing_threshold,
		.slab_journal_blocking_threshold = blocking_threshold,
		.slab_journal_scrubbing_threshold = scrubbing_threshold};
	return VDO_SUCCESS;
}

/**********************************************************************/
block_count_t vdo_get_saved_reference_count_size(block_count_t block_count)
{
	return compute_bucket_count(block_count, COUNTS_PER_BLOCK);
}
