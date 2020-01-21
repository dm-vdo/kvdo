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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/logger.c#4 $
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
static const char *priorityToLogLevel(int priority)
{
  switch (priority) {
    case LOG_EMERG:
    case LOG_ALERT:
    case LOG_CRIT:
      return KERN_CRIT;
    case LOG_ERR:
      return KERN_ERR;
    case LOG_WARNING:
      return KERN_WARNING;
    case LOG_NOTICE:
      return KERN_NOTICE;
    case LOG_INFO:
      return KERN_INFO;
    case LOG_DEBUG:
      return KERN_DEBUG;
    default:
      return "";
  }
}

/**********************************************************************/
static const char *getCurrentInterruptType(void)
{
  if (in_nmi()) {
    return "NMI";
  }
  if (in_irq()) {
    return "HI";
  }
  if (in_softirq()) {
    return "SI";
  }
  return "INTR";
}

/**
 * Emit a log message to the kernel log in a format suited to the current
 * thread context. Context info formats:
 *
 * interrupt:       kvdo[NMI]: blah
 * thread w/dev id: kvdo12:myprog: blah
 * kvdo thread:     kvdo12:foobarQ: blah
 * other thread:    kvdo: myprog: blah
 *
 * Fields: module name, interrupt level, process name, device ID.
 *
 * @param level       A string describing the logging level
 * @param moduleName  The name of the module doing the logging
 * @param prefix      The prefix of the log message
 * @param vaf1        The first message format descriptor
 * @param vaf2        The second message format descriptor
 **/
static void emitLogMessage(const char             *level,
                           const char             *moduleName,
                           const char             *prefix,
                           const struct va_format *vaf1,
                           const struct va_format *vaf2)
{
  if (in_interrupt()) {
    printk("%s%s[%s]: %s%pV%pV\n",
           level, moduleName, getCurrentInterruptType(),
           prefix, vaf1, vaf2);
    return;
  }

  // Not at interrupt level; we have a process we can look at, and
  // might have a device ID.
  int deviceInstance = getThreadDeviceID();
  if (deviceInstance != -1) {
    printk("%s%s%u:%s: %s%pV%pV\n",
           level, moduleName, deviceInstance, current->comm,
           prefix, vaf1, vaf2);
    return;
  }

  if (((current->flags & PF_KTHREAD) != 0)
      && (strncmp(moduleName, current->comm, strlen(moduleName)) == 0)) {
    /*
     * It's a kernel thread starting with "kvdo" (or whatever). Assume it's
     * ours and that its name is sufficient.
     */
    printk("%s%s: %s%pV%pV\n",
           level, current->comm,
           prefix, vaf1, vaf2);
    return;
  }

  // Identify the module and the process.
  printk("%s%s: %s: %s%pV%pV\n",
         level, moduleName, current->comm,
         prefix, vaf1, vaf2);
}

/**********************************************************************/
void logMessagePack(int         priority,
                    const char *prefix,
                    const char *fmt1,
                    va_list     args1,
                    const char *fmt2,
                    va_list     args2)
{
  if (priority > getLogLevel()) {
    return;
  }

  /*
   * The kernel's printk has some magic for indirection to a secondary
   * va_list. It wants us to supply a pointer to the va_list.
   *
   * However, va_list varies across platforms and can be an array
   * type, which makes passing it around as an argument kind of
   * tricky, due to the automatic conversion to a pointer. This makes
   * taking the address of the argument a dicey thing; if we use "&a"
   * it works fine for non-array types, but for array types we get the
   * address of a pointer. Functions like va_copy and sprintf don't
   * care as they get "va_list" values passed and are written to do
   * the right thing, but printk explicitly wants the address of the
   * va_list.
   *
   * So, we copy the va_list values to ensure that "&" consistently
   * works the way we want.
   */
  va_list args1Copy;
  va_copy(args1Copy, args1);
  va_list args2Copy;
  va_copy(args2Copy, args2);
  struct va_format vaf1 = {
    .fmt = (fmt1 != NULL) ? fmt1 : "",
    .va  = &args1Copy,
  };
  struct va_format vaf2 = {
    .fmt = (fmt2 != NULL) ? fmt2 : "",
    .va  = &args2Copy,
  };

  if (prefix == NULL) {
    prefix = "";
  }

  emitLogMessage(priorityToLogLevel(priority), THIS_MODULE->name,
                 prefix, &vaf1, &vaf2);

  va_end(args1Copy);
  va_end(args2Copy);
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
