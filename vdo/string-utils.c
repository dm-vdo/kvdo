// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "string-utils.h"

#include "errors.h"
#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"
#include "uds.h"

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

char *uds_append_to_buffer(char *buffer, char *buf_end, const char *fmt, ...)
{
	va_list ap;
	char *pos;

	va_start(ap, fmt);
	pos = uds_v_append_to_buffer(buffer, buf_end, fmt, ap);
	va_end(ap);
	return pos;
}

