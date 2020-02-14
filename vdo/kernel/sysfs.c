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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/sysfs.c#8 $
 */

#include "sysfs.h"

#include <linux/module.h>
#include <linux/version.h>

#include "dedupeIndex.h"
#include "dmvdo.h"
#include "logger.h"

extern int default_max_requests_active;

struct vdo_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kvdo_module_globals *kvdo_globals,
			struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct kvdo_module_globals *kvdo_globals,
			 const char *value,
			 size_t count);
	// Location of value, if .show == show_int or show_uint or show_bool.
	void *value_ptr;
};

static char *status_strings[] = {
	"UNINITIALIZED",
	"READY",
	"SHUTTING DOWN",
};

/**********************************************************************/
static ssize_t vdo_status_show(struct kvdo_module_globals *kvdo_globals,
			       struct attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%s\n", status_strings[kvdo_globals->status]);
}

/**********************************************************************/
static ssize_t vdo_log_level_show(struct kvdo_module_globals *kvdo_globals,
				  struct attribute *attr,
				  char *buf)
{
	return sprintf(buf, "%s\n", priorityToString(getLogLevel()));
}

/**********************************************************************/
static ssize_t vdo_log_level_store(struct kvdo_module_globals *kvdo_globals,
				   const char *buf,
				   size_t n)
{
	static char internal_buf[11];

	if (n > 10) {
		return -EINVAL;
	}

	memset(internal_buf, '\000', sizeof(internal_buf));
	memcpy(internal_buf, buf, n);
	if (internal_buf[n - 1] == '\n') {
		internal_buf[n - 1] = '\000';
	}
	setLogLevel(stringToPriority(internal_buf));
	return n;
}

/**********************************************************************/
static ssize_t scan_int(const char *buf,
			size_t n,
			int *value_ptr,
			int minimum,
			int maximum)
{
	if (n > 12) {
		return -EINVAL;
	}
	unsigned int value;

	if (sscanf(buf, "%d", &value) != 1) {
		return -EINVAL;
	}
	if (value < minimum) {
		value = minimum;
	} else if (value > maximum) {
		value = maximum;
	}
	*value_ptr = value;
	return n;
}

/**********************************************************************/
static ssize_t show_int(struct kvdo_module_globals *kvdo_globals,
			struct attribute *attr,
			char *buf)
{
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);

	return sprintf(buf, "%d\n", *(int *) vdo_attr->value_ptr);
}

/**********************************************************************/
static ssize_t scan_uint(const char *buf,
			 size_t n,
			 unsigned int *value_ptr,
			 unsigned int minimum,
			 unsigned int maximum)
{
	if (n > 12) {
		return -EINVAL;
	}
	unsigned int value;

	if (sscanf(buf, "%u", &value) != 1) {
		return -EINVAL;
	}
	if (value < minimum) {
		value = minimum;
	} else if (value > maximum) {
		value = maximum;
	}
	*value_ptr = value;
	return n;
}

/**********************************************************************/
static ssize_t show_uint(struct kvdo_module_globals *kvdo_globals,
			 struct attribute *attr,
			 char *buf)
{
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);

	return sprintf(buf, "%u\n", *(unsigned int *) vdo_attr->value_ptr);
}

/**********************************************************************/
static ssize_t scan_bool(const char *buf, size_t n, bool *value_ptr)
{
	unsigned int int_value = 0;

	n = scan_uint(buf, n, &int_value, 0, 1);
	if (n > 0) {
		*value_ptr = (int_value != 0);
	}
	return n;
}

/**********************************************************************/
static ssize_t show_bool(struct kvdo_module_globals *kvdo_globals,
			 struct attribute *attr,
			 char *buf)
{
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);

	return sprintf(buf, "%u\n", *(bool *) vdo_attr->value_ptr ? 1 : 0);
}

/**********************************************************************/
static ssize_t
vdo_trace_recording_store(struct kvdo_module_globals *kvdo_globals,
			  const char *buf,
			  size_t n)
{
	return scan_bool(buf, n, &trace_recording);
}

/**********************************************************************/
static ssize_t
vdo_max_req_active_store(struct kvdo_module_globals *kvdo_globals,
			 const char *buf,
			 size_t n)
{
	/*
	 * The base code has some hardcoded assumptions about the maximum
	 * number of requests that can be in progress. Maybe someday we'll
	 * do calculations with the actual number; for now, just make sure
	 * the assumption holds.
	 */
	return scan_int(buf,
			n,
			&default_max_requests_active,
			1,
			MAXIMUM_USER_VIOS);
}

/**********************************************************************/
static ssize_t
vdo_albireo_timeout_interval_store(struct kvdo_module_globals *kvdo_globals,
				   const char *buf,
				   size_t n)
{
	unsigned int value;
	ssize_t result = scan_uint(buf, n, &value, 0, UINT_MAX);

	if (result > 0) {
		set_albireo_timeout_interval(value);
	}
	return result;
}

/**********************************************************************/
static ssize_t
vdo_min_albireo_timer_interval_store(struct kvdo_module_globals *kvdo_globals,
				     const char *buf,
				     size_t n)
{
	unsigned int value;
	ssize_t result = scan_uint(buf, n, &value, 0, UINT_MAX);

	if (result > 0) {
		set_min_albireo_timer_interval(value);
	}
	return result;
}

/**********************************************************************/
static ssize_t vdo_version_show(struct kvdo_module_globals *kvdo_globals,
				struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%s\n", CURRENT_VERSION);
}

/**********************************************************************/
static ssize_t
vdo_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);
	if (vdo_attr->show == NULL) {
		return -EINVAL;
	}

	struct kvdo_module_globals *kvdo_globals;

	kvdo_globals = container_of(kobj, struct kvdo_module_globals, kobj);
	return (*vdo_attr->show)(kvdo_globals, attr, buf);
}

/**********************************************************************/
static ssize_t vdo_attr_store(struct kobject *kobj,
			      struct attribute *attr,
			      const char *buf,
			      size_t length)
{
	struct vdo_attribute *vdo_attr =
		container_of(attr, struct vdo_attribute, attr);
	if (vdo_attr->store == NULL) {
		return -EINVAL;
	}

	struct kvdo_module_globals *kvdo_globals;

	kvdo_globals = container_of(kobj, struct kvdo_module_globals, kobj);
	return (*vdo_attr->store)(kvdo_globals, buf, length);
}

static struct vdo_attribute vdo_status_attr = {
	.attr = {

			.name = "status",
			.mode = 0444,
		},
	.show = vdo_status_show,
};

static struct vdo_attribute vdo_log_level_attr = {
	.attr = {

			.name = "log_level",
			.mode = 0644,
		},
	.show = vdo_log_level_show,
	.store = vdo_log_level_store,
};

static struct vdo_attribute vdo_max_req_active_attr = {
	.attr = {

			.name = "max_requests_active",
			.mode = 0644,
		},
	.show = show_int,
	.store = vdo_max_req_active_store,
	.value_ptr = &default_max_requests_active,
};

static struct vdo_attribute vdo_albireo_timeout_interval = {
	.attr = {

			.name = "deduplication_timeout_interval",
			.mode = 0644,
		},
	.show = show_uint,
	.store = vdo_albireo_timeout_interval_store,
	.value_ptr = &albireo_timeout_interval,
};

static struct vdo_attribute vdo_min_albireo_timer_interval = {
	.attr = {

			.name = "min_deduplication_timer_interval",
			.mode = 0644,
		},
	.show = show_uint,
	.store = vdo_min_albireo_timer_interval_store,
	.value_ptr = &min_albireo_timer_interval,
};

static struct vdo_attribute vdo_trace_recording = {
	.attr = {

			.name = "trace_recording",
			.mode = 0644,
		},
	.show = show_bool,
	.store = vdo_trace_recording_store,
	.value_ptr = &trace_recording,
};

static struct vdo_attribute vdo_version_attr = {
	.attr = {

			.name = "version",
			.mode = 0444,
		},
	.show = vdo_version_show,
};

static struct attribute *defaultAttrs[] = {
	&vdo_status_attr.attr,
	&vdo_log_level_attr.attr,
	&vdo_max_req_active_attr.attr,
	&vdo_albireo_timeout_interval.attr,
	&vdo_min_albireo_timer_interval.attr,
	&vdo_trace_recording.attr,
	&vdo_version_attr.attr,
	NULL
};

static struct sysfs_ops vdo_sysfs_ops = {
	.show = vdo_attr_show,
	.store = vdo_attr_store,
};

/**********************************************************************/
static void vdo_release(struct kobject *kobj)
{
	return;
}

struct kobj_type vdo_ktype = {
	.release = vdo_release,
	.sysfs_ops = &vdo_sysfs_ops,
	.default_attrs = defaultAttrs,
};

/**********************************************************************/
int vdo_init_sysfs(struct kobject *module_object)
{
	kobject_init(module_object, &vdo_ktype);
	int result = kobject_add(module_object, NULL, THIS_MODULE->name);

	if (result < 0) {
		logError("kobject_add failed with status %d", -result);
		kobject_put(module_object);
	}
	logDebug("added sysfs objects");
	return result;
};

/**********************************************************************/
void vdo_put_sysfs(struct kobject *module_object)
{
	kobject_put(module_object);
}
