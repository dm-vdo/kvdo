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
 * $Id: //eng/uds-releases/krusty/src/uds/logger.c#20 $
 */

#include "logger.h"

#include "common.h"
#include "errors.h"
#include "stringUtils.h"
#include "uds-threads.h"
#include "uds.h"

typedef struct {
	const char *name;
	const int priority;
} PriorityName;

static const PriorityName PRIORITIES[] = {
	{ "ALERT", UDS_LOG_ALERT },
	{ "CRITICAL", UDS_LOG_CRIT },
	{ "CRIT", UDS_LOG_CRIT },
	{ "DEBUG", UDS_LOG_DEBUG },
	{ "EMERGENCY", UDS_LOG_EMERG },
	{ "EMERG", UDS_LOG_EMERG },
	{ "ERROR", UDS_LOG_ERR },
	{ "ERR", UDS_LOG_ERR },
	{ "INFO", UDS_LOG_INFO },
	{ "NOTICE", UDS_LOG_NOTICE },
	{ "PANIC", UDS_LOG_EMERG },
	{ "WARN", UDS_LOG_WARNING },
	{ "WARNING", UDS_LOG_WARNING },
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

static int log_level = UDS_LOG_INFO;

/**********************************************************************/
int get_uds_log_level(void)
{
	return log_level;
}

/**********************************************************************/
void set_uds_log_level(int new_log_level)
{
	log_level = new_log_level;
}

/**********************************************************************/
int uds_log_string_to_priority(const char *string)
{
	int i;
	for (i = 0; PRIORITIES[i].name != NULL; i++) {
		if (strcasecmp(string, PRIORITIES[i].name) == 0) {
			return PRIORITIES[i].priority;
		}
	}
	return UDS_LOG_INFO;
}

/**********************************************************************/
const char *uds_log_priority_to_string(int priority)
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
int uds_vlog_strerror(int priority,
		      int errnum,
		      const char *module,
		      const char *format,
		      va_list args)
{
	char errbuf[ERRBUF_SIZE];
	uds_log_embedded_message(priority,
				 module,
				 NULL,
				 format,
				 args,
				 ": %s (%u)",
				 string_error(errnum, errbuf, sizeof(errbuf)),
				 errnum);
	return errnum;
}

/**********************************************************************/
int __uds_log_strerror(int priority,
		       int errnum,
		       const char *module,
		       const char *format,
		       ...)
{
	va_list args;

	va_start(args, format);
	uds_vlog_strerror(priority, errnum, module, format, args);
	va_end(args);
	return errnum;
}
