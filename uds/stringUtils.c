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
 * $Id: //eng/uds-releases/krusty/src/uds/stringUtils.c#3 $
 */

#include "stringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/*****************************************************************************/
int alloc_sprintf(const char *what, char **strp, const char *fmt, ...)
{
	if (strp == NULL) {
		return UDS_INVALID_ARGUMENT;
	}
	va_list args;
	// We want the memory allocation to use our own ALLOCATE/FREE wrappers.
	va_start(args, fmt);
	int count = vsnprintf(NULL, 0, fmt, args) + 1;
	va_end(args);
	int result = ALLOCATE(count, char, what, strp);
	if (result == UDS_SUCCESS) {
		va_start(args, fmt);
		vsnprintf(*strp, count, fmt, args);
		va_end(args);
	}
	if ((result != UDS_SUCCESS) && (what != NULL)) {
		log_error("cannot allocate %s", what);
	}
	return result;
}

/*****************************************************************************/
int wrap_vsnprintf(const char *what,
		   char *buf,
		   size_t buf_size,
		   int error,
		   const char *fmt,
		   va_list ap,
		   size_t *needed)
{
	if (buf == NULL) {
		static char nobuf[1];
		buf = nobuf;
		buf_size = 0;
	}
	int n = vsnprintf(buf, buf_size, fmt, ap);
	if (n < 0) {
		return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
					       "%s: vsnprintf failed", what);
	}
	if (needed) {
		*needed = n;
	}
	if (((size_t) n >= buf_size) && (buf != NULL) &&
	    (error != UDS_SUCCESS)) {
		return logErrorWithStringError(error,
					       "%s: string too long", what);
	}
	return UDS_SUCCESS;
}

/*****************************************************************************/
int fixed_sprintf(const char *what,
		  char *buf,
		  size_t buf_size,
		  int error,
		  const char *fmt,
		  ...)
{
	if (buf == NULL) {
		return UDS_INVALID_ARGUMENT;
	}
	va_list args;
	va_start(args, fmt);
	int result =
		wrap_vsnprintf(what, buf, buf_size, error, fmt, args, NULL);
	va_end(args);
	return result;
}

/*****************************************************************************/
char *
v_append_to_buffer(char *buffer, char *buf_end, const char *fmt, va_list args)
{
	size_t n = vsnprintf(buffer, buf_end - buffer, fmt, args);
	if (n >= (size_t)(buf_end - buffer)) {
		buffer = buf_end;
	} else {
		buffer += n;
	}
	return buffer;
}

/*****************************************************************************/
char *append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	char *pos = v_append_to_buffer(buffer, buf_end, fmt, ap);
	va_end(ap);
	return pos;
}

/*****************************************************************************/
int string_to_signed_int(const char *nptr, int *num)
{
	long value;
	int result = string_to_signed_long(nptr, &value);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if ((value < INT_MIN) || (value > INT_MAX)) {
		return ERANGE;
	}
	*num = (int) value;
	return UDS_SUCCESS;
}

/*****************************************************************************/
int string_to_unsigned_int(const char *nptr, unsigned int *num)
{
	unsigned long value;
	int result = string_to_unsigned_long(nptr, &value);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (value > UINT_MAX) {
		return ERANGE;
	}
	*num = (unsigned int) value;
	return UDS_SUCCESS;
}
