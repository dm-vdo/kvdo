/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef PERMASSERT_H
#define PERMASSERT_H

#include "compiler.h"
#include "errors.h"
#include "logger.h"

#define STRINGIFY(X) #X
#define STRINGIFY_VALUE(X) STRINGIFY(X)

/*
 * A hack to apply the "warn if unused" attribute to an integral expression.
 *
 * Since GCC doesn't propagate the warn_unused_result attribute to
 * conditional expressions incorporating calls to functions with that
 * attribute, this function can be used to wrap such an expression.
 * With optimization enabled, this function contributes no additional
 * instructions, but the warn_unused_result attribute still applies to
 * the code calling it.
 *
 * @param value  The value to return
 *
 * @return       The supplied value
 */
static INLINE int __must_check uds_must_use(int value)
{
	return value;
}

/*
 * A replacement for assert() from assert.h.
 *
 * @param expr      The boolean expression being asserted
 * @param code      The error code to return on non-fatal assertion
 *                  failure
 * @param format    A printf() style format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 *
 * @return UDS_SUCCESS If expr is true, code if expr is false and
 *         exit_on_assertion_failure is false. When exit_on_assertion_failure
 *         is true and expr is false, the program will exit from within
 *         this macro.
 */
#define ASSERT_WITH_ERROR_CODE(expr, code, ...) \
	uds_must_use(__UDS_ASSERT(expr, code, __VA_ARGS__))

/*
 * A replacement for assert() from assert.h.
 *
 * @param expr      The boolean expression being asserted
 * @param format    A printf() style format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 *
 * @return UDS_SUCCESS If expr is true, UDS_ASSERTION_FAILED if expr is
 *         false and exit_on_assertion_failure is false. When
 *         exit_on_assertion_failure is true and expr is false, the
 *         program will exit from within this macro.
 */
#define ASSERT(expr, ...) \
	ASSERT_WITH_ERROR_CODE(expr, UDS_ASSERTION_FAILED, __VA_ARGS__)

/*
 * A replacement for assert() which logs on failure, but does not return an
 * error code. This should be used sparingly. If the expression is false and
 * exit_on_assertion_failure is true, the program will exit from within this
 * macro.
 *
 * @param expr      The boolean expression being asserted
 * @param format    A printf() syle format for the message to log on
 *                  assertion failure
 * @param arguments Any additional arguments required by the format
 */
#define ASSERT_LOG_ONLY(expr, ...) \
	__UDS_ASSERT(expr, UDS_ASSERTION_FAILED, __VA_ARGS__)

/*
 * Common bottleneck for use by the other assertion macros.
 */
#define __UDS_ASSERT(expr, code, ...)                                 \
	(likely(expr) ? UDS_SUCCESS                                   \
		      : uds_assertion_failed(STRINGIFY(expr),         \
					     code,                    \
					     UDS_LOGGING_MODULE_NAME, \
					     __FILE__,                \
					     __LINE__,                \
					     __VA_ARGS__))

/*
 * This macro is a convenient wrapper for ASSERT(false, ...).
 */
#define ASSERT_FALSE(...) ASSERT(false, __VA_ARGS__)

#define STATIC_ASSERT(expr)         \
	do {                        \
		switch (0) {        \
		case 0:;            \
			fallthrough;\
		case expr:;	    \
			fallthrough;\
		default:;           \
		}                   \
	} while (0)

#define STATIC_ASSERT_SIZEOF(type, expected_size) \
	STATIC_ASSERT(sizeof(type) == (expected_size))

/**
 * Set whether or not to exit on an assertion failure.
 *
 * @param should_exit If <code>true</code> assertion failures will cause
 *                    the program to exit
 *
 * @return The previous setting
 **/
bool set_exit_on_assertion_failure(bool should_exit);

/**
 * Log an assertion failure.
 *
 * @param expression_string The assertion
 * @param error_code        The error code to return
 * @param module_name       The name of the module containing the assertion
 * @param file_name         The file in which the assertion appears
 * @param line_number       The line number on which the assertion appears
 * @param format            A printf() style format describing the assertion
 *
 * @return The supplied error_code unless exit_on_assertion_failure is
 *         true, in which case the process will be aborted
 **/
int uds_assertion_failed(const char *expression_string,
			 int error_code,
			 const char *module_name,
			 const char *file_name,
			 int line_number,
			 const char *format,
			 ...)
	__printf(6, 7);

#endif /* PERMASSERT_H */
