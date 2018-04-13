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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/trace.c#1 $
 */

#include "trace.h"

#include "logger.h"
#include "stringUtils.h"
#include "timeUtils.h"

TRACE_LOCATION_SECTION TraceLocationRecord baseTraceLocation[] = {
  {
    .function = "<none>",
    .line     = 0,
  },
};

/**********************************************************************/
void addTraceRecord(Trace *trace, TraceLocation location)
{
  if (trace->used < NUM_TRACE_RECORDS) {
    TraceRecord *record = &trace->records[trace->used];
    trace->used++;

    record->when     = nowUsec();
    record->tid      = getThreadId();
    record->location = location - baseTraceLocation;
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
void formatTrace(Trace  *trace,
                 char   *buffer,
                 size_t  bufferLength,
                 size_t *msgLen)
{
  if (trace == NULL) {
    return;
  }
  memset(buffer, 0, bufferLength);
  char *buf = buffer;
  char *bufferEnd = buffer + bufferLength - 1;
  if (trace->used > 0) {
    TraceRecord *record = &trace->records[0];
    TraceLocationRecord *location = baseTraceLocation + record->location;
    snprintf(buf, bufferEnd - buf, "Trace[%s@%" PRIu64 ".%06" PRIu64,
             location->function, record->when / 1000000,
             record->when % 1000000);
    buf += strlen(buf);

    for (unsigned int i = 1; i < trace->used; i++) {
      TraceRecord *prev = record;
      record++;

      snprintf(buf, bufferEnd - buf, ",");
      buf += strlen(buf);

      location = baseTraceLocation + record->location;
      unsigned long timeDiff = record->when - prev->when;
      snprintf(buf, bufferEnd - buf, "%s+%lu",
               location->function, timeDiff);
      buf += strlen(buf);
    }
    if (bufferLength > 7) {
      if (buffer[bufferLength-5] != '\0') {
        // too long
        strcpy(buffer+bufferLength-5, "...]");
      } else {
        strcpy(buf, "]");
      }
    }
  }
  *msgLen = (buf - buffer);
}
