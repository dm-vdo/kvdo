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
 * $Id: //eng/uds-releases/krusty/src/uds/logger.c#13 $
 */

#include "logger.h"

#include "common.h"
#include "errors.h"
#include "stringUtils.h"
#include "threads.h"
#include "uds.h"

typedef struct {
	const char *name;
	const int priority;
} PriorityName;

static const PriorityName PRIORITIES[] = {
	{ "ALERT", LOG_ALERT },
	{ "CRITICAL", LOG_CRIT },
	{ "CRIT", LOG_CRIT },
	{ "DEBUG", LOG_DEBUG },
	{ "EMERGENCY", LOG_EMERG },
	{ "EMERG", LOG_EMERG },
	{ "ERROR", LOG_ERR },
	{ "ERR", LOG_ERR },
	{ "INFO", LOG_INFO },
	{ "NOTICE", LOG_NOTICE },
	{ "PANIC", LOG_EMERG },
	{ "WARN", LOG_WARNING },
	{ "WARNING", LOG_WARNING },
	{ NULL, -1 },
};

static const char *const PRIORITY_STRINGS[] = {
	"EMERGENCY",
	"ALERT",
	"CRITICAL",
	"ERROR",
	"WARN",
	"NOTICE",
	"INFO",
	"DEBUG",
};

static int log_level = LOG_INFO;

/**********************************************************************/
int get_log_level(void)
{
	return log_level;
}

/**********************************************************************/
void set_log_level(int new_log_level)
{
	log_level = new_log_level;
}

/**********************************************************************/
int string_to_priority(const char *string)
{
	int i;
	for (i = 0; PRIORITIES[i].name != NULL; i++) {
		if (strcasecmp(string, PRIORITIES[i].name) == 0) {
			return PRIORITIES[i].priority;
		}
	}
	return LOG_INFO;
}

/**********************************************************************/
const char *priority_to_string(int priority)
{
	if ((priority < 0) || (priority >= (int) COUNT_OF(PRIORITY_STRINGS))) {
		return "unknown";
	}
	return PRIORITY_STRINGS[priority];
}

/**********************************************************************/
void uds_log_embedded_message(int priority,
			      const char *module,
			      const char *prefix,
			      const char *fmt1,
			      va_list args1,
			      const char *fmt2,
			      ...)
{
	va_list ap;
	va_start(ap, fmt2);
	uds_log_message_pack(priority, module, prefix, fmt1, args1, fmt2, ap);
	va_end(ap);
}

/**********************************************************************/
int vlog_strerror(int priority, int errnum, const char *format, va_list args)
{
	// XXX plumb module name through strerror functions
	const char *module = NULL;
	char errbuf[ERRBUF_SIZE];
	uds_log_embedded_message(priority,
				 module,
				 NULL,
				 format,
				 args,
				 ": %s (%d)",
				 string_error(errnum, errbuf, sizeof(errbuf)),
				 errnum);
	return errnum;
}

/**********************************************************************/
int log_strerror(int priority, int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(priority, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_error_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_ERR, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_warning_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_WARNING, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_debug_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_DEBUG, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_info_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_INFO, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_notice_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_NOTICE, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_fatal_strerror(int errnum, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vlog_strerror(LOG_CRIT, errnum, format, args);
	va_end(args);
	return errnum;
}

/**********************************************************************/
int log_unrecoverable(int errnum, const char *format, ...)
{
	if (is_successful(errnum)) {
		return errnum;
	}
	va_list args;
	va_start(args, format);
	vlog_strerror(LOG_CRIT, errnum, format, args);
	va_end(args);
	return make_unrecoverable(errnum);
}
