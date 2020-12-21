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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/trace.c#9 $
 */

#include "trace.h"

#include "logger.h"
#include "stringUtils.h"
#include "timeUtils.h"

TRACE_LOCATION_SECTION const struct trace_location base_trace_location[] = {
	{
		.function = "<none>",
		.line = 0,
	},
};

/**********************************************************************/
void add_trace_record(struct trace *trace,
		      const struct trace_location *location)
{
	if (trace->used < NUM_TRACE_RECORDS) {
		struct trace_record *record = &trace->records[trace->used];
		trace->used++;
		record->when = current_time_us();
		record->tid = get_thread_id();
		record->location = location - base_trace_location;
	}
}

/*
 * The record display format used is a comma-separated list, each item
 * containing: optional function name; "@" + timestamp with seconds
 * and microseconds for the first record; if not the first record, "+"
 * and offset in microseconds from previous timestamp.
 *
 * If the buffer's too small, it'll end with an ellipsis.
 */
void format_trace(struct trace *trace,
		  char *buffer,
		  size_t buffer_length,
		  size_t *msg_len)
{
	char *buf = buffer;
	char *buffer_end = buffer + buffer_length - 1;

	if (trace == NULL) {
		return;
	}
	memset(buffer, 0, buffer_length);
	if (trace->used > 0) {
		unsigned int i;
		struct trace_record *record = &trace->records[0];
		const struct trace_location *location =
			base_trace_location + record->location;
		snprintf(buf,
			 buffer_end - buf,
			 "trace[%s@%llu.%06llu",
			 location->function,
			 record->when / 1000000,
			 record->when % 1000000);
		buf += strlen(buf);

		for (i = 1; i < trace->used; i++) {
			unsigned long time_diff;
			struct trace_record *prev = record;
			record++;

			snprintf(buf, buffer_end - buf, ",");
			buf += strlen(buf);

			location = base_trace_location + record->location;
			time_diff = record->when - prev->when;
			snprintf(buf,
				 buffer_end - buf,
				 "%s+%lu",
				 location->function,
				 time_diff);
			buf += strlen(buf);
		}
		if (buffer_length > 7) {
			if (buffer[buffer_length - 5] != '\0') {
				// too long
				strcpy(buffer + buffer_length - 5, "...]");
			} else {
				strcpy(buf, "]");
			}
		}
	}
	*msg_len = (buf - buffer);
}
