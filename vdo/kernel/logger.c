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
 * $Id: //eng/vdo-releases/magnesium-rhel7.6/src/c++/vdo/kernel/logger.c#1 $
 */

#include "logger.h"

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/module.h>

#include "errors.h"
#include "threadDevice.h"

static const int  DEFAULT_PRIORITY = LOG_INFO;

typedef struct {
  const char *name;
  const int   priority;
} PRIORITY_NAMES;

static const PRIORITY_NAMES PRIORITIES[] = {
  { "ALERT",     LOG_ALERT },
  { "CRIT",      LOG_CRIT },
  { "CRITICAL",  LOG_CRIT },
  { "DEBUG",     LOG_DEBUG },
  { "EMERG",     LOG_EMERG },
  { "EMERGENCY", LOG_EMERG },
  { "ERR",       LOG_ERR },
  { "ERROR",     LOG_ERR },
  { "INFO",      LOG_INFO },
  { "NOTICE",    LOG_NOTICE },
  { "PANIC",     LOG_EMERG },
  { "WARN",      LOG_WARNING },
  { "WARNING",   LOG_WARNING },
  { NULL, -1 },
};

enum {
  PRIORITY_COUNT = 8
};

static const char *PRIORITY_STRINGS[] = {
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

/**********************************************************************/
int stringToPriority(const char *string)
{
  for (int i = 0; PRIORITIES[i].name != NULL; i++) {
    if (strcasecmp(string, PRIORITIES[i].name) == 0) {
      return PRIORITIES[i].priority;
    }
  }
  return DEFAULT_PRIORITY;
}

/**********************************************************************/
int getLogLevel(void)
{
  return logLevel;
}

/**********************************************************************/
void setLogLevel(int newLogLevel)
{
  logLevel = newLogLevel;
}

/**********************************************************************/
const char *priorityToString(int priority)
{
  if ((priority < 0) || (priority >= PRIORITY_COUNT)) {
    return "unknown";
  }
  return PRIORITY_STRINGS[priority];
}

/**********************************************************************/
static void startLoggingForPriority(int priority)
{
  switch(priority) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
      printk(KERN_CRIT);
      break;
    case LOG_ERR:
      printk(KERN_ERR);
      break;
    case LOG_WARNING:
      printk(KERN_WARNING);
      break;
    case LOG_NOTICE:
      printk(KERN_NOTICE);
      break;
    case LOG_INFO:
      printk(KERN_INFO);
      break;
    case LOG_DEBUG:
      printk(KERN_DEBUG);
      break;
  }
  if (in_interrupt()) {
    char *type = "INTR";
    if (in_nmi()) {
      type = "NMI";
    } else if (in_irq()) {
      type = "HI";
    } else if (in_softirq()) {
      type = "SI";
    }
    printk("%s:[%s]: ", THIS_MODULE->name, type);
    return;
  }
  // Not at interrupt level; we have a process we can look at, and
  // might have a device ID.
  int deviceInstance = getThreadDeviceID();
  if (deviceInstance != -1) {
    printk("%s%u:%s: ", THIS_MODULE->name, deviceInstance, current->comm);
    return;
  }
  size_t nameLen = strlen(THIS_MODULE->name);
  if (((current->flags & PF_KTHREAD) != 0)
      && (strncmp(THIS_MODULE->name, current->comm, nameLen) == 0)) {
    /*
     * It's a kernel thread starting with "kvdo" (or whatever). Assume it's
     * ours and that its name is sufficient.
     */
    printk("%s: ", current->comm);
    return;
  }
  // Identify the module, the device counter, and the process.
  printk("%s: %s: ", THIS_MODULE->name, current->comm);
}

/**********************************************************************/
void logMessagePack(int         priority,
                    const char *prefix,
                    const char *fmt1,
                    va_list     args1,
                    const char *fmt2,
                    va_list     args2)
{
  if (priority > logLevel) {
    return;
  }
  startLoggingForPriority(priority);
  if (prefix != NULL) {
    printk("%s", prefix);
  }
  if (fmt1 != NULL) {
    vprintk(fmt1, args1);
  }
  if (fmt2 != NULL) {
    vprintk(fmt2, args2);
  }
  printk("\n");
}

/**********************************************************************/
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

/**********************************************************************/
void vLogMessage(int priority, const char *format, va_list args)
{
  va_list dummy;
  logMessagePack(priority, NULL, format, args, NULL, dummy);
}

/**********************************************************************/
void logMessage(int priority, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(priority, format, args);
  va_end(args);
}

/**********************************************************************/
__attribute__((format(printf, 2, 3)))
static void logAtLevel(int priority, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(priority, format, args);
  va_end(args);
}

/**********************************************************************/
void logDebug(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_DEBUG, format, args);
  va_end(args);
}

/**********************************************************************/
void logInfo(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_INFO, format, args);
  va_end(args);
}

/**********************************************************************/
void logNotice(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_NOTICE, format, args);
  va_end(args);
}

/**********************************************************************/
void logWarning(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_WARNING, format, args);
  va_end(args);
}

/**********************************************************************/
void logError(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_ERR, format, args);
  va_end(args);
}

/**********************************************************************/
void vLogError(const char *format, va_list args)
{
  vLogMessage(LOG_ERR, format, args);
}

/**********************************************************************/
void logBacktrace(int priority)
{
  logAtLevel(priority, "[backtrace]");
  if (priority > logLevel) {
    return;
  }
  dump_stack();
}

/**********************************************************************/
int vLogWithStringError(int         priority,
                        int         errnum,
                        const char *format,
                        va_list     args)
{
  char errbuf[ERRBUF_SIZE] = "";
  logEmbeddedMessage(priority, NULL, format, args, ": %s (%d)",
                     stringError(errnum, errbuf, sizeof(errbuf)),
                     errnum);
  return errnum;
}

/**********************************************************************/
int logWithStringError(int priority, int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(priority, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int logErrorWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_ERR, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int vLogErrorWithStringError(int errnum, const char *format, va_list args)
{
  vLogWithStringError(LOG_ERR, errnum, format, args);
  return errnum;
}

/**********************************************************************/
int logWarningWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_WARNING, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int logDebugWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_DEBUG, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int logInfoWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_INFO, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int logNoticeWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_NOTICE, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
int logFatalWithStringError(int errnum, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogWithStringError(LOG_CRIT, errnum, format, args);
  va_end(args);
  return errnum;
}

/**********************************************************************/
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

/**********************************************************************/
void logFatal(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  vLogMessage(LOG_CRIT, format, args);
  va_end(args);
}

/**********************************************************************/
void pauseForLogger(void)
{
  // Hopefully, a few milliseconds of sleep will be large enough
  // for the kernel log buffer to be flushed.
  msleep(4);
}
