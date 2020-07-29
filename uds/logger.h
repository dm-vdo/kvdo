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
 * $Id: //eng/uds-releases/krusty/src/uds/logger.h#7 $
 */

#ifndef LOGGER_H
#define LOGGER_H 1

#include <linux/ratelimit.h>
#include <linux/version.h>

#define LOG_EMERG 0   /* system is unusable */
#define LOG_ALERT 1   /* action must be taken immediately */
#define LOG_CRIT 2    /* critical conditions */
#define LOG_ERR 3     /* error conditions */
#define LOG_WARNING 4 /* warning conditions */
#define LOG_NOTICE 5  /* normal but significant condition */
#define LOG_INFO 6    /* informational */
#define LOG_DEBUG 7   /* debug-level messages */

// Make it easy to log real pointer values using %px when in development.
#ifdef LOG_INTERNAL
#define PRIptr "px"
#else
#define PRIptr "pK"
#endif

/*
 * Apply a rate limiter to a log method call.
 *
 * @param log_fn  A method that does logging, which is not invoked if we are
 *                running in the kernel and the ratelimiter detects that we
 *                are calling it frequently.
 */
#define log_ratelimit(log_fn, ...)                                        \
	do {                                                              \
		static DEFINE_RATELIMIT_STATE(_rs,                        \
					      DEFAULT_RATELIMIT_INTERVAL, \
					      DEFAULT_RATELIMIT_BURST);   \
		if (__ratelimit(&_rs)) {                                  \
			log_fn(__VA_ARGS__);                              \
		}                                                         \
	} while (0)

/**
 * @file
 *
 * All of the log<Level>() functions will preserve the callers value of errno.
 **/


/**
 * Get the current logging level.
 *
 * @return  the current logging priority level.
 **/
int get_log_level(void);

/**
 * Set the current logging level.
 *
 * @param new_log_level  the new value for the logging priority level.
 **/
void set_log_level(int new_log_level);

/**
 * Return the integer logging priority represented by a name.
 *
 * @param string  the name of the logging priority (case insensitive).
 *
 * @return the integer priority named by string, or LOG_INFO if not recognized.
 **/
int string_to_priority(const char *string);

/**
 * Return the printable name of a logging priority.
 *
 * @return the priority name
 **/
const char *priority_to_string(int priority);

/**
 * Log a debug message.
 *
 * @param format The format of the message (a printf style format)
 **/
void log_debug(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an informational message.
 *
 * @param  format The format of the message (a printf style format)
 **/
void log_info(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a normal (but notable) condition.
 *
 * @param  format The format of the message (a printf style format)
 **/
void log_notice(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a warning.
 *
 * @param  format The format of the message (a printf style format)
 **/
void log_warning(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log an error.
 *
 * @param  format The format of the message (a printf style format)
 **/
void log_error(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a message embedded within another message.
 *
 * @param priority      the priority at which to log the message
 * @param prefix        optional string prefix to message, may be NULL
 * @param fmt1          format of message first part (required)
 * @param args1         arguments for message first part (required)
 * @param fmt2          format of message second part
 **/
void log_embedded_message(int priority,
			  const char *prefix,
			  const char *fmt1,
			  va_list args1,
			  const char *fmt2,
			  ...)
	__attribute__((format(printf, 3, 0), format(printf, 5, 6)));

/**
 * Log a message pack consisting of multiple variable sections.
 *
 * @param priority      the priority at which to log the message
 * @param prefix        optional string prefix to message, may be NULL
 * @param fmt1          format of message first part (required)
 * @param args1         arguments for message first part
 * @param fmt2          format of message second part (required)
 * @param args2         arguments for message second part
 **/
void log_message_pack(int priority,
		      const char *prefix,
		      const char *fmt1,
		      va_list args1,
		      const char *fmt2,
		      va_list args2)
	__attribute__((format(printf, 3, 0), format(printf, 5, 0)));

/**
 * Log a stack backtrace.
 *
 * @param  priority The priority at which to log the backtrace
 **/
void log_backtrace(int priority);

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
int vLogWithStringError(int priority,
			int errnum,
			const char *format,
			va_list args) __attribute__((format(printf, 3, 0)));

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
 * IF the result is an error, log a FATAL level message and return the result
 * after marking it unrecoverable.  The UDS_SUCCESS and UDS_QUEUED results are
 * not considered errors and are returned unmodified.
 *
 * @param errnum  int value of errno or a UDS_* value.
 * @param format  The format of the message (a printf style format)
 *
 * @return makeUnrecoverable(errnum) or UDS_SUCCESS or UDS_QUEUED
 **/
int log_unrecoverable(int errnum, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * Log a fatal error.
 *
 * @param  format The format of the message (a printf style format)
 **/
void log_fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Log a message -- for internal use only.
 *
 * @param  priority The syslog priority value for the message.
 * @param  format   The format of the message (a printf style format)
 * @param  args     The variadic argument list of format parameters.
 **/
void v_log_message(int priority, const char *format, va_list args)
	__attribute__((format(printf, 2, 0)));

/**
 * Log a message
 *
 * @param  priority The syslog priority value for the message.
 * @param  format   The format of the message (a printf style format)
 **/
void log_message(int priority, const char *format, ...)
	__attribute__((format(printf, 2, 3)));

/**
 * Sleep or delay a short time (likely a few milliseconds) in an attempt allow
 * the log buffers to be written out in case they might be overrun. This is
 * unnecessary in user-space (and is a no-op there), but is needed when
 * quickly issuing a lot of log output in the Linux kernel, as when dumping a
 * large number of data structures.
 **/
void pause_for_logger(void);


#endif /* LOGGER_H */
