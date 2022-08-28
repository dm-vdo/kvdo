/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright Red Hat
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <linux/kernel.h>
#include <linux/string.h>

#include "compiler.h"
#include "type-defs.h"

/**
 * Convert a boolean value to its corresponding "true" or "false" string.
 *
 * @param value  The boolean value to convert
 *
 * @return "true" if value is true, "false" otherwise.
 **/
static INLINE const char *uds_bool_to_string(bool value)
{
	return (value ? "true" : "false");
}

/**
 * Write a printf-style string into a fixed-size buffer, returning
 * errors if it would not fit. (our version of snprintf)
 *
 * @param [in]  what     A description of what is being written, for error
 *                       logging; if NULL doesn't log anything.
 * @param [out] buf      The target buffer
 * @param [in]  buf_size The size of buf
 * @param [in]  error    Error code to return on overflow
 * @param [in]  fmt      The sprintf format parameter.
 *
 * @return <code>UDS_SUCCESS</code> or <code>error</code>
 **/
int __must_check uds_fixed_sprintf(const char *what,
				   char *buf,
				   size_t buf_size,
				   int error,
				   const char *fmt, ...)
	__printf(5, 6);

/**
 * Write printf-style string into an existing buffer, returning a specified
 * error code if it would not fit, and setting ``needed`` to the amount of
 * space that would be required.
 *
 * @param [in]  what     A description of what is being written, for logging.
 * @param [in]  buf      The buffer in which to write the string, or NULL to
 *                       merely determine the required space.
 * @param [in]  buf_size The size of buf.
 * @param [in]  error    The error code to return for exceeding the specified
 *                       space, UDS_SUCCESS if no logging required.
 * @param [in]  fmt      The sprintf format specification.
 * @param [in]  ap       The variable argument pointer (see <stdarg.h>).
 * @param [out] needed   If non-NULL, the actual amount of string space
 *                       required, which may be smaller or larger than
 *                       buf_size.
 *
 * @return UDS_SUCCESS if the string fits, the value of the error parameter if
 *         the string does not fit and a buffer was supplied, or
 *         UDS_UNEXPECTED_RESULT if vsnprintf fails in some other undocumented
 *         way.
 **/
int __must_check uds_wrap_vsnprintf(const char *what,
				    char *buf,
				    size_t buf_size,
				    int error,
				    const char *fmt,
				    va_list ap,
				    size_t *needed)
	__printf(5, 0);

/**
 * Helper to append a string to a buffer.
 *
 * @param buffer        the place at which to append the string
 * @param buf_end       pointer to the end of the buffer
 * @param fmt           a printf format string
 *
 * @return      the updated buffer position after the append
 *
 * if insufficient space is available, the contents are silently truncated
 **/
char *uds_append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
	__printf(3, 4);

/**
 * Variable-arglist helper to append a string to a buffer.
 *
 * @param buffer   the place at which to append the string
 * @param buf_end  pointer to the end of the buffer
 * @param fmt      a printf format string
 * @param args     printf arguments
 *
 * @return the updated buffer position after the append
 *
 * if insufficient space is available, the contents are silently truncated
 **/
char *uds_v_append_to_buffer(char *buffer, char *buf_end, const char *fmt,
			     va_list args)
	__printf(3, 0);


#endif /* STRING_UTILS_H */
