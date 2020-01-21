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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/sysfs.c#4 $
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
  struct kobject kobj;               // /sys/uds
  struct kobject parameterKobj;      // /sys/uds/parameter
  // These flags are used to ensure a clean shutdown
  bool flag;               // /sys/uds
  bool parameterFlag;      // /sys/uds/parameter
} objectRoot;

/**********************************************************************/
static char *bufferToString(const char *buf, size_t length)
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
static void emptyRelease(struct kobject *kobj)
{
  // Many of our sysfs share this release function that does nothing.
}

/**********************************************************************/
static ssize_t emptyShow(struct kobject   *kobj,
                         struct attribute *attr,
                         char             *buf)
{
  return 0;
}

/**********************************************************************/
static ssize_t emptyStore(struct kobject   *kobj,
                          struct attribute *attr,
                          const char       *buf,
                          size_t            length)
{
  return length;
}

static struct sysfs_ops emptyOps = {
  .show  = emptyShow,
  .store = emptyStore,
};

static struct attribute *emptyAttrs[] = {
  NULL,
};

static struct kobj_type emptyObjectType = {
  .release       = emptyRelease,
  .sysfs_ops     = &emptyOps,
  .default_attrs = emptyAttrs,
};


/**********************************************************************/
// This is the the code for the /sys/<module_name>/parameter directory.
//
// <dir>/log_level                 UDS_LOG_LEVEL
//
/**********************************************************************/

typedef struct {
  struct attribute  attr;
  const char *(*showString)(void);
  void (*storeString)(const char *);
} ParameterAttribute;

/**********************************************************************/
static ssize_t parameterShow(struct kobject   *kobj,
                             struct attribute *attr,
                             char             *buf)
{
  ParameterAttribute *pa = container_of(attr, ParameterAttribute, attr);
  if (pa->showString != NULL) {
    return sprintf(buf, "%s\n", pa->showString());
  } else {
    return -EINVAL;
  }
}

/**********************************************************************/
static ssize_t parameterStore(struct kobject   *kobj,
                              struct attribute *attr,
                              const char       *buf,
                              size_t            length)
{
  ParameterAttribute *pa = container_of(attr, ParameterAttribute, attr);
  char *string = bufferToString(buf, length);
  if (string == NULL) {
    return -ENOMEM;
  }
  int result = UDS_SUCCESS;
  if (pa->storeString != NULL) {
    pa->storeString(string);
  } else {
    return -EINVAL;
  }
  FREE(string);
  return result == UDS_SUCCESS ? length : result;
}

/**********************************************************************/

static const char *parameterShowLogLevel(void)
{
  return priorityToString(getLogLevel());
}

/**********************************************************************/

static void parameterStoreLogLevel(const char *string)
{
  setLogLevel(stringToPriority(string));
}

/**********************************************************************/

static ParameterAttribute logLevelAttr = {
  .attr        = { .name = "log_level", .mode = 0600 },
  .showString  = parameterShowLogLevel,
  .storeString = parameterStoreLogLevel,
};

static struct attribute *parameterAttrs[] = {
  &logLevelAttr.attr,
  NULL,
};

static struct sysfs_ops parameterOps = {
  .show  = parameterShow,
  .store = parameterStore,
};

static struct kobj_type parameterObjectType = {
  .release       = emptyRelease,
  .sysfs_ops     = &parameterOps,
  .default_attrs = parameterAttrs,
};

/**********************************************************************/
int initSysfs(void)
{
  memset(&objectRoot, 0, sizeof(objectRoot));
  kobject_init(&objectRoot.kobj, &emptyObjectType);
  int result = kobject_add(&objectRoot.kobj, NULL, THIS_MODULE->name);
  if (result == 0) {
    objectRoot.flag = true;
    kobject_init(&objectRoot.parameterKobj, &parameterObjectType);
    result = kobject_add(&objectRoot.parameterKobj, &objectRoot.kobj,
                         "parameter");
    if (result == 0) {
      objectRoot.parameterFlag = true;
    }
  }
  if (result != 0) {
    putSysfs();
  }
  return result;
}

/**********************************************************************/
void putSysfs()
{
  if (objectRoot.parameterFlag) {
    kobject_put(&objectRoot.parameterKobj);
  }
  if (objectRoot.flag) {
    kobject_put(&objectRoot.kobj);
  }
}
