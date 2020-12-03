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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/poolSysfs.c#11 $
 */

#include "poolSysfs.h"

#include "memoryAlloc.h"

#include "vdo.h"

#include "dedupeIndex.h"

struct pool_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kernel_layer *layer, char *buf);
	ssize_t (*store)(struct kernel_layer *layer, const char *value, size_t count);
};

/**********************************************************************/
static ssize_t vdo_pool_attr_show(struct kobject *kobj,
				  struct attribute *attr,
				  char *buf)
{
	struct pool_attribute *pool_attr = container_of(attr,
							struct pool_attribute,
							attr);
	if (pool_attr->show == NULL) {
		return -EINVAL;
	}
	struct kernel_layer *layer = container_of(kobj,
						  struct kernel_layer,
						  kobj);
	return pool_attr->show(layer, buf);
}

/**********************************************************************/
static ssize_t vdo_pool_attr_store(struct kobject *kobj,
				   struct attribute *attr,
				   const char *buf,
				   size_t length)
{
	struct pool_attribute *pool_attr = container_of(attr,
							struct pool_attribute,
							attr);
	if (pool_attr->store == NULL) {
		return -EINVAL;
	}
	struct kernel_layer *layer = container_of(kobj,
						  struct kernel_layer,
						  kobj);
	return pool_attr->store(layer, buf, length);
}

static struct sysfs_ops vdo_pool_sysfs_ops = {
	.show = vdo_pool_attr_show,
	.store = vdo_pool_attr_store,
};

/**********************************************************************/
static ssize_t pool_compressing_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%s\n",
		       (get_kvdo_compressing(&layer->kvdo) ? "1" : "0"));
}

/**********************************************************************/
static ssize_t pool_discards_active_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->discard_limiter.active);
}

/**********************************************************************/
static ssize_t pool_discards_limit_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->discard_limiter.limit);
}

/**********************************************************************/
static ssize_t pool_discards_limit_store(struct kernel_layer *layer,
					 const char *buf,
					 size_t length)
{
	unsigned int value;

	if ((length > 12) || (sscanf(buf, "%u", &value) != 1) || (value < 1)) {
		return -EINVAL;
	}
	layer->discard_limiter.limit = value;
	return length;
}

/**********************************************************************/
static ssize_t pool_discards_maximum_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->discard_limiter.maximum);
}

/**********************************************************************/
static ssize_t pool_instance_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->instance);
}

/**********************************************************************/
static ssize_t pool_requests_active_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->request_limiter.active);
}

/**********************************************************************/
static ssize_t pool_requests_limit_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->request_limiter.limit);
}

/**********************************************************************/
static ssize_t pool_requests_maximum_show(struct kernel_layer *layer, char *buf)
{
	return sprintf(buf, "%u\n", layer->request_limiter.maximum);
}

/**********************************************************************/
static void vdo_pool_release(struct kobject *kobj)
{
	struct kernel_layer *layer = container_of(kobj,
						  struct kernel_layer,
						  kobj);
	free_vdo(&layer->kvdo.vdo);
	FREE(layer);
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

struct kobj_type kernel_layer_kobj_type = {
	.release = vdo_pool_release,
	.sysfs_ops = &vdo_pool_sysfs_ops,
	.default_attrs = pool_attrs,
};

/**********************************************************************/
static void work_queue_directory_release(struct kobject *kobj)
{
	/*
	 * The work_queue_directory holds an implicit reference to its parent,
	 * the kernel_layer object (->kobj), so even if there are some
	 * external references held to the work_queue_directory when work
	 * queue shutdown calls kobject_put on the kernel_layer object, the
	 * kernel_layer object won't actually be released and won't free the
	 * kernel_layer storage until the work_queue_directory object is
	 * released first.
	 *
	 * So, we don't need to do any additional explicit management here.
	 *
	 * (But we aren't allowed to use a NULL function pointer to indicate
	 * a no-op.)
	 */
}

/**********************************************************************/
static struct attribute *no_attrs[] = {
	NULL,
};

static struct sysfs_ops no_sysfs_ops = {
	// These should never be reachable since there are no attributes.
	.show = NULL,
	.store = NULL,
};

struct kobj_type work_queue_directory_kobj_type = {
	.release = work_queue_directory_release,
	.sysfs_ops = &no_sysfs_ops,
	.default_attrs = no_attrs,
};
