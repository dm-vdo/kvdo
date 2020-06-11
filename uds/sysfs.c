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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/sysfs.c#4 $
 */

#include "sysfs.h"

#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"
#include "uds.h"

static struct {
	struct kobject kobj; // /sys/uds
	struct kobject parameter_kobj; // /sys/uds/parameter
	// These flags are used to ensure a clean shutdown
	bool flag; // /sys/uds
	bool parameter_flag; // /sys/uds/parameter
} object_root;

/**********************************************************************/
static char *buffer_to_string(const char *buf, size_t length)
{
	char *string;
	if (ALLOCATE(length + 1, char, __func__, &string) != UDS_SUCCESS) {
		return NULL;
	}
	memcpy(string, buf, length);
	string[length] = '\0';
	if (string[length - 1] == '\n') {
		string[length - 1] = '\0';
	}
	return string;
}

/**********************************************************************/
// This is the code for a directory in the /sys/<module_name> tree that
// contains no regular files (only subdirectories).
/**********************************************************************/

/**********************************************************************/
static void empty_release(struct kobject *kobj)
{
	// Many of our sysfs share this release function that does nothing.
}

/**********************************************************************/
static ssize_t
empty_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	return 0;
}

/**********************************************************************/
static ssize_t empty_store(struct kobject *kobj,
			   struct attribute *attr,
			   const char *buf,
			   size_t length)
{
	return length;
}

static struct sysfs_ops empty_ops = {
	.show = empty_show,
	.store = empty_store,
};

static struct attribute *empty_attrs[] = {
	NULL,
};

static struct kobj_type empty_object_type = {
	.release = empty_release,
	.sysfs_ops = &empty_ops,
	.default_attrs = empty_attrs,
};


/**********************************************************************/
// This is the the code for the /sys/<module_name>/parameter directory.
//
// <dir>/log_level                 UDS_LOG_LEVEL
//
/**********************************************************************/

struct parameter_attribute {
	struct attribute attr;
	const char *(*show_string)(void);
	void (*store_string)(const char *);
};

/**********************************************************************/
static ssize_t
parameter_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct parameter_attribute *pa =
		container_of(attr, struct parameter_attribute, attr);
	if (pa->show_string != NULL) {
		return sprintf(buf, "%s\n", pa->show_string());
	} else {
		return -EINVAL;
	}
}

/**********************************************************************/
static ssize_t parameter_store(struct kobject *kobj,
			       struct attribute *attr,
			       const char *buf,
			       size_t length)
{
	struct parameter_attribute *pa =
		container_of(attr, struct parameter_attribute, attr);
	if (pa->store_string == NULL) {
		return -EINVAL;
	}
	char *string = buffer_to_string(buf, length);
	if (string == NULL) {
		return -ENOMEM;
	}
	pa->store_string(string);
	FREE(string);
	return length;
}

/**********************************************************************/

static const char *parameter_show_log_level(void)
{
	return priorityToString(getLogLevel());
}

/**********************************************************************/

static void parameter_store_log_level(const char *string)
{
	setLogLevel(stringToPriority(string));
}

/**********************************************************************/

static struct parameter_attribute log_level_attr = {
	.attr = { .name = "log_level", .mode = 0600 },
	.show_string = parameter_show_log_level,
	.store_string = parameter_store_log_level,
};

static struct attribute *parameter_attrs[] = {
	&log_level_attr.attr,
	NULL,
};

static struct sysfs_ops parameter_ops = {
	.show = parameter_show,
	.store = parameter_store,
};

static struct kobj_type parameter_object_type = {
	.release = empty_release,
	.sysfs_ops = &parameter_ops,
	.default_attrs = parameter_attrs,
};

/**********************************************************************/
int init_sysfs(void)
{
	memset(&object_root, 0, sizeof(object_root));
	kobject_init(&object_root.kobj, &empty_object_type);
	int result = kobject_add(&object_root.kobj, NULL, THIS_MODULE->name);
	if (result == 0) {
		object_root.flag = true;
		kobject_init(&object_root.parameter_kobj,
			     &parameter_object_type);
		result = kobject_add(&object_root.parameter_kobj,
				     &object_root.kobj,
				     "parameter");
		if (result == 0) {
			object_root.parameter_flag = true;
		}
	}
	if (result != 0) {
		put_sysfs();
	}
	return result;
}

/**********************************************************************/
void put_sysfs()
{
	if (object_root.parameter_flag) {
		kobject_put(&object_root.parameter_kobj);
	}
	if (object_root.flag) {
		kobject_put(&object_root.kobj);
	}
}
