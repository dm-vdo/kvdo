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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/loggerLinuxKernel.c#5 $
 */

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "logger.h"

/**********************************************************************/
static const char *priority_to_log_level(int priority)
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
static const char *get_current_interrupt_type(void)
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
void log_message_pack(int priority,
		      const char *prefix,
		      const char *fmt1,
		      va_list args1,
		      const char *fmt2,
		      va_list args2)
{
	va_list args1_copy, args2_copy;
	struct va_format vaf1, vaf2;

	if (priority > get_log_level()) {
		return;
	}

	/*
	 * It is implementation dependent whether va_list is defined as an
	 * array type that decays to a pointer when passed as an
	 * argument. Copy args1 and args2 with va_copy so that vaf1 and
	 * vaf2 get proper va_list pointers irrespective of how va_list is
	 * defined.
	 */
	va_copy(args1_copy, args1);
	vaf1.fmt = fmt1;
	vaf1.va = &args1_copy;

	va_copy(args2_copy, args2);
	vaf2.fmt = fmt2;
	vaf2.va = &args2_copy;

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
		printk("%s%s[%s]: %s%pV%pV\n",
		       priority_to_log_level(priority),
		       THIS_MODULE->name,
		       get_current_interrupt_type(),
		       prefix,
		       &vaf1,
		       &vaf2);
	} else {
		printk("%s%s: %s: %s%pV%pV\n",
		       priority_to_log_level(priority),
		       THIS_MODULE->name,
		       current->comm,
		       prefix,
		       &vaf1,
		       &vaf2);
	}

	va_end(args1_copy);
	va_end(args2_copy);
}

/**********************************************************************/
void log_backtrace(int priority)
{
	if (priority > get_log_level()) {
		return;
	}
	log_message(priority, "%s", "[backtrace]");
	dump_stack();
}

/**********************************************************************/
void pause_for_logger(void)
{
	// Hopefully, a few milliseconds of sleep will be large enough
	// for the kernel log buffer to be flushed.
	msleep(4);
}
