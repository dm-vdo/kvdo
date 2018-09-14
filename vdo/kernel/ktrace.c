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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/ktrace.c#1 $
 */

#include "ktrace.h"

#include "memoryAlloc.h"

#include "dataVIO.h"

#include "kvio.h"
#include "logger.h"

enum {
  // How much data from a trace can we log in one call without messing
  // up the log or losing data?
  TRACE_LOG_MAX         = 820,

  // What fraction (1 out of TRACE_SAMPLE_INTERVAL VIOs) to trace
  TRACE_SAMPLE_INTERVAL = 3,
};

bool traceRecording = false;

static struct {
  char         buffer[2000];
  unsigned int counter;
  struct mutex lock;
} traceLoggingState;

/**
 * Initialize a SampleCounter structure with the given sampling interval.
 *
 * @param counter    The counter to initialize
 * @param interval   The desired sampling interval
 **/
static void initializeSampleCounter(SampleCounter *counter,
                                    unsigned int   interval)
{
  spin_lock_init(&counter->lock);
  counter->tick = 0;
  counter->interval = interval;
}

/*************************************************************************/
bool sampleThisOne(SampleCounter *counter)
{
  bool wantTracing = false;
  spin_lock(&counter->lock);
  counter->tick++;
  if (counter->tick >= counter->interval) {
    counter->tick = 0;
    wantTracing = true;
  }
  spin_unlock(&counter->lock);
  return wantTracing;
}

/*************************************************************************/
static void freeTraceDataBuffer(void *poolData, void *data)
{
  Trace *trace = (Trace *) data;
  FREE(trace);
}

/*************************************************************************/
static int allocTraceDataBuffer(void *poolData, void **dataPtr)
{
  Trace *trace;
  int result = ALLOCATE(1, Trace, __func__, &trace);
  if (result != VDO_SUCCESS) {
    logError("trace data allocation failure %d", result);
    return result;
  }

  *dataPtr = trace;
  return VDO_SUCCESS;
}

/*************************************************************************/
int allocTraceFromPool(KernelLayer *layer, Trace **tracePointer)
{
  int result = allocBufferFromPool(layer->traceBufferPool,
                                   (void **) tracePointer);
  if (result == VDO_SUCCESS) {
    (*tracePointer)->used = 0;
  }
  return result;
}

/*************************************************************************/
void freeTraceToPool(KernelLayer *layer, Trace *trace)
{
  freeBufferToPool(layer->traceBufferPool, trace);
}

/*************************************************************************/
int traceKernelLayerInit(KernelLayer *layer)
{
  layer->vioTraceRecording = traceRecording;
  initializeSampleCounter(&layer->traceSampleCounter, TRACE_SAMPLE_INTERVAL);
  unsigned int traceRecordsNeeded = 0;
  if (layer->vioTraceRecording) {
    traceRecordsNeeded += layer->requestLimiter.limit;
  }
  if (traceRecordsNeeded > 0) {
    return makeBufferPool("KVDO Trace Data Pool", traceRecordsNeeded,
                          allocTraceDataBuffer, freeTraceDataBuffer, NULL,
                          layer, &layer->traceBufferPool);
  }
  return VDO_SUCCESS;
}

/*************************************************************************/
void initializeTraceLoggingOnce(void)
{
  mutex_init(&traceLoggingState.lock);
}

/*************************************************************************/
void logKvioTrace(KVIO *kvio)
{
  KernelLayer *layer = kvio->layer;

  mutex_lock(&traceLoggingState.lock);
  traceLoggingState.counter++;
  // Log about 0.1% to avoid spewing data faster than syslog can keep up
  // (on certain of Permabit's test machines).
  // Yes, the 37 is arbitrary and meaningless.

  if (layer->traceLogging && ((traceLoggingState.counter % 1024) == 37)) {
    kvioAddTraceRecord(kvio, THIS_LOCATION(NULL));
    size_t traceLen = 0;
    formatTrace(kvio->vio->trace, traceLoggingState.buffer,
                sizeof(traceLoggingState.buffer), &traceLen);

    if (isMetadata(kvio)) {
      logInfo("finishing kvio %s meta @%p %s",
              (isWriteVIO(kvio->vio) ? "read" : "write"),
              kvio, traceLoggingState.buffer);
    } else if (isCompressedWriter(kvio)) {
      logInfo("finishing kvio write comp @%p %s",
              kvio, traceLoggingState.buffer);
    } else {
      const char *dupeLabel = "";
      if (isWriteVIO(kvio->vio)) {
        DataVIO *dataVIO = vioAsDataVIO(kvio->vio);
        if (isTrimDataVIO(dataVIO)) {
          dupeLabel = "trim ";
        } else if (dataVIO->isZeroBlock) {
          dupeLabel = "zero ";
        } else if (dataVIO->isDuplicate) {
          dupeLabel = "dupe ";
        } else {
          dupeLabel = "new ";
        }
      }

      logInfo("finishing kvio %s data %s@%p %.*s",
              (isWriteVIO(kvio->vio) ? "read" : "write"),
              dupeLabel, kvio, TRACE_LOG_MAX, traceLoggingState.buffer);
      char *buf = traceLoggingState.buffer;
      while (traceLen > TRACE_LOG_MAX) {
        traceLen -= TRACE_LOG_MAX;
        buf += TRACE_LOG_MAX;
        logInfo("more kvio %p path: %.*s", kvio, TRACE_LOG_MAX, buf);
      }
    }
  }

  mutex_unlock(&traceLoggingState.lock);
}
