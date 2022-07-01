// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "permassert.h"

#include "errors.h"
#include "logger.h"


int uds_assertion_failed(const char *expression_string,
			 int code,
			 const char *module_name,
			 const char *file_name,
			 int line_number,
			 const char *format,
			 ...)
{
	va_list args;

	va_start(args, format);

	uds_log_embedded_message(UDS_LOG_ERR,
				 module_name,
				 "assertion \"",
				 format,
				 args,
				 "\" (%s) failed at %s:%d",
				 expression_string,
				 file_name,
				 line_number);
	uds_log_backtrace(UDS_LOG_ERR);


	va_end(args);

	return code;
}
