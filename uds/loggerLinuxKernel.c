/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/loggerLinuxKernel.c#2 $
 */

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "logger.h"

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

  /*
   * Context info formats:
   *
   * interrupt:   uds[NMI]: blah
   * process:     uds: myprog: blah
   *
   * Fields: module name, interrupt level or process name.
   *
   * XXX need the equivalent of VDO's deviceInstance here
   */
  if (in_interrupt()) {
    printk("%s%s[%s]: %s%pV%pV\n", priorityToLogLevel(priority),
	   THIS_MODULE->name, getCurrentInterruptType(), prefix, &vaf1, &vaf2);
  } else {
    printk("%s%s: %s: %s%pV%pV\n", priorityToLogLevel(priority),
	   THIS_MODULE->name, current->comm, prefix, &vaf1, &vaf2);
  }

  va_end(args1Copy);
  va_end(args2Copy);
}

/**********************************************************************/
void logBacktrace(int priority)
{
  if (priority > getLogLevel()) {
    return;
  }
  logMessage(priority, "[backtrace]");
  dump_stack();
}

/**********************************************************************/
void pauseForLogger(void)
{
  // Hopefully, a few milliseconds of sleep will be large enough
  // for the kernel log buffer to be flushed.
  msleep(4);
}
