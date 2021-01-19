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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/physicalLayer.h#39 $
 */

#ifndef PHYSICAL_LAYER_H
#define PHYSICAL_LAYER_H

#include "types.h"

static const crc32_checksum_t INITIAL_CHECKSUM = 0xffffffff;

enum {
	/* The size of a CRC-32 checksum */
	CHECKSUM_SIZE = sizeof(crc32_checksum_t),
};

/**
 * A function to destroy a physical layer and NULL out the reference to it.
 *
 * @param layer_ptr  A pointer to the layer to destroy
 **/
typedef void layer_destructor(PhysicalLayer **layer_ptr);

/**
 * A function to report the block count of a physicalLayer.
 *
 * @param layer  The layer
 *
 * @return The block count of the layer
 **/
typedef block_count_t block_count_getter(PhysicalLayer *layer);

/**
 * A function which can allocate a buffer suitable for use in an
 * extent_reader or extent_writer.
 *
 * @param [in]  layer       The physical layer in question
 * @param [in]  bytes       The size of the buffer, in bytes.
 * @param [in]  why         The occasion for allocating the buffer
 * @param [out] buffer_ptr  A pointer to hold the buffer
 *
 * @return a success or error code
 **/
typedef int buffer_allocator(PhysicalLayer *layer,
			     size_t bytes,
			     const char *why,
			     char **buffer_ptr);

/**
 * A function which can read an extent from a physicalLayer.
 *
 * @param [in]  layer       The physical layer from which to read
 * @param [in]  startBlock  The physical block number of the start of the
 *                          extent
 * @param [in]  blockCount  The number of blocks in the extent
 * @param [out] buffer      A buffer to hold the extent
 *
 * @return a success or error code
 **/
typedef int extent_reader(PhysicalLayer *layer,
			  physical_block_number_t startBlock,
			  size_t blockCount,
			  char *buffer);

/**
 * A function which can write an extent to a physicalLayer.
 *
 * @param [in]  layer          The physical layer to which to write
 * @param [in]  startBlock     The physical block number of the start of the
 *                             extent
 * @param [in]  blockCount     The number of blocks in the extent
 * @param [in]  buffer         The buffer which contains the data
 *
 * @return a success or error code
 **/
typedef int extent_writer(PhysicalLayer *layer,
			  physical_block_number_t startBlock,
			  size_t blockCount,
			  char *buffer);

/**
 * A function to allocate an allocating_vio for compressed writes.
 *
 * @param [in]  layer               The physical layer
 * @param [in]  parent              The parent of this vio
 * @param [in]  data                The buffer
 * @param [out] allocating_vio_ptr  A pointer to hold the new allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int compressed_write_vio_creator(PhysicalLayer *layer,
				         void *parent,
				         char *data,
				         struct allocating_vio **allocating_vio_ptr);

/**
 * A function to destroy a vio. The pointer to the vio will be nulled out.
 *
 * @param vio_ptr  A pointer to the vio to destroy
 **/
typedef void vio_destructor(struct vio **vio_ptr);

/**
 * A function to zero the contents of a data_vio.
 *
 * @param dataVIO  The data_vio to zero
 **/
typedef async_data_operation data_vio_zeroer;

/**
 * A function to copy the contents of a data_vio into another data_vio.
 *
 * @param source       The dataVIO to copy from
 * @param destination  The dataVIO to copy to
 **/
typedef void data_copier(struct data_vio *source,
			 struct data_vio *destination);

/**
 * A function to apply a partial write to a data_vio which has completed the
 * read portion of a read-modify-write operation.
 *
 * @param dataVIO  The dataVIO to modify
 **/
typedef async_data_operation data_modifier;

/**
 * A function to asynchronously hash the block data, setting the chunk name of
 * the data_vio. This is asynchronous to allow the computation to be done on
 * different threads.
 *
 * @param dataVIO  The data_vio to hash
 **/
typedef async_data_operation data_hasher;

/**
 * A function to determine whether a block is a duplicate. This function
 * expects the 'physical' field of the data_vio to be set to the physical block
 * where the block will be written if it is not a duplicate. If the block does
 * turn out to be a duplicate, the data_vio's 'isDuplicate' field will be set to
 * true, and the data_vio's 'advice' field will be set to the physical block and
 * mapping state of the already stored copy of the block.
 *
 * @param dataVIO  The data_vio containing the block to check.
 **/
typedef async_data_operation duplication_checker;

/**
 * A function to verify the duplication advice by examining an already-stored
 * data block. This function expects the 'physical' field of the data_vio to be
 * set to the physical block where the block will be written if it is not a
 * duplicate, and the 'duplicate' field to be set to the physical block and
 * mapping state where a copy of the data may already exist. If the block is
 * not a duplicate, the data_vio's 'isDuplicate' field will be cleared.
 *
 * @param dataVIO  The dataVIO containing the block to check.
 **/
typedef async_data_operation duplication_verifier;

/**
 * A function to read a single data_vio from the layer.
 *
 * If the data_vio does not describe a read-modify-write operation, the
 * physical layer may safely acknowledge the related user I/O request
 * as complete.
 *
 * @param dataVIO  The data_vio to read
 **/
typedef async_data_operation data_reader;

/**
 * A function to read a single metadata vio from the layer.
 *
 * @param vio  The vio to read
 **/
typedef async_operation metadata_reader;

/**
 * A function to write a single data_vio to the layer
 *
 * @param dataVIO  The data_vio to write
 **/
typedef async_data_operation data_writer;

/**
 * A function to write a single metadata vio from the layer.
 *
 * @param vio  The vio to write
 **/
typedef async_operation metadata_writer;

/**
 * A function to inform the layer that a data_vio's related I/O request can be
 * safely acknowledged as complete, even though the data_vio itself may have
 * further processing to do.
 *
 * @param dataVIO  The data_vio to acknowledge
 **/
typedef async_data_operation data_acknowledger;

/**
 * A function to compare the contents of a data_vio to another data_vio.
 *
 * @param first   The first data_vio to compare
 * @param second  The second data_vio to compare
 *
 * @return <code>true</code> if the contents of the two DataVIOs are the same
 **/
typedef bool data_vio_comparator(struct data_vio *first,
				 struct data_vio *second);

/**
 * A function to compress the data in a data_vio.
 *
 * @param dataVIO  The data_vio to compress
 **/
typedef async_data_operation data_compressor;

/**
 * Update UDS.
 *
 * @param dataVIO  The data_vio which needs to change the entry for its data
 **/
typedef async_data_operation albireo_updater;

/**
 * A function to finish flush requests
 *
 * @param vdoFlush  The flush requests
 **/
typedef void flush_complete(struct vdo_flush **vdoFlush);

/**
 * A function to query the write policy of the layer.
 *
 * @param layer  The layer to query
 *
 * @return the write policy of the layer
 **/
typedef enum write_policy write_policy_getter(PhysicalLayer *layer);

/**
 * A function to wait for an admin operation to complete. This function should
 * not be called from a base-code thread.
 *
 * @param layer  The layer on which to wait
 **/
typedef void operation_waiter(PhysicalLayer *layer);

/**
 * A function to inform the layer of the result of an admin operation.
 *
 * @param layer  The layer to inform
 **/
typedef void operation_complete(PhysicalLayer *layer);

/**
 * An abstraction representing the underlying physical layer.
 **/
struct physicalLayer {
	// Management interface
	layer_destructor *destroy;

	// Synchronous interface
	block_count_getter *getBlockCount;

	// Synchronous IO interface
	buffer_allocator *allocateIOBuffer;
	extent_reader *reader;
	extent_writer *writer;

	write_policy_getter *getWritePolicy;

	// Synchronous interfaces (vio-based)
	compressed_write_vio_creator *createCompressedWriteVIO;
	data_vio_zeroer *zeroDataVIO;
	data_copier *copyData;
	data_modifier *applyPartialWrite;

	// Asynchronous interface (vio-based)
	data_hasher *hashData;
	duplication_checker *checkForDuplication;
	duplication_verifier *verifyDuplication;
	data_reader *readData;
	data_writer *writeData;
	compressed_writer *writeCompressedBlock;
	metadata_reader *readMetadata;
	metadata_writer *writeMetadata;
	data_acknowledger *acknowledgeDataVIO;
	data_vio_comparator *compareDataVIOs;
	data_compressor *compressDataVIO;
	albireo_updater *updateAlbireo;

	// Asynchronous interface (other)
	flush_complete *completeFlush;
	operation_waiter *waitForAdminOperation;
	operation_complete *completeAdminOperation;
};

/**
 * Create a new allocating_vio for compressed writes.
 *
 * @param [in]  vdo                The vdo
 * @param [in]  parent             The parent to assign to the allocating_vio's
 *                                 completion
 * @param [in]  data               The buffer
 * @param [out] allocating_vio_ptr  A pointer to hold new allocating_vio
 *
 * @return VDO_SUCCESS or an error
 **/
int __must_check
create_compressed_write_vio(struct vdo *vdo,
			    void *parent,
			    char *data,
			    struct allocating_vio **allocating_vio_ptr);

/**
 * Get the id of the callback thread on which a completion is currently
 * running, or -1 if no such thread.
 *
 * @return the current thread ID
 **/
thread_id_t get_callback_thread_id(void);

/**
 * A function to update a running CRC-32 checksum.
 *
 * @param crc     The current value of the crc
 * @param buffer  The data to add to the checksum
 * @param length  The length of the data
 *
 * @return The updated value of the checksum
 **/
crc32_checksum_t
update_crc32(crc32_checksum_t crc, const byte *buffer, size_t length);

/**
 * Destroy a vio. The pointer to the vio will be nulled out.
 *
 * @param vio_ptr  A pointer to the vio to destroy
 **/
void destroy_vio(struct vio **vio_ptr);

/**
 * Read or write a single metadata kvio.
 *
 * @param vio  The vio to read or write
 **/
void submit_metadata_vio(struct vio *vio);

/**
 * A function to asynchronously hash the block data, setting the chunk name of
 * the data_vio. This is asynchronous to allow the computation to be done on
 * different threads.
 *
 * @param data_vio  The data_vio to hash
 **/
void hash_data_vio(struct data_vio *data_vio);

/**
 * A function to determine whether a block is a duplicate. This function
 * expects the 'physical' field of the data_vio to be set to the physical block
 * where the block will be written if it is not a duplicate. If the block does
 * turn out to be a duplicate, the data_vio's 'isDuplicate' field will be set to
 * true, and the data_vio's 'advice' field will be set to the physical block and
 * mapping state of the already stored copy of the block.
 *
 * @param data_vio  The data_vio containing the block to check.
 **/
void check_for_duplication(struct data_vio *data_vio);

/**
 * A function to verify the duplication advice by examining an already-stored
 * data block. This function expects the 'physical' field of the data_vio to be
 * set to the physical block where the block will be written if it is not a
 * duplicate, and the 'duplicate' field to be set to the physical block and
 * mapping state where a copy of the data may already exist. If the block is
 * not a duplicate, the data_vio's 'isDuplicate' field will be cleared.
 *
 * @param data_vio  The data_vio containing the block to check.
 **/
void verify_duplication(struct data_vio *data_vio);

/**
 * Update the index with new dedupe advice.
 *
 * @param data_vio  The data_vio which needs to change the entry for its data
 **/
void update_dedupe_index(struct data_vio *data_vio);

/**
 * A function to zero the contents of a non-write data_vio -- a read, or a RMW
 * before becoming a write.
 *
 * @param data_vio  The data_vio to zero
 **/
void zero_data_vio(struct data_vio *data_vio);

/**
 * A function to copy the data of a write data_vio into a read data_vio.
 *
 * @param source       The data_vio to copy from
 * @param destination  The data_vio to copy to
 **/
void copy_data(struct data_vio *source, struct data_vio *destination);

/**
 * A function to apply a partial write to a data_vio which has completed the
 * read portion of a read-modify-write operation.
 *
 * @param data_vio  The data_vio to modify
 **/
void apply_partial_write(struct data_vio *data_vio);

/**
 * A function to inform the layer that a data_vio's related I/O request can be
 * safely acknowledged as complete, even though the data_vio itself may have
 * further processing to do.
 *
 * @param data_vio  The data_vio to acknowledge
 **/
void acknowledge_data_vio(struct data_vio *data_vio);

/**
 * A function to compress the data in a data_vio.
 *
 * @param data_vio  The data_vio to compress
 **/
void compress_data_vio(struct data_vio *data_vio);

/**
 * A function to read a single data_vio from the layer.
 *
 * If the data_vio does not describe a read-modify-write operation, the
 * physical layer may safely acknowledge the related user I/O request
 * as complete.
 *
 * @param data_vio  The data_vio to read
 **/
void read_data_vio(struct data_vio *data_vio);

/**
 * A function to write a single data_vio to the layer
 *
 * @param data_vio  The data_vio to write
 **/
void write_data_vio(struct data_vio *data_vio);

/**
 * A function to write a single compressed block to the layer
 *
 * @param allocating_vio  The allocating_vio to write
 **/
void write_compressed_block(struct allocating_vio *allocating_vio);

/**
 * A function to compare the contents of a data_vio to another data_vio.
 *
 * @param first   The first data_vio to compare
 * @param second  The second data_vio to compare
 *
 * @return <code>true</code> if the contents of the two DataVIOs are the same
 **/
bool compare_data_vios(struct data_vio *first, struct data_vio *second);

#endif // PHYSICAL_LAYER_H
