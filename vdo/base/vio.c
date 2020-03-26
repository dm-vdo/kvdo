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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vio.c#15 $
 */

#include "vio.h"

#include "logger.h"

#include "dataVIO.h"
#include "vdoInternal.h"

#include <linux/ratelimit.h>

/**********************************************************************/
void freeVIO(struct vio **vioPtr)
{
  struct vio *vio = *vioPtr;
  if (vio == NULL) {
    return;
  }

  destroy_vio(vioPtr);
}

/**********************************************************************/
void initializeVIO(struct vio            *vio,
                   VIOType                type,
                   VIOPriority            priority,
                   struct vdo_completion *parent,
                   struct vdo            *vdo,
                   PhysicalLayer         *layer)
{
  vio->vdo      = vdo;
  vio->type     = type;
  vio->priority = priority;

  struct vdo_completion *completion = vioAsCompletion(vio);
  initialize_completion(completion, VIO_COMPLETION, layer);
  completion->parent = parent;
}

/**********************************************************************/
void vioDoneCallback(struct vdo_completion *completion)
{
  struct vio *vio = asVIO(completion);
  completion->callback     = vio->callback;
  completion->errorHandler = vio->errorHandler;
  complete_completion(completion);
}

/**********************************************************************/
const char *getVIOReadWriteFlavor(const struct vio *vio)
{
  if (isReadVIO(vio)) {
    return "read";
  }
  return (isWriteVIO(vio) ? "write" : "read-modify-write");
}

/**********************************************************************/
void updateVIOErrorStats(struct vio *vio, const char *format, ...)
{
  int priority;
  int result = vioAsCompletion(vio)->result;
  switch (result) {
  case VDO_READ_ONLY:
    atomicAdd64(&vio->vdo->error_stats.readOnlyErrorCount, 1);
    return;

  case VDO_NO_SPACE:
    atomicAdd64(&vio->vdo->error_stats.noSpaceErrorCount, 1);
    priority = LOG_DEBUG;
    break;

  default:
    priority = LOG_ERR;
  }

  static DEFINE_RATELIMIT_STATE(errorLimiter, DEFAULT_RATELIMIT_INTERVAL,
                                DEFAULT_RATELIMIT_BURST);

  if (!__ratelimit(&errorLimiter)) {
    return;
  }

  va_list args;
  va_start(args, format);
  vLogWithStringError(priority, result, format, args);
  va_end(args);
}

/**
 * Handle an error from a metadata I/O.
 *
 * @param completion  The vio
 **/
static void handleMetadataIOError(struct vdo_completion *completion)
{
  struct vio *vio = asVIO(completion);
  updateVIOErrorStats(vio,
                      "Completing %s vio of type %u for physical block %"
                       PRIu64 " with error",
                       getVIOReadWriteFlavor(vio), vio->type, vio->physical);
  vioDoneCallback(completion);
}

/**********************************************************************/
void launchMetadataVIO(struct vio          *vio,
                       PhysicalBlockNumber  physical,
                       vdo_action          *callback,
                       vdo_action          *errorHandler,
                       VIOOperation         operation)
{
  vio->operation    = operation;
  vio->physical     = physical;
  vio->callback     = callback;
  vio->errorHandler = errorHandler;

  struct vdo_completion *completion = vioAsCompletion(vio);
  reset_completion(completion);
  completion->callback     = vioDoneCallback;
  completion->errorHandler = handleMetadataIOError;

  submitMetadataVIO(vio);
}

/**
 * Handle a flush error.
 *
 * @param completion  The flush vio
 **/
static void handleFlushError(struct vdo_completion *completion)
{
  logErrorWithStringError(completion->result, "Error flushing layer");
  completion->errorHandler = asVIO(completion)->errorHandler;
  complete_completion(completion);
}

/**********************************************************************/
void launchFlush(struct vio *vio, vdo_action *callback, vdo_action *errorHandler)
{
  ASSERT_LOG_ONLY(get_write_policy(vio->vdo) == WRITE_POLICY_ASYNC,
                  "pure flushes should not currently be issued in sync mode");

  struct vdo_completion *completion = vioAsCompletion(vio);
  reset_completion(completion);
  completion->callback     = callback;
  completion->errorHandler = handleFlushError;
  vio->errorHandler        = errorHandler;
  vio->operation           = VIO_FLUSH_BEFORE;
  vio->physical            = ZERO_BLOCK;

  completion->layer->flush(vio);
}
