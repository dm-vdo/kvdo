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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/ktrace.h#1 $
 */

#ifndef KTRACE_H
#define KTRACE_H

#include <linux/device-mapper.h>

#include "common.h"
#include "trace.h"

struct kernelLayer;
struct kvio;

// Implement event sampling once per N.
typedef struct {
  unsigned int interval;
  unsigned int tick;
  spinlock_t   lock;
} SampleCounter;

/**
 * Flag indicating whether newly created VDO devices should record trace info.
 **/
extern bool traceRecording;

/**
 * Updates the counter state and returns true once each time the
 * sampling interval is reached.
 *
 * @param counter    The sampling counter info
 *
 * @return whether to do sampling on this invocation
 **/
bool sampleThisOne(SampleCounter *counter);

/**
 * Initialize trace data in the KernelLayer
 *
 * @param layer  The KernelLayer
 *
 * @return VDO_SUCCESS, or an error code
 **/
int traceKernelLayerInit(struct kernelLayer *layer);

/**
 * Initialize the mutex used when logging latency tracing data.
 **/
void initializeTraceLoggingOnce(void);

/**
 * Allocate a trace buffer
 *
 * @param layer         The KernelLayer
 * @param tracePointer  The trace buffer is returned here
 *
 * @return VDO_SUCCESS or an error code
 **/
int allocTraceFromPool(struct kernelLayer *layer, Trace **tracePointer);

/**
 * Free a trace buffer
 *
 * @param layer  The KernelLayer
 * @param trace  The trace buffer
 **/
void freeTraceToPool(struct kernelLayer *layer, Trace *trace);

/**
 * Log the trace at kvio freeing time
 *
 * @param kvio  The kvio structure
 **/
void logKvioTrace(struct kvio *kvio);

#endif /* KTRACE_H */
