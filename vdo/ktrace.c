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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/ktrace.c#24 $
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

bool trace_recording;

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
static void free_trace_data_buffer(void *data)
{
	struct trace *trace = (struct trace *) data;

	FREE(trace);
}

/*************************************************************************/
static int alloc_trace_data_buffer(void **data_ptr)
{
	struct trace *trace;
	int result = ALLOCATE(1, struct trace, __func__, &trace);

	if (result != VDO_SUCCESS) {
		uds_log_error("trace data allocation failure %d", result);
		return result;
	}

	*data_ptr = trace;
	return VDO_SUCCESS;
}

/*************************************************************************/
int alloc_trace_from_pool(struct kernel_layer *layer,
			  struct trace **trace_pointer)
{
	int result = alloc_buffer_from_pool(layer->trace_buffer_pool,
					    (void **) trace_pointer);
	if (result == VDO_SUCCESS) {
		(*trace_pointer)->used = 0;
	}
	return result;
}

/*************************************************************************/
void free_trace_to_pool(struct kernel_layer *layer, struct trace *trace)
{
	free_buffer_to_pool(layer->trace_buffer_pool, trace);
}

/*************************************************************************/
int trace_kernel_layer_init(struct kernel_layer *layer)
{
	layer->vio_trace_recording = trace_recording;
	initialize_sample_counter(&layer->trace_sample_counter,
				  TRACE_SAMPLE_INTERVAL);
	unsigned int trace_records_needed = 0;

	if (layer->vio_trace_recording) {
		trace_records_needed += layer->request_limiter.limit;
	}
	if (trace_records_needed > 0) {
		return make_buffer_pool("KVDO Trace Data Pool",
					trace_records_needed,
					alloc_trace_data_buffer,
					free_trace_data_buffer,
					NULL,
					&layer->trace_buffer_pool);
	}
	return VDO_SUCCESS;
}

/*************************************************************************/
void initialize_trace_logging_once(void)
{
	mutex_init(&trace_logging_state.lock);
}

/*************************************************************************/
void log_vio_trace(struct vio *vio)
{
	struct kernel_layer *layer =
		as_kernel_layer(vio_as_completion(vio)->layer);

	mutex_lock(&trace_logging_state.lock);
	trace_logging_state.counter++;
	// Log about 0.1% to avoid spewing data faster than syslog can keep up
	// (on certain of Permabit's test machines).
	// Yes, the 37 is arbitrary and meaningless.

	if (layer->trace_logging &&
	    ((trace_logging_state.counter % 1024) == 37)) {
		vio_add_trace_record(vio, THIS_LOCATION(NULL));
		size_t trace_len = 0;

		format_trace(vio->trace, trace_logging_state.buffer,
			    sizeof(trace_logging_state.buffer), &trace_len);

		if (is_metadata_vio(vio)) {
			log_info("finishing vio %s meta @%px %s",
				 (is_write_vio(vio) ? "read" : "write"),
				 vio, trace_logging_state.buffer);
		} else if (is_compressed_write_vio(vio)) {
			log_info("finishing vio write comp @%px %s",
				 vio, trace_logging_state.buffer);
		} else {
			const char *dupe_label = "";

			if (is_write_vio(vio)) {
				struct data_vio *data_vio
					= vio_as_data_vio(vio);
				if (is_trim_data_vio(data_vio)) {
					dupe_label = "trim ";
				} else if (data_vio->is_zero_block) {
					dupe_label = "zero ";
				} else if (data_vio->is_duplicate) {
					dupe_label = "dupe ";
				} else {
					dupe_label = "new ";
				}
			}

			log_info("finishing vio %s data %s@%px %.*s",
				 (is_write_vio(vio) ? "read" : "write"),
				 dupe_label,
				 vio,
				 TRACE_LOG_MAX,
				 trace_logging_state.buffer);
			char *buf = trace_logging_state.buffer;

			while (trace_len > TRACE_LOG_MAX) {
				trace_len -= TRACE_LOG_MAX;
				buf += TRACE_LOG_MAX;
				log_info("more vio %px path: %.*s",
					 vio, TRACE_LOG_MAX, buf);
			}
		}
	}

	mutex_unlock(&trace_logging_state.lock);
}
