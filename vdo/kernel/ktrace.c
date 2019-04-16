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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/ktrace.c#7 $
 */

#include "ktrace.h"

#include "memoryAlloc.h"

#include "dataVIO.h"

#include "kvio.h"
#include "logger.h"

enum {
	// How much data from a trace can we log in one call without messing
	// up the log or losing data?
	TRACE_LOG_MAX = 820,

	// What fraction (1 out of TRACE_SAMPLE_INTERVAL VIOs) to trace
	TRACE_SAMPLE_INTERVAL = 3,
};

bool trace_recording = false;

static struct {
	char buffer[2000];
	unsigned int counter;
	struct mutex lock;
} trace_logging_state;

/**
 * Initialize a struct sample_counter structure with the given
 * sampling interval.
 *
 * @param counter    The counter to initialize
 * @param interval   The desired sampling interval
 **/
static void initialize_sample_counter(struct sample_counter *counter,
				      unsigned int interval)
{
	spin_lock_init(&counter->lock);
	counter->tick = 0;
	counter->interval = interval;
}

/*************************************************************************/
bool sample_this_one(struct sample_counter *counter)
{
	bool want_tracing = false;
	spin_lock(&counter->lock);
	counter->tick++;
	if (counter->tick >= counter->interval) {
		counter->tick = 0;
		want_tracing = true;
	}
	spin_unlock(&counter->lock);
	return want_tracing;
}

/*************************************************************************/
static void free_trace_data_buffer(void *pool_data, void *data)
{
	Trace *trace = (Trace *)data;
	FREE(trace);
}

/*************************************************************************/
static int alloc_trace_data_buffer(void *pool_data, void **data_ptr)
{
	Trace *trace;
	int result = ALLOCATE(1, Trace, __func__, &trace);
	if (result != VDO_SUCCESS) {
		logError("trace data allocation failure %d", result);
		return result;
	}

	*data_ptr = trace;
	return VDO_SUCCESS;
}

/*************************************************************************/
int alloc_trace_from_pool(struct kernel_layer *layer, Trace **trace_pointer)
{
	int result = alloc_buffer_from_pool(layer->traceBufferPool,
					    (void **)trace_pointer);
	if (result == VDO_SUCCESS) {
		(*trace_pointer)->used = 0;
	}
	return result;
}

/*************************************************************************/
void free_trace_to_pool(struct kernel_layer *layer, Trace *trace)
{
	free_buffer_to_pool(layer->traceBufferPool, trace);
}

/*************************************************************************/
int trace_kernel_layer_init(struct kernel_layer *layer)
{
	layer->vioTraceRecording = trace_recording;
	initialize_sample_counter(&layer->traceSampleCounter,
				  TRACE_SAMPLE_INTERVAL);
	unsigned int trace_records_needed = 0;
	if (layer->vioTraceRecording) {
		trace_records_needed += layer->requestLimiter.limit;
	}
	if (trace_records_needed > 0) {
		return make_buffer_pool("KVDO Trace Data Pool",
					trace_records_needed,
					alloc_trace_data_buffer,
					free_trace_data_buffer,
					NULL,
					layer,
					&layer->traceBufferPool);
	}
	return VDO_SUCCESS;
}

/*************************************************************************/
void initialize_trace_logging_once(void)
{
	mutex_init(&trace_logging_state.lock);
}

/*************************************************************************/
void log_kvio_trace(struct kvio *kvio)
{
	struct kernel_layer *layer = kvio->layer;

	mutex_lock(&trace_logging_state.lock);
	trace_logging_state.counter++;
	// Log about 0.1% to avoid spewing data faster than syslog can keep up
	// (on certain of Permabit's test machines).
	// Yes, the 37 is arbitrary and meaningless.

	if (layer->traceLogging &&
	    ((trace_logging_state.counter % 1024) == 37)) {
		kvio_add_trace_record(kvio, THIS_LOCATION(NULL));
		size_t trace_len = 0;
		formatTrace(kvio->vio->trace, trace_logging_state.buffer,
			    sizeof(trace_logging_state.buffer), &trace_len);

		if (is_metadata(kvio)) {
			logInfo("finishing kvio %s meta @%" PRIptr " %s",
				(isWriteVIO(kvio->vio) ? "read" : "write"),
				kvio, trace_logging_state.buffer);
		} else if (is_compressed_writer(kvio)) {
			logInfo("finishing kvio write comp @%" PRIptr " %s",
				kvio, trace_logging_state.buffer);
		} else {
			const char *dupe_label = "";
			if (isWriteVIO(kvio->vio)) {
				DataVIO *data_vio = vioAsDataVIO(kvio->vio);
				if (isTrimDataVIO(data_vio)) {
					dupe_label = "trim ";
				} else if (data_vio->isZeroBlock) {
					dupe_label = "zero ";
				} else if (data_vio->isDuplicate) {
					dupe_label = "dupe ";
				} else {
					dupe_label = "new ";
				}
			}

			logInfo("finishing kvio %s data %s@%" PRIptr " %.*s",
				(isWriteVIO(kvio->vio) ? "read" : "write"),
				dupe_label,
				kvio,
				TRACE_LOG_MAX,
				trace_logging_state.buffer);
			char *buf = trace_logging_state.buffer;
			while (trace_len > TRACE_LOG_MAX) {
				trace_len -= TRACE_LOG_MAX;
				buf += TRACE_LOG_MAX;
				logInfo("more kvio %" PRIptr " path: %.*s",
					kvio, TRACE_LOG_MAX, buf);
			}
		}
	}

	mutex_unlock(&trace_logging_state.lock);
}
