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
 * $Id: //eng/uds-releases/krusty/src/uds/stringUtils.h#6 $
 */

#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <stdarg.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "compiler.h"
#include "typeDefs.h"

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
 * Allocate a string built according to format (our version of asprintf).
 *
 * @param [in]  what    A description of what is being allocated, for error
 *                      logging; if NULL doesn't log anything.
 * @param [out] strp    The pointer in which to store the allocated string.
 * @param [in]  fmt     The sprintf format parameter.
 *
 * @return UDS_SUCCESS, or the appropriately translated asprintf error
 **/
int __must_check uds_alloc_sprintf(const char *what,
				   char **strp,
				   const char *fmt, ...)
	__printf(3, 4);

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

/**
 * Our version of strtok_r, since some platforma apparently don't define it.
 *
 * @param str           On first call, the string to tokenize. On subsequent
 *                      calls, NULL.
 * @param delims        The set of delimiter characters.
 * @param state_ptr     The address of a variable which holds the state of
 *                      the tokenization between calls to uds_next_token.
 *
 * @return the next token if any, or NULL
 **/
char *uds_next_token(char *str, const char *delims, char **state_ptr);

/**
 * Parse a string representing a decimal uint64_t.
 *
 * @param str           The string.
 * @param num           Where to put the number.
 *
 * @return UDS_SUCCESS or the error UDS_INVALID_ARGUMENT if the string
 *         is not in the correct format.
 **/
int __must_check uds_parse_uint64(const char *str, uint64_t *num);

/**
 * Attempt to convert a string to an integer (base 10)
 *
 * @param nptr  Pointer to string to convert
 * @param num   The resulting integer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check uds_string_to_signed_int(const char *nptr, int *num);

/**
 * Attempt to convert a string to a long integer (base 10)
 *
 * @param nptr  Pointer to string to convert
 * @param num   The resulting long integer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check uds_string_to_signed_long(const char *nptr, long *num);

/**
 * Attempt to convert a string to an unsigned integer (base 10).
 *
 * @param nptr  Pointer to string to convert
 * @param num   The resulting unsigned integer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
uds_string_to_unsigned_int(const char *nptr, unsigned int *num);

/**
 * Attempt to convert a string to an unsigned long integer (base 10).
 *
 * @param nptr  Pointer to string to convert
 * @param num   The resulting long unsigned integer
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
uds_string_to_unsigned_long(const char *nptr, unsigned long *num);

#endif /* STRING_UTILS_H */
