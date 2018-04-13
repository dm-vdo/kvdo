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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/physicalLayer.h#1 $
 */

#ifndef PHYSICAL_LAYER_H
#define PHYSICAL_LAYER_H

#include "types.h"

static const CRC32Checksum INITIAL_CHECKSUM = 0xffffffff;

enum {
  /* The size of a CRC-32 checksum */
  CHECKSUM_SIZE = sizeof(CRC32Checksum),
};

/**
 * A function to destroy a physical layer and NULL out the reference to it.
 *
 * @param layerPtr  A pointer to the layer to destroy
 **/
typedef void LayerDestructor(PhysicalLayer **layerPtr);

/**
 * A function to update a running CRC-32 checksum.
 *
 * @param crc     The current value of the crc
 * @param buffer  The data to add to the checksum
 * @param length  The length of the data
 *
 * @return The updated value of the checksum
 **/
typedef uint32_t CRC32Updater(CRC32Checksum  crc,
                              const byte    *buffer,
                              size_t         length);

/**
 * A function to report the block count of a physicalLayer.
 *
 * @param layer  The layer
 *
 * @return The block count of the layer
 **/
typedef BlockCount BlockCountGetter(PhysicalLayer *layer);

/**
 * A function which can allocate a buffer suitable for use in an
 * ExtentReader or ExtentWriter.
 *
 * @param [in]  layer      The physical layer in question
 * @param [in]  bytes      The size of the buffer, in bytes.
 * @param [in]  why        The occasion for allocating the buffer
 * @param [out] bufferPtr  A pointer to hold the buffer
 *
 * @return a success or error code
 **/
typedef int BufferAllocator(PhysicalLayer  *layer,
                            size_t          bytes,
                            const char     *why,
                            char          **bufferPtr);

/**
 * A function which can read an extent from a physicalLayer.
 *
 * @param [in]  layer       The physical layer from which to read
 * @param [in]  startBlock  The physical block number of the start of the
 *                          extent
 * @param [in]  blockCount  The number of blocks in the extent
 * @param [out] buffer      A buffer to hold the extent
 * @param [out] blocksRead  A pointer to hold the number of blocks read (may be
 *                          NULL)
 *
 * @return a success or error code
 **/
typedef int ExtentReader(PhysicalLayer       *layer,
                         PhysicalBlockNumber  startBlock,
                         size_t               blockCount,
                         char                *buffer,
                         size_t              *blocksRead);

/**
 * A function which can write an extent to a physicalLayer.
 *
 * @param [in]  layer          The physical layer to which to write
 * @param [in]  startBlock     The physical block number of the start of the
 *                             extent
 * @param [in]  blockCount     The number of blocks in the extent
 * @param [in]  buffer         The buffer which contains the data
 * @param [out] blocksWritten  A pointer to hold the number of blocks written
 *                             (may be NULL)
 *
 * @return a success or error code
 **/
typedef int ExtentWriter(PhysicalLayer       *layer,
                         PhysicalBlockNumber  startBlock,
                         size_t               blockCount,
                         char                *buffer,
                         size_t              *blocksWritten);

/**
 * A function to allocate a metadata VIO.
 *
 * @param [in]  layer     The physical layer
 * @param [in]  vioType   The type of VIO to create
 * @param [in]  priority  The relative priority to assign to the VIOs
 * @param [in]  parent    The parent of this VIO
 * @param [in]  data      The buffer
 * @param [out] vioPtr    A pointer to hold the new VIO
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int MetadataVIOCreator(PhysicalLayer  *layer,
                               VIOType         vioType,
                               VIOPriority     priority,
                               void           *parent,
                               char           *data,
                               VIO           **vioPtr);

/**
 * A function to allocate an AllocatingVIO for compressed writes.
 *
 * @param [in]  layer             The physical layer
 * @param [in]  parent            The parent of this VIO
 * @param [in]  data              The buffer
 * @param [out] allocatingVIOPtr  A pointer to hold the new AllocatingVIO
 *
 * @return VDO_SUCCESS or an error
 **/
typedef int CompressedWriteVIOCreator(PhysicalLayer  *layer,
                                      void           *parent,
                                      char           *data,
                                      AllocatingVIO **allocatingVIOPtr);

/**
 * A function to destroy a VIO. The pointer to the VIO will be nulled out.
 *
 * @param vioPtr  A pointer to the VIO to destroy
 **/
typedef void VIODestructor(VIO **vioPtr);

/**
 * A function to zero the contents of a DataVIO.
 *
 * @param dataVIO  The DataVIO to zero
 **/
typedef AsyncDataOperation DataVIOZeroer;

/**
 * A function to copy the contents of a DataVIO into another DataVIO.
 *
 * @param source       The dataVIO to copy from
 * @param destination  The dataVIO to copy to
 **/
typedef void DataCopier(DataVIO *source, DataVIO *destination);

/**
 * A function to apply a partial write to a DataVIO which has completed the
 * read portion of a read-modify-write operation.
 *
 * @param dataVIO  The dataVIO to modify
 **/
typedef AsyncDataOperation DataModifier;

/**
 * A function to asynchronously hash the block data, setting the chunk name of
 * the DataVIO. This is asynchronous to allow the computation to be done on
 * different threads.
 *
 * @param dataVIO  The DataVIO to hash
 **/
typedef AsyncDataOperation DataHasher;

/**
 * A function to determine whether a block is a duplicate. This function
 * expects the 'physical' field of the DataVIO to be set to the physical block
 * where the block will be written if it is not a duplicate. If the block does
 * turn out to be a duplicate, the DataVIO's 'isDuplicate' field will be set to
 * true, and the DataVIO's 'advice' field will be set to the physical block and
 * mapping state of the already stored copy of the block.
 *
 * @param dataVIO  The DataVIO containing the block to check.
 **/
typedef AsyncDataOperation DuplicationChecker;

/**
 * A function to verify the duplication advice by examining an already-stored
 * data block. This function expects the 'physical' field of the DataVIO to be
 * set to the physical block where the block will be written if it is not a
 * duplicate, and the 'duplicate' field to be set to the physical block and
 * mapping state where a copy of the data may already exist. If the block is
 * not a duplicate, the DataVIO's 'isDuplicate' field will be cleared.
 *
 * @param dataVIO  The dataVIO containing the block to check.
 **/
typedef AsyncDataOperation DuplicationVerifier;

/**
 * A function to read a single DataVIO from the layer.
 *
 * If the DataVIO does not describe a read-modify-write operation, the
 * physical layer may safely acknowledge the related user I/O request
 * as complete.
 *
 * @param dataVIO  The DataVIO to read
 **/
typedef AsyncDataOperation DataReader;

/**
 * A function to read a single metadata VIO from the layer.
 *
 * @param vio  The vio to read
 **/
typedef AsyncOperation MetadataReader;

/**
 * A function to write a single DataVIO to the layer
 *
 * @param dataVIO  The DataVIO to write
 **/
typedef AsyncDataOperation DataWriter;

/**
 * A function to write a single metadata VIO from the layer.
 *
 * @param vio  The vio to write
 **/
typedef AsyncOperation MetadataWriter;

/**
 * A function to inform the layer that a DataVIO's related I/O request can be
 * safely acknowledged as complete, even though the DataVIO itself may have
 * further processing to do.
 *
 * @param dataVIO  The DataVIO to acknowledge
 **/
typedef AsyncDataOperation DataAcknowledger;

/**
 * A function to compare the contents of a DataVIO to another DataVIO.
 *
 * @param first   The first DataVIO to compare
 * @param second  The second DataVIO to compare
 *
 * @return <code>true</code> if the contents of the two DataVIOs are the same
 **/
typedef bool DataVIOComparator(DataVIO *first, DataVIO *second);

/**
 * A function to compress the data in a DataVIO.
 *
 * @param dataVIO  The DataVIO to compress
 **/
typedef AsyncDataOperation DataCompressor;

/**
 * Update albireo.
 *
 * @param dataVIO  The DataVIO which needs to change the entry for its data
 **/
typedef AsyncDataOperation AlbireoUpdater;

/**
 * A function to finish flush requests
 *
 * @param vdoFlush  The flush requests
 **/
typedef void FlushComplete(VDOFlush **vdoFlush);

/**
 * A function to query whether the layer requires flushes for persistence.
 *
 * @param layer  The layer to query
 *
 * @return <code>true</code> if the layer requires flushes
 **/
typedef bool FlushQuerier(PhysicalLayer *layer);

/**
 * A function to create an object that can be enqueued to run in a specified
 * thread. The Enqueueable will be put into the 'enqueueable' field of the
 * supplied completion.
 *
 * @param completion  The completion to invoke the callback of
 *
 * @return VDO_SUCCESS or an error code
 **/
typedef int EnqueueableCreator(VDOCompletion *completion);

/**
 * A function to destroy and deallocate an Enqueueable object.
 *
 * @param enqueueablePtr  Pointer to the object pointer to be destroyed
 **/
typedef void EnqueueableDestructor(Enqueueable **enqueueablePtr);

/**
 * A function to enqueue the Enqueueable object to run on the thread specified
 * by its associated completion.
 *
 * @param enqueueable  The object to be enqueued
 **/
typedef void Enqueuer(Enqueueable *enqueueable);

/**
 * A function to wait for an admin operation to complete. This function should
 * not be called from a base-code thread.
 *
 * @param layer  The layer on which to wait
 **/
typedef void OperationWaiter(PhysicalLayer *layer);

/**
 * A function to inform the layer of the result of an admin operation.
 *
 * @param layer  The layer to inform
 **/
typedef void OperationComplete(PhysicalLayer *layer);

/**
 * A function to get the id of the current thread.
 *
 * @return The id of the current thread
 **/
typedef ThreadID ThreadIDGetter(void);

/**
 * A function to return the physical layer pointer for the current thread.
 *
 * @return The physical layer pointer
 **/
typedef PhysicalLayer *PhysicalLayerGetter(void);

/**
 * An abstraction representing the underlying physical layer.
 **/
struct physicalLayer {
  // Management interface
  LayerDestructor           *destroy;

  // Synchronous interface
  CRC32Updater              *updateCRC32;
  BlockCountGetter          *getBlockCount;

  // Synchronous IO interface
  BufferAllocator           *allocateIOBuffer;
  ExtentReader              *reader;
  ExtentWriter              *writer;

  FlushQuerier              *isFlushRequired;

  // Synchronous interfaces (vio-based)
  MetadataVIOCreator        *createMetadataVIO;
  CompressedWriteVIOCreator *createCompressedWriteVIO;
  VIODestructor             *freeVIO;
  DataVIOZeroer             *zeroDataVIO;
  DataCopier                *copyData;
  DataModifier              *applyPartialWrite;

  // Asynchronous interface (vio-based)
  DataHasher                *hashData;
  DuplicationChecker        *checkForDuplication;
  DuplicationVerifier       *verifyDuplication;
  DataReader                *readData;
  DataWriter                *writeData;
  CompressedWriter          *writeCompressedBlock;
  MetadataReader            *readMetadata;
  MetadataWriter            *writeMetadata;
  MetadataWriter            *flush;
  DataAcknowledger          *acknowledgeDataVIO;
  DataVIOComparator         *compareDataVIOs;
  DataCompressor            *compressDataVIO;
  AlbireoUpdater            *updateAlbireo;

  // Asynchronous interface (other)
  FlushComplete             *completeFlush;
  EnqueueableCreator        *createEnqueueable;
  EnqueueableDestructor     *destroyEnqueueable;
  Enqueuer                  *enqueue;
  OperationWaiter           *waitForAdminOperation;
  OperationComplete         *completeAdminOperation;

  // Thread specific interface
  ThreadIDGetter            *getCurrentThreadID;
};

/**
 * Register the layer-specific implementation of getPhysicalLayer().
 *
 * @param getter  The function to be called
 **/
void registerPhysicalLayerGetter(PhysicalLayerGetter *getter);

/**
 * Fetch the physical layer pointer for the current thread.
 *
 * @return The physical layer pointer
 **/
PhysicalLayer *getPhysicalLayer(void);

/**
 * Get the id of the callback thread on which a completion is current running.
 *
 * @return the current thread ID
 **/
static inline ThreadID getCallbackThreadID(void)
{
  return getPhysicalLayer()->getCurrentThreadID();
}

#endif // PHYSICAL_LAYER_H
