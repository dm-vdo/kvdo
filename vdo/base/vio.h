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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/vio.h#2 $
 */

#ifndef VIO_H
#define VIO_H

#include "completion.h"
#include "trace.h"
#include "types.h"
#include "vdo.h"

/**
 * A representation of a single block which may be passed between the VDO base
 * and the physical layer.
 **/
struct vio {
  /* The completion for this VIO */
  VDOCompletion        completion;

  /* The functions to call when this VIO's operation is complete */
  VDOAction           *callback;
  VDOAction           *errorHandler;

  /* The VDO handling this VIO */
  VDO                 *vdo;

  /* The address on the underlying device of the block to be read/written */
  PhysicalBlockNumber  physical;

  /* The type of request this VIO is servicing */
  VIOOperation         operation;

  /* The queueing priority of the VIO operation */
  VIOPriority          priority;

  /* The VIO type is used for statistics and instrumentation. */
  VIOType              type;

  /* Used for logging and debugging */
  Trace               *trace;
};

/**
 * Convert a generic VDOCompletion to a VIO.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a VIO
 **/
static inline VIO *asVIO(VDOCompletion *completion)
{
  STATIC_ASSERT(offsetof(VIO, completion) == 0);
  assertCompletionType(completion->type, VIO_COMPLETION);
  return (VIO *) completion;
}

/**
 * Convert a VIO to a generic completion.
 *
 * @param vio The VIO to convert
 *
 * @return The VIO as a completion
 **/
static inline VDOCompletion *vioAsCompletion(VIO *vio)
{
  return &vio->completion;
}

/**
 * Create a VIO.
 *
 * @param [in]  layer     The physical layer
 * @param [in]  vioType   The type of VIO to create
 * @param [in]  priority  The relative priority to assign to the VIO
 * @param [in]  parent    The parent of the VIO
 * @param [in]  data      The buffer
 * @param [out] vioPtr    A pointer to hold the new VIO
 *
 * @return VDO_SUCCESS or an error
 **/
static inline int createVIO(PhysicalLayer  *layer,
                            VIOType         vioType,
                            VIOPriority     priority,
                            void           *parent,
                            char           *data,
                            VIO           **vioPtr)
{
  return layer->createMetadataVIO(layer, vioType, priority, parent, data,
                                  vioPtr);
}

/**
 * Destroy a vio. The pointer to the VIO will be nulled out.
 *
 * @param vioPtr  A pointer to the VIO to destroy
 **/
void freeVIO(VIO **vioPtr);

/**
 * Initialize a VIO.
 *
 * @param vio       The VIO to initialize
 * @param type      The VIO type
 * @param priority  The relative priority of the VIO
 * @param parent    The parent (the extent completion) to assign to the VIO
 *                  completion
 * @param vdo       The VDO for this VIO
 * @param layer     The layer for this VIO
 **/
void initializeVIO(VIO           *vio,
                   VIOType        type,
                   VIOPriority    priority,
                   VDOCompletion *parent,
                   VDO           *vdo,
                   PhysicalLayer *layer);

/**
 * The very last step in processing a VIO. Set the VIO's completion's callback
 * and error handler from the fields set in the VIO itself on launch and then
 * actually complete the VIO's completion.
 *
 * @param completion  The VIO
 **/
void vioDoneCallback(VDOCompletion *completion);

/**
 * Get the name of a VIO's operation.
 *
 * @param vio  The VIO
 *
 * @return The name of the VIO's operation (read, write, or read-modify-write)
 **/
const char *getVIOReadWriteFlavor(const VIO *vio);

/**
 * Update per-VIO error stats.
 *
 * @param vio  The VIO which got an error
 *
 * @return The level at which to log the error
 **/
int updateVIOErrorStats(VIO *vio);

/**
 * Add a trace record for the current source location.
 *
 * @param vio      The VIO structure to be updated
 * @param location The source-location descriptor to be recorded
 **/
static inline void vioAddTraceRecord(VIO *vio, TraceLocation location)
{
  if (unlikely(vio->trace != NULL)) {
    addTraceRecord(vio->trace, location);
  }
}

/**
 * Check whether a VIO is servicing an external data request.
 *
 * @param vio  The VIO to check
 **/
static inline bool isDataVIO(VIO *vio)
{
  return isDataVIOType(vio->type);
}

/**
 * Check whether a VIO is for compressed block writes
 *
 * @param vio  The VIO to check
 **/
static inline bool isCompressedWriteVIO(VIO *vio)
{
  return isCompressedWriteVIOType(vio->type);
}

/**
 * Check whether a VIO is for metadata
 *
 * @param vio  The VIO to check
 **/
static inline bool isMetadataVIO(VIO *vio)
{
  return isMetadataVIOType(vio->type);
}

/**
 * Check whether a VIO is a read.
 *
 * @param vio  The VIO
 *
 * @return <code>true</code> if the VIO is a read
 **/
static inline bool isReadVIO(const VIO *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_READ);
}

/**
 * Check whether a VIO is a read-modify-write.
 *
 * @param vio  The VIO
 *
 * @return <code>true</code> if the VIO is a read-modify-write
 **/
static inline bool isReadModifyWriteVIO(const VIO *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_READ_MODIFY_WRITE);
}

/**
 * Check whether a VIO is a write.
 *
 * @param vio  The VIO
 *
 * @return <code>true</code> if the VIO is a write
 **/
static inline bool isWriteVIO(const VIO *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_WRITE);
}

/**
 * Check whether a VIO requires a flush before doing its I/O.
 *
 * @param vio  The VIO
 *
 * @return <code>true</code> if the VIO requires a flush before
 **/
static inline bool vioRequiresFlushBefore(const VIO *vio)
{
  return ((vio->operation & VIO_FLUSH_BEFORE) == VIO_FLUSH_BEFORE);
}

/**
 * Check whether a VIO requires a flush after doing its I/O.
 *
 * @param vio  The VIO
 *
 * @return <code>true</code> if the VIO requires a flush after
 **/
static inline bool vioRequiresFlushAfter(const VIO *vio)
{
  return ((vio->operation & VIO_FLUSH_AFTER) == VIO_FLUSH_AFTER);
}

/**
 * Launch a metadata VIO.
 *
 * @param vio           The VIO to launch
 * @param physical      The physical block number to read or write
 * @param callback      The function to call when the VIO completes its I/O
 * @param errorHandler  The handler for write errors
 * @param operation     The operation to perform (read or write)
 **/
void launchMetadataVIO(VIO                 *vio,
                       PhysicalBlockNumber  physical,
                       VDOAction           *callback,
                       VDOAction           *errorHandler,
                       VIOOperation         operation);

/**
 * Launch a metadata read VIO.
 *
 * @param vio           The VIO to launch
 * @param physical      The physical block number to read
 * @param callback      The function to call when the VIO completes its read
 * @param errorHandler  The handler for write errors
 **/
static inline void launchReadMetadataVIO(VIO                 *vio,
                                         PhysicalBlockNumber  physical,
                                         VDOAction           *callback,
                                         VDOAction           *errorHandler)
{
  launchMetadataVIO(vio, physical, callback, errorHandler, VIO_READ);
}

/**
 * Launch a metadata write VIO.
 *
 * @param vio           The VIO to launch
 * @param physical      The physical block number to write
 * @param callback      The function to call when the VIO completes its write
 * @param errorHandler  The handler for write errors
 **/
static inline void launchWriteMetadataVIO(VIO                 *vio,
                                          PhysicalBlockNumber  physical,
                                          VDOAction           *callback,
                                          VDOAction           *errorHandler)
{
  launchMetadataVIO(vio, physical, callback, errorHandler, VIO_WRITE);
}

/**
 * Launch a metadata write VIO optionally flushing the layer before and/or
 * after the write operation.
 *
 * @param vio          The VIO to launch
 * @param physical     The physical block number to write
 * @param callback     The function to call when the VIO completes its
 *                     operation
 * @param errorHandler The handler for flush or write errors
 * @param flushBefore  Whether or not to flush before writing
 * @param flushAfter   Whether or not to flush after writing
 **/
static inline
void launchWriteMetadataVIOWithFlush(VIO                 *vio,
                                     PhysicalBlockNumber  physical,
                                     VDOAction           *callback,
                                     VDOAction           *errorHandler,
                                     bool                 flushBefore,
                                     bool                 flushAfter)
{
  launchMetadataVIO(vio, physical, callback, errorHandler,
                    (VIO_WRITE
                     | (flushBefore ? VIO_FLUSH_BEFORE : 0)
                     | (flushAfter ? VIO_FLUSH_AFTER : 0)));
}

/**
 * Issue a flush to the layer. If the layer does not require flushing, this
 * method will immediately finish the VIO with which it was called. Care must
 * be taken to avoid introducing a stack overflow in that case.
 *
 * @param vio           The VIO to notify when the flush is complete
 * @param callback      The function to call when the flush is complete
 * @param errorHandler  The handler for flush errors
 **/
void launchFlush(VIO *vio, VDOAction *callback, VDOAction *errorHandler);

#endif // VIO_H
