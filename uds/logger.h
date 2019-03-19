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
 * $Id: //eng/uds-releases/gloria/src/uds/logger.h#1 $
 */

#ifndef LOGGER_H
#define LOGGER_H 1

#include <stdarg.h>

#include "loggerDefs.h"

/**
 * @file
 *
 * The functions in this file are not thread safe in the sense that nothing
 * prevents multiple threads from opening or closing loggers out from under
 * other threads. In reality this isn't a problem since the only calls in
 * production code to openLogger() and closeLogger() are made in uds.c while
 * uds mutex is held, and uds does not make any logging calls before it calls
 * openLogger or after it calls closeLogger().
 *
 * All of the log<Level>() functions will preserve the callers value of errno.
 **/

/**
 * Start the logger.
 **/
void openLogger(void);

/**
 * Stop the logger.
 **/
void closeLogger(void);

/**
 * Get the current logging level.
 *
 * @return  the current logging priority level.
 **/
int getLogLevel(void);

/**
 * Set the current logging level.
 *
 * @param newLogLevel  the new value for the logging priority level.
 **/
void setLogLevel(int newLogLevel);

/**
 * Return the integer logging priority represented by a name.
 *
 * @param string  the name of the logging priority (case insensitive).
 *
 * @return the integer priority named by string, or LOG_INFO if not recognized.
 **/
int stringToPriority(const char *string);

/**
 * Return the printable name of a logging priority.
 *
 * @return the priority name
 **/
const char *priorityToString(int priority);

/**
 * Log a debug message.
 *
 * @param format The format of the message (a printf style format)
 **/
void logDebug(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an informational message.
 *
 * @param  format The format of the message (a printf style format)
 **/
void logInfo(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a normal (but notable) condition.
 *
 * @param  format The format of the message (a printf style format)
 **/
void logNotice(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a warning.
 *
 * @param  format The format of the message (a printf style format)
 **/
void logWarning(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an error.
 *
 * @param  format The format of the message (a printf style format)
  **/
void logError(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a message embedded within another message.
 *
 * @param priority      the priority at which to log the message
 * @param prefix        optional string prefix to message, may be NULL
 * @param fmt1          format of message first part, may be NULL
 * @param args1         arguments for message first part
 * @param fmt2          format of message second part
 **/
void logEmbeddedMessage(int         priority,
                        const char *prefix,
                        const char *fmt1,
                        va_list     args1,
                        const char *fmt2,
                        ...)
  __attribute__((format(printf, 3, 0), format(printf, 5, 6)));

/**
 * Log a message pack consisting of multiple variable sections.
 *
 * @param priority      the priority at which to log the message
 * @param prefix        optional string prefix to message, may be NULL
 * @param fmt1          format of message first part, may be NULL
 * @param args1         arguments for message first part
 * @param fmt2          format of message second part, may be NULL
 * @param args2         arguments for message second part
 **/
void logMessagePack(int         priority,
                    const char *prefix,
                    const char *fmt1,
                    va_list     args1,
                    const char *fmt2,
                    va_list     args2)
  __attribute__((format(printf, 3, 0)));

/**
 * Log a stack backtrace.
 *
 * @param  priority The priority at which to log the backtrace
 **/
void logBacktrace(int priority);

/**
 * Log a message with an error from an error code.
 *
 * @param  priority The priority of the logging entry
 * @param  errnum   Int value of errno or a UDS_* value.
 * @param  format   The format of the message (a printf style format)
 *
 * @return errnum
 **/
int logWithStringError(int priority, int errnum, const char *format, ...)
  __attribute__((format(printf, 3, 4)));

/**
 * Log a message with an error from an error code.
 *
 * @param  priority The priority of the logging entry
 * @param  errnum   Int value of errno or a UDS_* value.
 * @param  format   The format of the message (a printf style format)
 * @param  args     The list of arguments with format.
 *
 * @return errnum
 **/
int vLogWithStringError(int         priority,
                        int         errnum,
                        const char *format,
                        va_list     args)
  __attribute__((format(printf, 3, 0)));

/**
 * Log an error prefixed with the string associated with the errnum.
 *
 * @param errnum Int value of errno or a UDS_* value.
 * @param format The format of the message (a printf style format)
 *
 * @return errnum
 **/
int logErrorWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**********************************************************************/
int logDebugWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**********************************************************************/
int logInfoWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**********************************************************************/
int logNoticeWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**********************************************************************/
int logWarningWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**********************************************************************/
int logFatalWithStringError(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**
 * Log an ERROR level message and return makeUnrecoverable(errnum)
 * UDS_SUCCESS is ignored and returned.
 *
 * @param  errnum Int value of errno or a UDS_* value.
 * @param  format The format of the message (a printf style format)
 * @return makeUnrecoverable(errnum) or UDS_SUCCESS.
 **/
int logUnrecoverable(int errnum, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**
 * Log a fatal error.
 *
 * @param  format The format of the message (a printf style format)
 **/
void logFatal(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a message -- for internal use only.
 *
 * @param  priority The syslog priority value for the message.
 * @param  format   The format of the message (a printf style format)
 * @param  args     The variadic argument list of format parameters.
 **/
void vLogMessage(int priority, const char *format, va_list args)
  __attribute__((format(printf, 2, 0)));

/**
 * Log a message
 *
 * @param  priority The syslog priority value for the message.
 * @param  format   The format of the message (a printf style format)
 **/
void logMessage(int priority, const char *format, ...)
  __attribute__((format(printf, 2, 3)));

/**
 * Sleep or delay a short time (likely a few milliseconds) in an attempt allow
 * the log buffers to be written out in case they might be overrun. This is
 * unnecessary in user-space (and is a no-op there), but is needed when
 * quickly issuing a lot of log output in the Linux kernel, as when dumping a
 * large number of data structures.
 **/
void pauseForLogger(void);

#endif /* LOGGER_H */
