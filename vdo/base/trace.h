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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/trace.h#1 $
 */

#ifndef TRACE_H
#define TRACE_H

#ifndef __KERNEL__
#include "cpu.h"
#endif

#include "threads.h"

/*
 * We need these records to be glued together with no intervening
 * bytes. That makes it rather sensitive to how the compiler,
 * assembler, and linker may add padding. Force extra alignment to
 * make it more reliable.
 *
 * Trace point descriptor language:
 *
 * The descriptor string provided at a trace point can have one or
 * more components, separated by ";". The first (or only) component is
 * a string to be formatted and shown in the flowchart graph. The
 * remaining components must be of the form "var=string", and assign
 * string values to "variables" that last through the processing of
 * the remainder of the current trace being read.
 *
 * The string displayed has variable substitutions done for any
 * occurrences of "$var" in the string.
 *
 * So, the descriptor sequence:
 *   kvdoWriteVIO;io=writeData;j=normal
 *   submitBio($io)
 *   writeJournalBlock($j)
 * would cause the graph generator to show the strings:
 *   kvdoWriteVIO
 *   submitBio(writeData)
 *   writeJournalBlock(normal)
 *
 * Substitutions are done in the variable assignment strings when
 * they're processed, so "foo=x($bar)" sets "foo" using the current
 * value of "bar"; it doesn't cause "bar" to be looked up when "$foo"
 * is seen later.
 *
 * The variable named "F" is automatically updated with the name of
 * the function associated with the descriptor, so you don't have to
 * explicitly repeat the name of the function if you just want to
 * augment it with more information. This may be desirable if a trace
 * point is expected to be reached more than once at different stages
 * of processing, or in a static function with a generic-sounding name
 * that needs disambiguation for graphing.
 *
 * If no descriptor string is provided, the
 * function:lineNumber:threadName string reported via systemtap will
 * be used in the graph.
 *
 * Current variable names used:
 *   cb=(various)      random info to log when enqueueing VIO callback
 *   dup=post,update   deduplication operation
 *   io=(various)      kind of I/O and data it's being done on
 *   j=normal,dedupe   kind of journal update being done
 *   js=mapWrite,writeZero,unmap  which step of journaling we're doing
 */
typedef const struct __attribute__((aligned(16))) traceLocationRecord {
  const char *function;
  int         line;
  const char *description;
} TraceLocationRecord;

/*
 * With well under 100 locations defined at the moment, even with no
 * idea where &baseTraceLocation will fall relative to the others, we
 * only need to support a range of -100..+100.
 */
typedef int32_t TraceLocationNumber;

/* The type to pass around */
typedef TraceLocationRecord *TraceLocation;

/*
 * N.B.: This code uses GCC extensions to create static, initialized
 * objects inline, describing the current function and line number.
 * The objects are collected into a table we can index with small
 * signed integers relative to &baseTraceLocation.
 *
 * We need baseTraceLocation because there's no standard way to get
 * the address of the start of this array we're defining.  And because
 * we're not playing any (additional) special linker tricks to ensure
 * ordering of the object files, the offsets may be signed, and we
 * don't know the range beyond the fact that we don't have hundreds of
 * these records lying around.
 *
 * By specifying a name that starts with neither .data nor .rodata, we
 * leave it to the toolchain to pick a location for us, based on
 * things like whether the section needs write access, which it does
 * for a PIC library but not for a kernel module.
 */

#define TRACE_LOCATION_SECTION \
  __attribute__((section(".kvdo_trace_locations")))

extern TRACE_LOCATION_SECTION TraceLocationRecord baseTraceLocation[];

#define TRACE_JOIN2(a,b) a##b
#define TRACE_JOIN(a,b) TRACE_JOIN2(a,b)
#define THIS_LOCATION(DESCRIPTION)                                      \
  __extension__                                                         \
  ({                                                                    \
    static TRACE_LOCATION_SECTION                                       \
      TraceLocationRecord TRACE_JOIN(loc,__LINE__) = {                  \
      .function    = __func__,                                          \
      .line        = __LINE__,                                          \
      .description = DESCRIPTION,                                       \
    };                                                                  \
    &TRACE_JOIN(loc,__LINE__);                                          \
  })

typedef struct traceRecord {
  uint64_t            when;     // counted in usec
  pid_t               tid;
  TraceLocationNumber location;
} TraceRecord;

enum { NUM_TRACE_RECORDS = 71 };

typedef struct trace {
  unsigned int used;
  TraceRecord  records[NUM_TRACE_RECORDS];
} Trace;

/**
 * Store a new record in the trace data.
 *
 * @param trace    The trace data to be updated
 * @param location The source-location descriptor to be recorded
 **/
void addTraceRecord(Trace *trace, TraceLocation location);

/**
 * Format trace data into a string for logging.
 *
 * @param [in]  trace         The trace data to be logged
 * @param [in]  buffer        The buffer in which to store the string
 * @param [in]  bufferLength  Length of the buffer
 * @param [out] msgLen        Length of the formatted string
 **/
void formatTrace(Trace  *trace,
                 char   *buffer,
                 size_t  bufferLength,
                 size_t *msgLen);

#endif /* TRACE_H */
