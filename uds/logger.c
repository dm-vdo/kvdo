/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/logger.c#2 $
 */

#include "logger.h"

#include "common.h"
#include "errors.h"
#include "stringUtils.h"
#include "threads.h"
#include "uds.h"

typedef struct {
  const char *name;
  const int   priority;
} PriorityName;

static const PriorityName PRIORITIES[] = {
  { "ALERT",     LOG_ALERT   },
  { "CRITICAL",  LOG_CRIT    },
  { "CRIT",      LOG_CRIT    },
  { "DEBUG",     LOG_DEBUG   },
  { "EMERGENCY", LOG_EMERG   },
  { "EMERG",     LOG_EMERG   },
  { "ERROR",     LOG_ERR     },
  { "ERR",       LOG_ERR     },
  { "INFO",      LOG_INFO    },
  { "NOTICE",    LOG_NOTICE  },
  { "PANIC",     LOG_EMERG   },
  { "WARN",      LOG_WARNING },
  { "WARNING",   LOG_WARNING },
  { NULL,        -1          },
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

static int logLevel = LOG_INFO;

/*****************************************************************************/
int getLogLevel(void)
{
  return logLevel;
}

/*****************************************************************************/
void setLogLevel(int newLogLevel)
{
  logLevel = newLogLevel;
}

/*****************************************************************************/
int stringToPriority(const char *string)
{
  for (int i = 0; PRIORITIES[i].name != NULL; i++) {
    if (strcasecmp(string, PRIORITIES[i].name) == 0) {
      return PRIORITIES[i].priority;
    }
  }
  return LOG_INFO;
}

/*****************************************************************************/
const char *priorityToString(int priority)
{
  if ((priority < 0) || (priority >= (int) COUNT_OF(PRIORITY_STRINGS))) {
    return "unknown";
  }
  return PRIORITY_STRINGS[priority];
}

/*****************************************************************************/
void logEmbeddedMessage(int         priority,
                        const char *prefix,
                        const char *fmt1,
                        va_list     args1,
                        const char *fmt2,
                        ...)
{
  va_list ap;
  va_start(ap, fmt2);
  logMessagePack(priority, prefix, fmt1, args1, fmt2, ap);
  va_end(ap);
}

#pragma GCC diagnostic push
/*
 * GCC (version 8.1.1 20180502 (Red Hat 8.1.1-1)) on Fedora 28 seems
 * to think that this function should get a printf format
 * attribute. But we have no second format string, and no additional
 * arguments at the call site, and GCC also gets unhappy trying to
 * analyze the format and values when there are none. So we'll just
 * shut it up.
 */
#pragma GCC diagnostic ignored "-Wsuggest-attribute=format"
/**
 * Log a message.
 *
 * This helper function exists solely to create a valid va_list with
 * no useful info. It does the real work of vLogMessage, which wants a
 * second va_list object to pass down.
 *
 * @param  priority The syslog priority value for the message.
 * @param  format   The format of the message (a printf style format)
 * @param  args     The variadic argument list of format parameters.
 **/
static void vLogMessageHelper(int         priority,
                              const char *format,
                              va_list     args,
                              ...)
{
  va_list dummy;
  va_start(dummy, args);
  logMessagePack(priority, NULL, format, args, NULL, dummy);
  va_end(dummy);
}
#pragma GCC diagnostic pop

/*****************************************************************************/
void vLogMessage(int priority, const char *format, va_list args)
{
  vLogMessageHelper(priority, format, args);
}

/*****************************************************************************/
void logMessage(int priority, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(priority, format, args);
  va_end(args);
}

/*****************************************************************************/
void logDebug(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_DEBUG, format, args);
  va_end(args);
}

/*****************************************************************************/
void logInfo(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_INFO, format, args);
  va_end(args);
}

/*****************************************************************************/
void logNotice(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_NOTICE, format, args);
  va_end(args);
}

/*****************************************************************************/
void logWarning(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_WARNING, format, args);
  va_end(args);
}

/*****************************************************************************/
void logError(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_ERR, format, args);
  va_end(args);
}

/*****************************************************************************/
int vLogWithStringError(int         priority,
                        int         errnum,
                        const char *format,
                        va_list     args)
{
  char errbuf[ERRBUF_SIZE];
  logEmbeddedMessage(priority, NULL, format, args, ": %s (%d)",
                     stringError(errnum, errbuf, sizeof(errbuf)),
                     errnum);
  return errnum;
}

/*****************************************************************************/
int logWithStringError(int priority, int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(priority, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logErrorWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_ERR, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logWarningWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_WARNING, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logDebugWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_DEBUG, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logInfoWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_INFO, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logNoticeWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_NOTICE, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logFatalWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_CRIT, errnum, format, args);
  va_end(args);
  return errnum;
}

/*****************************************************************************/
int logUnrecoverable(int errnum, const char *format, ...)
{
  if ((errnum == UDS_SUCCESS || errnum == UDS_QUEUED) || (errnum == 0)) {
    return errnum;
  }
  va_list args;
  va_start(args, format);
  vLogWithStringError(LOG_CRIT, errnum, format, args);
  va_end(args);
  return makeUnrecoverable(errnum);
}

/*****************************************************************************/
void logFatal(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_CRIT, format, args);
  va_end(args);
}
