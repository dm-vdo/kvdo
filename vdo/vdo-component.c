// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "vdo-component.h"

#include "buffer.h"
#include "logger.h"
#include "permassert.h"

#include "constants.h"
#include "header.h"
#include "num-utils.h"
#include "slab-depot-format.h"
#include "status-codes.h"
#include "types.h"

/*
 * The current version for the data encoded in the super block. This must
 * be changed any time there is a change to encoding of the component data
 * of any VDO component.
 */
static const struct version_number VDO_COMPONENT_DATA_41_0 = {
	.major_version = 41,
	.minor_version = 0,
};

/*
 * A packed, machine-independent, on-disk representation of the vdo_config
 * in the VDO component data in the super block.
 */
struct packed_vdo_config {
	__le64 logical_blocks;
	__le64 physical_blocks;
	__le64 slab_size;
	__le64 recovery_journal_size;
	__le64 slab_journal_blocks;
} __packed;

/*
 * A packed, machine-independent, on-disk representation of version 41.0
 * of the VDO component data in the super block.
 */
struct packed_vdo_component_41_0 {
	__le32 state;
	__le64 complete_recoveries;
	__le64 read_only_recoveries;
	struct packed_vdo_config config;
	__le64 nonce;
} __packed;

/**
 * vdo_get_component_encoded_size() - Get the size of the encoded state of the
 *                                    vdo itself.
 *
 * Return: The encoded size of the vdo's state.
 */
size_t vdo_get_component_encoded_size(void)
{
	return (sizeof(struct packed_version_number)
		+ sizeof(struct packed_vdo_component_41_0));
}

/**
 * pack_vdo_config() - Convert a vdo_config to its packed on-disk
 *                     representation.
 * @config: The vdo config to convert.
 *
 * Return: The platform-independent representation of the config.
 */
static inline struct packed_vdo_config
pack_vdo_config(struct vdo_config config)
{
	return (struct packed_vdo_config) {
		.logical_blocks = __cpu_to_le64(config.logical_blocks),
		.physical_blocks = __cpu_to_le64(config.physical_blocks),
		.slab_size = __cpu_to_le64(config.slab_size),
		.recovery_journal_size =
			__cpu_to_le64(config.recovery_journal_size),
		.slab_journal_blocks =
			__cpu_to_le64(config.slab_journal_blocks),
	};
}

/**
 * pack_vdo_component() - Convert a vdo_component to its packed on-disk
 *                        representation.
 * @component: The VDO component data to convert.
 *
 * Return: The platform-independent representation of the component.
 */
static inline struct packed_vdo_component_41_0
pack_vdo_component(const struct vdo_component component)
{
	return (struct packed_vdo_component_41_0) {
		.state = __cpu_to_le32(component.state),
		.complete_recoveries =
			__cpu_to_le64(component.complete_recoveries),
		.read_only_recoveries =
			__cpu_to_le64(component.read_only_recoveries),
		.config = pack_vdo_config(component.config),
		.nonce = __cpu_to_le64(component.nonce),
	};
}

/**
 * vdo_encode_component() - Encode the component data for the vdo itself.
 * @component: The component structure.
 * @buffer: The buffer in which to encode the vdo.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_encode_component(struct vdo_component component, struct buffer *buffer)
{
	int result;
	struct packed_vdo_component_41_0 packed;

	result = vdo_encode_version_number(VDO_COMPONENT_DATA_41_0, buffer);
	if (result != VDO_SUCCESS) {
		return result;
	}

	packed = pack_vdo_component(component);
	return put_bytes(buffer, sizeof(packed), &packed);
}

/**
 * unpack_vdo_config() - Convert a packed_vdo_config to its native in-memory
 *                       representation.
 * @config: The packed vdo config to convert.
 *
 * Return: The native in-memory representation of the vdo config.
 */
static inline struct vdo_config
unpack_vdo_config(struct packed_vdo_config config)
{
	return (struct vdo_config) {
		.logical_blocks = __le64_to_cpu(config.logical_blocks),
		.physical_blocks = __le64_to_cpu(config.physical_blocks),
		.slab_size = __le64_to_cpu(config.slab_size),
		.recovery_journal_size =
			__le64_to_cpu(config.recovery_journal_size),
		.slab_journal_blocks =
			__le64_to_cpu(config.slab_journal_blocks),
	};
}

/**
 * unpack_vdo_component_41_0() - Convert a packed_vdo_component_41_0 to its
 *                               native in-memory representation.
 * @component: The packed vdo component data to convert.
 *
 * Return: The native in-memory representation of the component.
 */
static inline struct vdo_component
unpack_vdo_component_41_0(struct packed_vdo_component_41_0 component)
{
	return (struct vdo_component) {
		.state = __le32_to_cpu(component.state),
		.complete_recoveries =
			__le64_to_cpu(component.complete_recoveries),
		.read_only_recoveries =
			__le64_to_cpu(component.read_only_recoveries),
		.config = unpack_vdo_config(component.config),
		.nonce = __le64_to_cpu(component.nonce),
	};
}

/**
 * vdo_decode_component_41_0() - Decode the version 41.0 component data for
 *                               the vdo itself from a buffer.
 * @buffer: A buffer positioned at the start of the encoding.
 * @component: The component structure to receive the decoded values.
 *
 * Return: VDO_SUCCESS or an error.
 */
static int __must_check
vdo_decode_component_41_0(struct buffer *buffer,
			  struct vdo_component *component)
{
	struct packed_vdo_component_41_0 packed;
	int result = get_bytes_from_buffer(buffer, sizeof(packed), &packed);

	if (result != UDS_SUCCESS) {
		return result;
	}

	*component = unpack_vdo_component_41_0(packed);
	return VDO_SUCCESS;
}

/**
 * vdo_decode_component() - Decode the component data for the vdo itself from
 *                          the component data buffer in the super block.
 * @buffer: The buffer being decoded.
 * @component: The component structure in which to store the result of a
 *             successful decode.
 *
 * Return: VDO_SUCCESS or an error.
 */
int vdo_decode_component(struct buffer *buffer,
			 struct vdo_component *component)
{
	struct version_number version;
	int result = vdo_decode_version_number(buffer, &version);

	if (result != VDO_SUCCESS) {
		return result;
	}

	result = vdo_validate_version(version, VDO_COMPONENT_DATA_41_0,
				      "VDO component data");
	if (result != VDO_SUCCESS) {
		return result;
	}

	return vdo_decode_component_41_0(buffer, component);
}

/**
 * vdo_validate_config() - Validate constraints on a VDO config.
 * @config: The VDO config.
 * @physical_block_count: The minimum block count of the underlying storage.
 * @logical_block_count: The expected logical size of the VDO, or 0 if the
 *                       logical size may be unspecified.
 *
 * Return: A success or error code.
 */
int vdo_validate_config(const struct vdo_config *config,
			block_count_t physical_block_count,
			block_count_t logical_block_count)
{
	struct slab_config slab_config;
	int result = ASSERT(config->slab_size > 0, "slab size unspecified");

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_2(config->slab_size),
			"slab size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_size <= (1 << MAX_VDO_SLAB_BITS),
			"slab size must be less than or equal to 2^%d",
			MAX_VDO_SLAB_BITS);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_journal_blocks >= MINIMUM_VDO_SLAB_JOURNAL_BLOCKS,
		       "slab journal size meets minimum size");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->slab_journal_blocks <= config->slab_size,
			"slab journal size is within expected bound");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = vdo_configure_slab(config->slab_size,
				    config->slab_journal_blocks,
				    &slab_config);
	if (result != VDO_SUCCESS) {
		return result;
	}

	result = ASSERT((slab_config.data_blocks >= 1),
			"slab must be able to hold at least one block");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physical_blocks > 0,
			"physical blocks unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->physical_blocks <= MAXIMUM_VDO_PHYSICAL_BLOCKS,
			"physical block count %llu exceeds maximum %llu",
			(unsigned long long) config->physical_blocks,
			(unsigned long long) MAXIMUM_VDO_PHYSICAL_BLOCKS);
	if (result != UDS_SUCCESS) {
		return VDO_OUT_OF_RANGE;
	}

	if (physical_block_count != config->physical_blocks) {
		uds_log_error("A physical size of %llu blocks was specified, not the %llu blocks configured in the vdo super block",
			      (unsigned long long) physical_block_count,
			      (unsigned long long) config->physical_blocks);
		return VDO_PARAMETER_MISMATCH;
	}

	if (logical_block_count > 0) {
		result = ASSERT((config->logical_blocks > 0),
				"logical blocks unspecified");
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (logical_block_count != config->logical_blocks) {
			uds_log_error("A logical size of %llu blocks was specified, but that differs from the %llu blocks configured in the vdo super block",
			      (unsigned long long) logical_block_count,
			      (unsigned long long) config->logical_blocks);
			return VDO_PARAMETER_MISMATCH;
		}
	}

	result = ASSERT(config->logical_blocks <= MAXIMUM_VDO_LOGICAL_BLOCKS,
			"logical blocks too large");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(config->recovery_journal_size > 0,
			"recovery journal size unspecified");
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = ASSERT(is_power_of_2(config->recovery_journal_size),
			"recovery journal size must be a power of two");
	if (result != UDS_SUCCESS) {
		return result;
	}

	return result;
}
