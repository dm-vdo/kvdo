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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/loggerLinuxKernel.c#1 $
 */

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "logger.h"

/**********************************************************************/
void openLogger(void)
{
}

/**********************************************************************/
void closeLogger(void)
{
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
    printk("%s[%s]: ", THIS_MODULE->name, type);
  } else {
    // XXX need the equivalent of VDO's deviceInstance here
    printk("%s: %s: ", THIS_MODULE->name, current->comm);
  }
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
