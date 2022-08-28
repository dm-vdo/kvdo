// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "logger.h"

#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/module.h>
#include <linux/sched.h>

#include "thread-device.h"
#include "uds-threads.h"

struct priority_name {
	const char *name;
	const int priority;
};

static const struct priority_name PRIORITIES[] = {
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

int get_uds_log_level(void)
{
	return log_level;
}

void set_uds_log_level(int new_log_level)
{
	log_level = new_log_level;
}

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

const char *uds_log_priority_to_string(int priority)
{
	if ((priority < 0) ||
	    (priority >= (int) ARRAY_SIZE(PRIORITY_STRINGS))) {
		return "unknown";
	}
	return PRIORITY_STRINGS[priority];
}

static const char *priority_to_log_level(int priority)
{
	switch (priority) {
	case UDS_LOG_EMERG:
	case UDS_LOG_ALERT:
	case UDS_LOG_CRIT:
		return KERN_CRIT;
	case UDS_LOG_ERR:
		return KERN_ERR;
	case UDS_LOG_WARNING:
		return KERN_WARNING;
	case UDS_LOG_NOTICE:
		return KERN_NOTICE;
	case UDS_LOG_INFO:
		return KERN_INFO;
	case UDS_LOG_DEBUG:
		return KERN_DEBUG;
	default:
		return "";
	}
}

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

/**
 * Emit a log message to the kernel log in a format suited to the current
 * thread context. Context info formats:
 *
 * interrupt:           uds[NMI]: blah
 * kvdo thread:         kvdo12:foobarQ: blah
 * thread w/device id:  kvdo12:myprog: blah
 * other thread:        uds: myprog: blah
 *
 * Fields: module name, interrupt level, process name, device ID.
 *
 * @param level   A string describing the logging level
 * @param module  The name of the module doing the logging
 * @param prefix  The prefix of the log message
 * @param vaf1    The first message format descriptor
 * @param vaf2    The second message format descriptor
 **/
static void emit_log_message(const char *level,
			     const char *module,
			     const char *prefix,
			     const struct va_format *vaf1,
			     const struct va_format *vaf2)
{
	int device_instance;

	/*
	 * In interrupt context, identify the interrupt type and module.
	 * Ignore the process/thread since it could be anything.
	 */
	if (in_interrupt()) {
		const char *type = get_current_interrupt_type();

		printk("%s%s[%s]: %s%pV%pV\n",
		       level, module, type, prefix, vaf1, vaf2);
		return;
	}

	/*
	 * Not at interrupt level; we have a process we can look at, and
	 * might have a device ID.
	 */
	device_instance = uds_get_thread_device_id();
	if (device_instance >= 0) {
		printk("%s%s%u:%s: %s%pV%pV\n",
		       level,
		       module,
		       device_instance,
		       current->comm,
		       prefix,
		       vaf1,
		       vaf2);
		return;
	}

	/*
	 * If it's a kernel thread and the module name is a prefix of its
	 * name, assume it is ours and only identify the thread.
	 */
	if (((current->flags & PF_KTHREAD) != 0) &&
	    (strncmp(module, current->comm, strlen(module)) == 0)) {
		printk("%s%s: %s%pV%pV\n",
		       level, current->comm, prefix, vaf1, vaf2);
		return;
	}

	/* Identify the module and the process. */
	printk("%s%s: %s: %s%pV%pV\n",
	       level, module, current->comm, prefix, vaf1, vaf2);
}

void uds_log_message_pack(int priority,
			  const char *module,
			  const char *prefix,
			  const char *fmt1,
			  va_list args1,
			  const char *fmt2,
			  va_list args2)
{
	const char *level;
	va_list args1_copy, args2_copy;
	struct va_format vaf1, vaf2;

	if (priority > get_uds_log_level()) {
		return;
	}

	level = priority_to_log_level(priority);
	if (module == NULL) {
		module = UDS_LOGGING_MODULE_NAME;
	}
	if (prefix == NULL) {
		prefix = "";
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

	emit_log_message(level, module, prefix, &vaf1, &vaf2);

	va_end(args1_copy);
	va_end(args2_copy);
}

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

int uds_vlog_strerror(int priority,
		      int errnum,
		      const char *module,
		      const char *format,
		      va_list args)
{
	char errbuf[UDS_MAX_ERROR_MESSAGE_SIZE];
	const char *message = uds_string_error(errnum, errbuf, sizeof(errbuf));

	uds_log_embedded_message(priority,
				 module,
				 NULL,
				 format,
				 args,
				 ": %s (%d)",
				 message,
				 errnum);
	return errnum;
}

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

void uds_log_backtrace(int priority)
{
	if (priority > get_uds_log_level()) {
		return;
	}
	dump_stack();
}

void __uds_log_message(int priority,
		       const char *module,
		       const char *format,
		       ...)
{
	va_list args;

	va_start(args, format);
	uds_log_embedded_message(priority, module, NULL,
				 format, args, "%s", "");
	va_end(args);
}

void uds_pause_for_logger(void)
{
	/* Allow a few milliseconds for the kernel log buffer to be flushed. */
	fsleep(4000);
}
