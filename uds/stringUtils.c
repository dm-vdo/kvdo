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
 * $Id: //eng/uds-releases/krusty-rhel9.0-beta/src/uds/stringUtils.c#1 $
 */

#include "stringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/**********************************************************************/
int uds_alloc_sprintf(const char *what, char **strp, const char *fmt, ...)
{
	va_list args;
	int result;
	int count;
	if (strp == NULL) {
		return UDS_INVALID_ARGUMENT;
	}
	// We want the memory allocation to use our own UDS_ALLOCATE/UDS_FREE
	// wrappers.
	va_start(args, fmt);
	count = vsnprintf(NULL, 0, fmt, args) + 1;
	va_end(args);
	result = UDS_ALLOCATE(count, char, what, strp);
	if (result == UDS_SUCCESS) {
		va_start(args, fmt);
		vsnprintf(*strp, count, fmt, args);
		va_end(args);
	}
	if ((result != UDS_SUCCESS) && (what != NULL)) {
		uds_log_error("cannot allocate %s", what);
	}
	return result;
}

/**********************************************************************/
int uds_wrap_vsnprintf(const char *what,
		       char *buf,
		       size_t buf_size,
		       int error,
		       const char *fmt,
		       va_list ap,
		       size_t *needed)
{
	int n;
	if (buf == NULL) {
		static char nobuf[1];
		buf = nobuf;
		buf_size = 0;
	}
	n = vsnprintf(buf, buf_size, fmt, ap);
	if (n < 0) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "%s: vsnprintf failed", what);
	}
	if (needed) {
		*needed = n;
	}
	if (((size_t) n >= buf_size) && (buf != NULL) &&
	    (error != UDS_SUCCESS)) {
		return uds_log_error_strerror(error,
					      "%s: string too long", what);
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int uds_fixed_sprintf(const char *what,
		      char *buf,
		      size_t buf_size,
		      int error,
		      const char *fmt,
		      ...)
{
	va_list args;
	int result;
	if (buf == NULL) {
		return UDS_INVALID_ARGUMENT;
	}
	va_start(args, fmt);
	result = uds_wrap_vsnprintf(what, buf, buf_size, error, fmt, args,
				    NULL);
	va_end(args);
	return result;
}

/**********************************************************************/
char *uds_v_append_to_buffer(char *buffer, char *buf_end, const char *fmt,
			     va_list args)
{
	size_t n = vsnprintf(buffer, buf_end - buffer, fmt, args);
	if (n >= (size_t)(buf_end - buffer)) {
		buffer = buf_end;
	} else {
		buffer += n;
	}
	return buffer;
}

/**********************************************************************/
char *uds_append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
{
	va_list ap;
	char *pos;

	va_start(ap, fmt);
	pos = uds_v_append_to_buffer(buffer, buf_end, fmt, ap);
	va_end(ap);
	return pos;
}

/**********************************************************************/
int uds_string_to_signed_int(const char *nptr, int *num)
{
	long value;
	int result = uds_string_to_signed_long(nptr, &value);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if ((value < INT_MIN) || (value > INT_MAX)) {
		return ERANGE;
	}
	*num = (int) value;
	return UDS_SUCCESS;
}

/**********************************************************************/
int uds_string_to_unsigned_int(const char *nptr, unsigned int *num)
{
	unsigned long value;
	int result = uds_string_to_unsigned_long(nptr, &value);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (value > UINT_MAX) {
		return ERANGE;
	}
	*num = (unsigned int) value;
	return UDS_SUCCESS;
}
