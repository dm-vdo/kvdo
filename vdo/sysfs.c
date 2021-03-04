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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/sysfs.c#18 $
 */

#include "sysfs.h"

#include <linux/module.h>

#include "dedupeIndex.h"
#include "dmvdo.h"
#include "logger.h"

extern int default_max_requests_active;

struct vdo_attribute {
	struct attribute attr;
	ssize_t (*show)(struct vdo_module_globals *vdo_globals,
			struct attribute *attr,
			char *buf);
	ssize_t (*store)(struct vdo_module_globals *vdo_globals,
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
static ssize_t vdo_status_show(struct vdo_module_globals *vdo_globals,
			       struct attribute *attr,
			       char *buf)
{
	return sprintf(buf, "%s\n", status_strings[vdo_globals->status]);
}

/**********************************************************************/
static ssize_t vdo_log_level_show(struct vdo_module_globals *vdo_globals,
				  struct attribute *attr,
				  char *buf)
{
	return sprintf(buf, "%s\n", priority_to_string(get_log_level()));
}

/**********************************************************************/
static ssize_t vdo_log_level_store(struct vdo_module_globals *vdo_globals
				   __always_unused,
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
	set_log_level(string_to_priority(internal_buf));
	return n;
}

/**********************************************************************/
static ssize_t scan_int(const char *buf,
			size_t n,
			int *value_ptr,
			int minimum,
			int maximum)
{
	unsigned int value;
	if (n > 12) {
		return -EINVAL;
	}

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
static ssize_t show_int(struct vdo_module_globals *vdo_globals __always_unused,
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
	unsigned int value;
	if (n > 12) {
		return -EINVAL;
	}

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
static ssize_t show_uint(struct vdo_module_globals *vdo_globals
			 __always_unused,
			 struct attribute *attr,
			 char *buf)
{
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);

	return sprintf(buf, "%u\n", *(unsigned int *) vdo_attr->value_ptr);
}

/**********************************************************************/
static ssize_t
vdo_max_req_active_store(struct vdo_module_globals *vdo_globals
			  __always_unused,
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
vdo_dedupe_timeout_interval_store(struct vdo_module_globals *vdo_globals
				   __always_unused,
				   const char *buf,
				   size_t n)
{
	unsigned int value;
	ssize_t result = scan_uint(buf, n, &value, 0, UINT_MAX);

	if (result > 0) {
		set_dedupe_index_timeout_interval(value);
	}
	return result;
}

/**********************************************************************/
static ssize_t
vdo_min_dedupe_timer_interval_store(struct vdo_module_globals *vdo_globals
				     __always_unused,
				     const char *buf,
				     size_t n)
{
	unsigned int value;
	ssize_t result = scan_uint(buf, n, &value, 0, UINT_MAX);

	if (result > 0) {
		set_min_dedupe_index_timer_interval(value);
	}
	return result;
}

/**********************************************************************/
static ssize_t vdo_version_show(struct vdo_module_globals *vdo_globals
				__always_unused,
				struct attribute *attr,
				char *buf)
{
	return sprintf(buf, "%s\n", CURRENT_VERSION);
}

/**********************************************************************/
static ssize_t
vdo_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct vdo_module_globals *vdo_globals;
	struct vdo_attribute *vdo_attr = container_of(attr,
						      struct vdo_attribute,
						      attr);
	if (vdo_attr->show == NULL) {
		return -EINVAL;
	}

	vdo_globals = container_of(kobj, struct vdo_module_globals, kobj);
	return (*vdo_attr->show)(vdo_globals, attr, buf);
}

/**********************************************************************/
static ssize_t vdo_attr_store(struct kobject *kobj,
			      struct attribute *attr,
			      const char *buf,
			      size_t length)
{
	struct vdo_module_globals *vdo_globals;
	struct vdo_attribute *vdo_attr =
		container_of(attr, struct vdo_attribute, attr);
	if (vdo_attr->store == NULL) {
		return -EINVAL;
	}

	vdo_globals = container_of(kobj, struct vdo_module_globals, kobj);
	return (*vdo_attr->store)(vdo_globals, buf, length);
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

static struct vdo_attribute vdo_dedupe_timeout_interval = {
	.attr = {
			.name = "deduplication_timeout_interval",
			.mode = 0644,
		},
	.show = show_uint,
	.store = vdo_dedupe_timeout_interval_store,
	.value_ptr = &dedupe_index_timeout_interval,
};

static struct vdo_attribute vdo_min_dedupe_timer_interval = {
	.attr = {
			.name = "min_deduplication_timer_interval",
			.mode = 0644,
		},
	.show = show_uint,
	.store = vdo_min_dedupe_timer_interval_store,
	.value_ptr = &min_dedupe_index_timer_interval,
};

static struct vdo_attribute vdo_version_attr = {
	.attr = {
			.name = "version",
			.mode = 0444,
		},
	.show = vdo_version_show,
};

static struct attribute *default_attrs[] = {
	&vdo_status_attr.attr,
	&vdo_log_level_attr.attr,
	&vdo_max_req_active_attr.attr,
	&vdo_dedupe_timeout_interval.attr,
	&vdo_min_dedupe_timer_interval.attr,
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
	.default_attrs = default_attrs,
};

/**********************************************************************/
int vdo_init_sysfs(struct kobject *module_object)
{
	int result;
	kobject_init(module_object, &vdo_ktype);
	result = kobject_add(module_object, NULL, THIS_MODULE->name);

	if (result < 0) {
		uds_log_error("kobject_add failed with status %d", -result);
		kobject_put(module_object);
	}
	log_debug("added sysfs objects");
	return result;
};

/**********************************************************************/
void vdo_put_sysfs(struct kobject *module_object)
{
	kobject_put(module_object);
}
