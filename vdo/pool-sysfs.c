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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/pool-sysfs.c#1 $
 */

#include "poolSysfs.h"

#include "memoryAlloc.h"

#include "vdo.h"

#include "dedupeIndex.h"

struct pool_attribute {
	struct attribute attr;
	ssize_t (*show)(struct vdo *vdo, char *buf);
	ssize_t (*store)(struct vdo *vdo, const char *value, size_t count);
};

/**********************************************************************/
static ssize_t vdo_pool_attr_show(struct kobject *directory,
				  struct attribute *attr,
				  char *buf)
{
	struct pool_attribute *pool_attr = container_of(attr,
							struct pool_attribute,
							attr);
	struct vdo *vdo = container_of(directory, struct vdo, vdo_directory);

	if (pool_attr->show == NULL) {
		return -EINVAL;
	}
	return pool_attr->show(vdo, buf);
}

/**********************************************************************/
static ssize_t vdo_pool_attr_store(struct kobject *directory,
				   struct attribute *attr,
				   const char *buf,
				   size_t length)
{
	struct pool_attribute *pool_attr = container_of(attr,
							struct pool_attribute,
							attr);
	struct vdo *vdo = container_of(directory, struct vdo, vdo_directory);

	if (pool_attr->store == NULL) {
		return -EINVAL;
	}
	return pool_attr->store(vdo, buf, length);
}

static struct sysfs_ops vdo_pool_sysfs_ops = {
	.show = vdo_pool_attr_show,
	.store = vdo_pool_attr_store,
};

/**********************************************************************/
static ssize_t pool_compressing_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%s\n",
		       (get_vdo_compressing(vdo) ? "1" : "0"));
}

/**********************************************************************/
static ssize_t pool_discards_active_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->discard_limiter.active);
}

/**********************************************************************/
static ssize_t pool_discards_limit_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->discard_limiter.limit);
}

/**********************************************************************/
static ssize_t pool_discards_limit_store(struct vdo *vdo,
					 const char *buf,
					 size_t length)
{
	unsigned int value;

	if ((length > 12) || (sscanf(buf, "%u", &value) != 1) || (value < 1)) {
		return -EINVAL;
	}
	vdo->discard_limiter.limit = value;
	return length;
}

/**********************************************************************/
static ssize_t pool_discards_maximum_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->discard_limiter.maximum);
}

/**********************************************************************/
static ssize_t pool_instance_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->instance);
}

/**********************************************************************/
static ssize_t pool_requests_active_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->request_limiter.active);
}

/**********************************************************************/
static ssize_t pool_requests_limit_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->request_limiter.limit);
}

/**********************************************************************/
static ssize_t pool_requests_maximum_show(struct vdo *vdo, char *buf)
{
	return sprintf(buf, "%u\n", vdo->request_limiter.maximum);
}

/**********************************************************************/
static void vdo_pool_release(struct kobject *directory)
{
	UDS_FREE(container_of(directory, struct vdo, vdo_directory));
}

static struct pool_attribute vdo_pool_compressing_attr = {
	.attr = {
			.name = "compressing",
			.mode = 0444,
		},
	.show = pool_compressing_show,
};

static struct pool_attribute vdo_pool_discards_active_attr = {
	.attr = {
			.name = "discards_active",
			.mode = 0444,
		},
	.show = pool_discards_active_show,
};

static struct pool_attribute vdo_pool_discards_limit_attr = {
	.attr = {
			.name = "discards_limit",
			.mode = 0644,
		},
	.show = pool_discards_limit_show,
	.store = pool_discards_limit_store,
};

static struct pool_attribute vdo_pool_discards_maximum_attr = {
	.attr = {
			.name = "discards_maximum",
			.mode = 0444,
		},
	.show = pool_discards_maximum_show,
};

static struct pool_attribute vdo_pool_instance_attr = {
	.attr = {
			.name = "instance",
			.mode = 0444,
		},
	.show = pool_instance_show,
};

static struct pool_attribute vdo_pool_requests_active_attr = {
	.attr = {
			.name = "requests_active",
			.mode = 0444,
		},
	.show = pool_requests_active_show,
};

static struct pool_attribute vdo_pool_requests_limit_attr = {
	.attr = {
			.name = "requests_limit",
			.mode = 0444,
		},
	.show = pool_requests_limit_show,
};

static struct pool_attribute vdo_pool_requests_maximum_attr = {
	.attr = {
			.name = "requests_maximum",
			.mode = 0444,
		},
	.show = pool_requests_maximum_show,
};

static struct attribute *pool_attrs[] = {
	&vdo_pool_compressing_attr.attr,
	&vdo_pool_discards_active_attr.attr,
	&vdo_pool_discards_limit_attr.attr,
	&vdo_pool_discards_maximum_attr.attr,
	&vdo_pool_instance_attr.attr,
	&vdo_pool_requests_active_attr.attr,
	&vdo_pool_requests_limit_attr.attr,
	&vdo_pool_requests_maximum_attr.attr,
	NULL,
};

struct kobj_type vdo_directory_type = {
	.release = vdo_pool_release,
	.sysfs_ops = &vdo_pool_sysfs_ops,
	.default_attrs = pool_attrs,
};
