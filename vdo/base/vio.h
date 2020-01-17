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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.h#8 $
 */

#ifndef VIO_H
#define VIO_H

#include <stdarg.h>

#include "completion.h"
#include "trace.h"
#include "types.h"
#include "vdo.h"

/**
 * A representation of a single block which may be passed between the VDO base
 * and the physical layer.
 **/
struct vio {
  /* The completion for this vio */
  struct vdo_completion  completion;

  /* The functions to call when this vio's operation is complete */
  VDOAction             *callback;
  VDOAction             *errorHandler;

  /* The vdo handling this vio */
  struct vdo            *vdo;

  /* The address on the underlying device of the block to be read/written */
  PhysicalBlockNumber    physical;

  /* The type of request this vio is servicing */
  VIOOperation           operation;

  /* The queueing priority of the vio operation */
  VIOPriority            priority;

  /* The vio type is used for statistics and instrumentation. */
  VIOType                type;

  /* Used for logging and debugging */
  Trace                 *trace;
};

/**
 * Convert a generic vdo_completion to a vio.
 *
 * @param completion  The completion to convert
 *
 * @return The completion as a vio
 **/
static inline struct vio *asVIO(struct vdo_completion *completion)
{
  STATIC_ASSERT(offsetof(struct vio, completion) == 0);
  assertCompletionType(completion->type, VIO_COMPLETION);
  return (struct vio *) completion;
}

/**
 * Convert a vio to a generic completion.
 *
 * @param vio The vio to convert
 *
 * @return The vio as a completion
 **/
static inline struct vdo_completion *vioAsCompletion(struct vio *vio)
{
  return &vio->completion;
}

/**
 * Create a vio.
 *
 * @param [in]  layer     The physical layer
 * @param [in]  vioType   The type of vio to create
 * @param [in]  priority  The relative priority to assign to the vio
 * @param [in]  parent    The parent of the vio
 * @param [in]  data      The buffer
 * @param [out] vioPtr    A pointer to hold the new vio
 *
 * @return VDO_SUCCESS or an error
 **/
static inline int createVIO(PhysicalLayer  *layer,
                            VIOType         vioType,
                            VIOPriority     priority,
                            void           *parent,
                            char           *data,
                            struct vio    **vioPtr)
{
  return layer->createMetadataVIO(layer, vioType, priority, parent, data,
                                  vioPtr);
}

/**
 * Destroy a vio. The pointer to the vio will be nulled out.
 *
 * @param vioPtr  A pointer to the vio to destroy
 **/
void freeVIO(struct vio **vioPtr);

/**
 * Initialize a vio.
 *
 * @param vio       The vio to initialize
 * @param type      The vio type
 * @param priority  The relative priority of the vio
 * @param parent    The parent (the extent completion) to assign to the vio
 *                  completion
 * @param vdo       The vdo for this vio
 * @param layer     The layer for this vio
 **/
void initializeVIO(struct vio            *vio,
                   VIOType                type,
                   VIOPriority            priority,
                   struct vdo_completion *parent,
                   struct vdo            *vdo,
                   PhysicalLayer         *layer);

/**
 * The very last step in processing a vio. Set the vio's completion's callback
 * and error handler from the fields set in the vio itself on launch and then
 * actually complete the vio's completion.
 *
 * @param completion  The vio
 **/
void vioDoneCallback(struct vdo_completion *completion);

/**
 * Get the name of a vio's operation.
 *
 * @param vio  The vio
 *
 * @return The name of the vio's operation (read, write, or read-modify-write)
 **/
const char *getVIOReadWriteFlavor(const struct vio *vio);

/**
 * Update per-vio error stats and log the error.
 *
 * @param vio     The vio which got an error
 * @param format  The format of the message to log (a printf style format)
 **/
void updateVIOErrorStats(struct vio *vio, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**
 * Add a trace record for the current source location.
 *
 * @param vio      The vio structure to be updated
 * @param location The source-location descriptor to be recorded
 **/
static inline void vioAddTraceRecord(struct vio *vio, TraceLocation location)
{
  if (unlikely(vio->trace != NULL)) {
    addTraceRecord(vio->trace, location);
  }
}

/**
 * Check whether a vio is servicing an external data request.
 *
 * @param vio  The vio to check
 **/
static inline bool isDataVIO(struct vio *vio)
{
  return isDataVIOType(vio->type);
}

/**
 * Check whether a vio is for compressed block writes
 *
 * @param vio  The vio to check
 **/
static inline bool isCompressedWriteVIO(struct vio *vio)
{
  return isCompressedWriteVIOType(vio->type);
}

/**
 * Check whether a vio is for metadata
 *
 * @param vio  The vio to check
 **/
static inline bool isMetadataVIO(struct vio *vio)
{
  return isMetadataVIOType(vio->type);
}

/**
 * Check whether a vio is a read.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a read
 **/
static inline bool isReadVIO(const struct vio *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_READ);
}

/**
 * Check whether a vio is a read-modify-write.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a read-modify-write
 **/
static inline bool isReadModifyWriteVIO(const struct vio *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_READ_MODIFY_WRITE);
}

/**
 * Check whether a vio is a write.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio is a write
 **/
static inline bool isWriteVIO(const struct vio *vio)
{
  return ((vio->operation & VIO_READ_WRITE_MASK) == VIO_WRITE);
}

/**
 * Check whether a vio requires a flush before doing its I/O.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio requires a flush before
 **/
static inline bool vioRequiresFlushBefore(const struct vio *vio)
{
  return ((vio->operation & VIO_FLUSH_BEFORE) == VIO_FLUSH_BEFORE);
}

/**
 * Check whether a vio requires a flush after doing its I/O.
 *
 * @param vio  The vio
 *
 * @return <code>true</code> if the vio requires a flush after
 **/
static inline bool vioRequiresFlushAfter(const struct vio *vio)
{
  return ((vio->operation & VIO_FLUSH_AFTER) == VIO_FLUSH_AFTER);
}

/**
 * Launch a metadata vio.
 *
 * @param vio           The vio to launch
 * @param physical      The physical block number to read or write
 * @param callback      The function to call when the vio completes its I/O
 * @param errorHandler  The handler for write errors
 * @param operation     The operation to perform (read or write)
 **/
void launchMetadataVIO(struct vio          *vio,
                       PhysicalBlockNumber  physical,
                       VDOAction           *callback,
                       VDOAction           *errorHandler,
                       VIOOperation         operation);

/**
 * Launch a metadata read vio.
 *
 * @param vio           The vio to launch
 * @param physical      The physical block number to read
 * @param callback      The function to call when the vio completes its read
 * @param errorHandler  The handler for write errors
 **/
static inline void launchReadMetadataVIO(struct vio          *vio,
                                         PhysicalBlockNumber  physical,
                                         VDOAction           *callback,
                                         VDOAction           *errorHandler)
{
  launchMetadataVIO(vio, physical, callback, errorHandler, VIO_READ);
}

/**
 * Launch a metadata write vio.
 *
 * @param vio           The vio to launch
 * @param physical      The physical block number to write
 * @param callback      The function to call when the vio completes its write
 * @param errorHandler  The handler for write errors
 **/
static inline void launchWriteMetadataVIO(struct vio          *vio,
                                          PhysicalBlockNumber  physical,
                                          VDOAction           *callback,
                                          VDOAction           *errorHandler)
{
  launchMetadataVIO(vio, physical, callback, errorHandler, VIO_WRITE);
}

/**
 * Launch a metadata write vio optionally flushing the layer before and/or
 * after the write operation.
 *
 * @param vio          The vio to launch
 * @param physical     The physical block number to write
 * @param callback     The function to call when the vio completes its
 *                     operation
 * @param errorHandler The handler for flush or write errors
 * @param flushBefore  Whether or not to flush before writing
 * @param flushAfter   Whether or not to flush after writing
 **/
static inline
void launchWriteMetadataVIOWithFlush(struct vio          *vio,
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
 * Issue a flush to the layer. Currently expected to be used only in
 * async mode.
 *
 * @param vio           The vio to notify when the flush is complete
 * @param callback      The function to call when the flush is complete
 * @param errorHandler  The handler for flush errors
 **/
void launchFlush(struct vio *vio, VDOAction *callback, VDOAction *errorHandler);

#endif // VIO_H
