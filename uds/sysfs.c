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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/sysfs.c#1 $
 */

#include "sysfs.h"

#include <linux/kobject.h>
#include <linux/module.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "notificationDefs.h"
#include "stringUtils.h"
#include "uds.h"
#include "uds-param.h"

static struct {
  struct kobject kobj;               // /sys/uds
  struct kobject indexKobj;          // /sys/uds/index
  struct kobject parameterKobj;      // /sys/uds/parameter
  // This spinlock protects the rest of this struct, and the linked lists
  // pointed to by the *Head members.
  spinlock_t lock;
  struct indexObject         *indexHead;          // /sys/uds/index
  struct sessionName         *sessionHead;        // session-name table
  // These flags are used to ensure a clean shutdown
  bool flag;               // /sys/uds
  bool indexFlag;          // /sys/uds/index
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
// This is the code to handle index session notifications.  It maintains a
// table of session_id::name entries.
/**********************************************************************/

typedef struct sessionName {
  UdsIndexSession     indexSession;
  const char         *name;
  struct sessionName *next;
} SessionName;

/**********************************************************************/
SessionName **findSessionName(UdsIndexSession session)
{
  SessionName *sn, **psn;
  for (psn = &objectRoot.sessionHead; (sn = *psn) != NULL; psn = &sn->next) {
    if (sn->indexSession.id == session.id) {
      return psn;
    }
  }
  return NULL;
}

/**********************************************************************/
void notifyIndexClosed(UdsIndexSession session)
{
  SessionName *sn = NULL;
  spin_lock_irq(&objectRoot.lock);
  SessionName **psn = findSessionName(session);
  if (psn != NULL) {
    sn = *psn;
    *psn = sn->next;
  }
  spin_unlock_irq(&objectRoot.lock);
  if (sn != NULL) {
    freeConst(sn->name);
    FREE(sn);
  }
}

/**********************************************************************/
void notifyIndexOpened(UdsIndexSession session, const char *name)
{
  char *nameCopy;
  if (duplicateString(name, __func__, &nameCopy) != UDS_SUCCESS) {
    return;
  }
  SessionName *sn;
  if (ALLOCATE(1, SessionName, __func__, &sn) != UDS_SUCCESS) {
    FREE(nameCopy);
    return;
  }
  sn->indexSession = session;
  sn->name = nameCopy;
  spin_lock_irq(&objectRoot.lock);
  sn->next = objectRoot.sessionHead;
  objectRoot.sessionHead = sn;
  spin_unlock_irq(&objectRoot.lock);
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
// This is the the code for a /sys/configuration/<config_name> directory.
// This directory is no longer used, but the code is also used by the
// /sys/index/<session>/configuration directory.
/**********************************************************************/

typedef struct configurationObject {
  struct kobject   kobj;
  UdsConfiguration userConfig;
} ConfigurationObject;

typedef struct {
  struct attribute attr;
  ssize_t (*showStuff)(UdsConfiguration, char *);
  unsigned int (*showUint)(UdsConfiguration);
} ConfigurationAttribute;

/**********************************************************************/
static void configurationRelease(struct kobject *kobj)
{
  ConfigurationObject *co = container_of(kobj, ConfigurationObject, kobj);
  udsFreeConfiguration(co->userConfig);
  FREE(co);
}

/**********************************************************************/
static ssize_t configurationShow(struct kobject   *kobj,
                                 struct attribute *attr,
                                 char             *buf)
{
  ConfigurationAttribute *ca = container_of(attr, ConfigurationAttribute, attr);
  ConfigurationObject *co = container_of(kobj, ConfigurationObject, kobj);
  if (ca->showStuff != NULL) {
    return ca->showStuff(co->userConfig, buf);
  } else if (ca->showUint != NULL) {
    return sprintf(buf, "%u\n", ca->showUint(co->userConfig));
  } else {
    return -EINVAL;
  }
}

/**********************************************************************/
static unsigned int configurationShowCfreq(UdsConfiguration userConfig)
{
  return udsConfigurationGetCheckpointFrequency(userConfig) ? 1 : 0;
}

/**********************************************************************/
static ssize_t configurationShowMem(UdsConfiguration userConfig, char *buf)
{
  unsigned int mem = udsConfigurationGetMemory(userConfig);
  if (mem == UDS_MEMORY_CONFIG_256MB) {
    return sprintf(buf, "0.25\n");
  } else if (mem == UDS_MEMORY_CONFIG_512MB) {
    return sprintf(buf, "0.5\n");
  } else if (mem == UDS_MEMORY_CONFIG_768MB) {
    return sprintf(buf, "0.75\n");
  } else {
    return sprintf(buf, "%u\n", mem);
  }
}

/**********************************************************************/
static ssize_t configurationShowSize(UdsConfiguration userConfig, char *buf)
{
  uint64_t size;
  int result = udsComputeIndexSize(userConfig, 0, &size);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "error sizing index configuration");
    return result < UDS_ERROR_CODE_BASE ? -result : -EINVAL;
  }
  return sprintf(buf, "%" PRIu64 "\n", size);
}

/**********************************************************************/
static unsigned int configurationShowSparse(UdsConfiguration userConfig)
{
  return udsConfigurationGetSparse(userConfig) ? 1 : 0;
}

/**********************************************************************/
static ssize_t configurationStore(struct kobject   *kobj,
                                  struct attribute *attr,
                                  const char       *buf,
                                  size_t            length)
{
  return -EINVAL;
}

/**********************************************************************/
// This is the the code for a /sys/index/<session>/configuration directory.
// It is a read-only version of the general configuration directory.
//
// <dir>/checkpoint_frequency
//    R uses udsConfigurationGetCheckpointFrequency
//
// <dir>/mem
//    R uses udsConfigurationGetMemory
//
// <dir>/sparse
//    R uses udsConfigurationGetSparse
//
/**********************************************************************/

static ConfigurationAttribute cfreqRoAttr = {
  .attr     = { .name = "checkpoint_frequency", .mode = 0400 },
  .showUint = configurationShowCfreq,
};

static ConfigurationAttribute memRoAttr = {
  .attr      = { .name = "mem", .mode = 0400 },
  .showStuff = configurationShowMem,
};

static ConfigurationAttribute sizeRoAttr = {
  .attr      = { .name = "size", .mode = 0400 },
  .showStuff = configurationShowSize,
};

static ConfigurationAttribute sparseRoAttr = {
  .attr     = { .name = "sparse", .mode = 0400 },
  .showUint = configurationShowSparse,
};

static struct attribute *configurationRoAttrs[] = {
  &cfreqRoAttr.attr,
  &memRoAttr.attr,
  &sizeRoAttr.attr,
  &sparseRoAttr.attr,
  NULL,
};

static struct sysfs_ops configurationOps = {
  .show  = configurationShow,
  .store = configurationStore,
};

static struct kobj_type configurationRoObjectType = {
  .release       = configurationRelease,
  .sysfs_ops     = &configurationOps,
  .default_attrs = configurationRoAttrs,
};

/**********************************************************************/
// This is the code for a /sys/index/<session> directory.
//
// <dir>/checkpoints
//    R the number of checkpoints done in this index session
//
// <dir>/collisions
//    R the number of collisions recorded in the master index
//
// <dir>/configuration
//    Directory contains the index configuration
//
// <dir>/context
//    R block context number
//
// <dir>/deletions_found
//    R the number of delete calls that deleted an existing entry
//
// <dir>/deletions_not_found
//    R the number of delete calls that did nothing
//
// <dir>/disk_used
//    R an estimate of the index's size on disk
//
// <dir>/entries_discarded
//    R the number of chunk names discarded from the master index in this
//      index session
//
// <dir>/entries_indexed
//    R the number of chunk names recorded in the master index
//
// <dir>/fill
//    W fill the index
//
// <dir>/memory_used
//    R an estimate of the index's size in memory
//
// <dir>/name
//    R uses index name
//
// <dir>/posts_found
//    R the number of post calls that found an existing entry
//
// <dir>/posts_not_found
//    R the number of post calls that added an entry
//
// <dir>/queries_found
//    R the number of query calls that found an existing entry
//
// <dir>/queries_not_found
//    R the number of query calls that found no entry
//
// <dir>/updates_found
//    R the number of update calls that found an existing entry
//
// <dir>/updates_not_found
//    R the number of update calls that added an entry
/**********************************************************************/

typedef struct indexObject {
  struct kobject      kobj;
  struct kobject     *configurationKobj;
  UdsIndexSession     indexSession;
  UdsBlockContext     blockContext;
  const char         *name;
  struct indexObject *next;
} IndexObject;

typedef struct {
  struct attribute attr;
  uint64_t (*showContextStat)(UdsContextStats *);
  uint64_t (*showIndexStat)(UdsIndexStats *);
  const char *(*showString)(IndexObject *);
  unsigned int (*showUint)(IndexObject *);
  ssize_t (*storeAny)(IndexObject *);
} IndexAttribute;

/**********************************************************************/
static void indexRelease(struct kobject *kobj)
{
  IndexObject *io = container_of(kobj, IndexObject, kobj);
  freeConst(io->name);
  FREE(io);
}

/**********************************************************************/
static ssize_t indexShow(struct kobject   *kobj,
                         struct attribute *attr,
                         char             *buf)
{
  IndexAttribute *ia = container_of(attr, IndexAttribute, attr);
  IndexObject *io = container_of(kobj, IndexObject, kobj);
  if (ia->showContextStat != NULL) {
    UdsContextStats stats;
    int result = udsGetBlockContextStats(io->blockContext, &stats);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result,
                              "error getting block context stats for %s",
                              io->name);
      return result < UDS_ERROR_CODE_BASE ? -result : -EINVAL;
    }
    return sprintf(buf, "%" PRIu64 "\n", ia->showContextStat(&stats));
  } else if (ia->showIndexStat != NULL) {
    UdsIndexStats stats;
    int result = udsGetBlockContextIndexStats(io->blockContext, &stats);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "error getting index stats for %s",
                              io->name);
      return result < UDS_ERROR_CODE_BASE ? -result : -EINVAL;
    }
    return sprintf(buf, "%" PRIu64 "\n", ia->showIndexStat(&stats));
  } else if (ia->showString != NULL) {
    return sprintf(buf, "%s\n", ia->showString(io));
  } else if (ia->showUint != NULL) {
    return sprintf(buf, "%u\n", ia->showUint(io));
  } else {
    return -EINVAL;
  }
}

/**********************************************************************/
static uint64_t indexShowCheckpoints(UdsIndexStats *indexStats)
{
  return indexStats->checkpoints;
}

/**********************************************************************/
static uint64_t indexShowCollisions(UdsIndexStats *indexStats)
{
  return indexStats->collisions;
}

/**********************************************************************/
static unsigned int indexShowContext(IndexObject *io)
{
  return io->blockContext.id;
}

/**********************************************************************/
static uint64_t indexShowDeletionsFound(UdsContextStats *contextStats)
{
  return contextStats->deletionsFound;
}

/**********************************************************************/
static uint64_t indexShowDeletionsNotFound(UdsContextStats *contextStats)
{
  return contextStats->deletionsNotFound;
}

/**********************************************************************/
static uint64_t indexShowDiskUsed(UdsIndexStats *indexStats)
{
  return indexStats->diskUsed;
}

/**********************************************************************/
static uint64_t indexShowEntriesDiscarded(UdsIndexStats *indexStats)
{
  return indexStats->entriesDiscarded;
}

/**********************************************************************/
static uint64_t indexShowEntriesIndexed(UdsIndexStats *indexStats)
{
  return indexStats->entriesIndexed;
}

/**********************************************************************/
static uint64_t indexShowMemoryUsed(UdsIndexStats *indexStats)
{
  return indexStats->memoryUsed;
}

/**********************************************************************/
static const char *indexShowName(IndexObject *io)
{
  return io->name;
}

/**********************************************************************/
static uint64_t indexShowPostsFound(UdsContextStats *contextStats)
{
  return contextStats->postsFound;
}

/**********************************************************************/
static uint64_t indexShowPostsNotFound(UdsContextStats *contextStats)
{
  return contextStats->postsNotFound;
}

/**********************************************************************/
static uint64_t indexShowQueriesFound(UdsContextStats *contextStats)
{
  return contextStats->queriesFound;
}

/**********************************************************************/
static uint64_t indexShowQueriesNotFound(UdsContextStats *contextStats)
{
  return contextStats->queriesNotFound;
}

/**********************************************************************/
static uint64_t indexShowUpdatesFound(UdsContextStats *contextStats)
{
  return contextStats->updatesFound;
}

/**********************************************************************/
static uint64_t indexShowUpdatesNotFound(UdsContextStats *contextStats)
{
  return contextStats->updatesNotFound;
}

/**********************************************************************/
static ssize_t indexStore(struct kobject   *kobj,
                          struct attribute *attr,
                          const char       *buf,
                          size_t            length)
{
  IndexAttribute *ia = container_of(attr, IndexAttribute, attr);
  IndexObject *io = container_of(kobj, IndexObject, kobj);
  ssize_t result = -EINVAL;
  if (ia->storeAny != NULL) {
    result = ia->storeAny(io);
  }
  return result == 0 ? length : result;
}

/**********************************************************************/
static ssize_t indexStoreFill(IndexObject *io)
{
  int count = 128;
  unsigned long seed = __builtin_bswap64(jiffies);
  for (;;) {
    UdsIndexStats stats;
    int result = udsGetBlockContextIndexStats(io->blockContext, &stats);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "error getting index stats for %s",
                              io->name);
      return -EIO;
    }
    if (stats.entriesDiscarded > 0) {
      // We did a discard, so the index is now full
      int result = udsFlushBlockContext(io->blockContext);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "error flushing %s", io->name);
        return -EIO;
      }
      return 0;
    }
    for (int i = 0; i < count; i++) {
      UdsChunkName name = udsCalculateMurmur3ChunkName(&seed, sizeof(seed));
      seed += 1;
      result = udsPostBlockName(io->blockContext, NULL, &name, &name);
      if (result != UDS_SUCCESS) {
        logErrorWithStringError(result, "error posting to %s", io->name);
        return -EIO;
      }
    }
    count = 1024;
  }
}

/**********************************************************************/

static IndexAttribute checkpointsAttr = {
  .attr          = { .name = "checkpoints", .mode = 0400 },
  .showIndexStat = indexShowCheckpoints,
};

static IndexAttribute collisionsAttr = {
  .attr          = { .name = "collisions", .mode = 0400 },
  .showIndexStat = indexShowCollisions,
};

static IndexAttribute contextAttr = {
  .attr     = { .name = "context", .mode = 0400 },
  .showUint = indexShowContext,
};

static IndexAttribute deletionsFoundAttr = {
  .attr            = { .name = "deletions_found", .mode = 0400 },
  .showContextStat = indexShowDeletionsFound,
};

static IndexAttribute deletionsNotFoundAttr = {
  .attr            = { .name = "deletions_not_found", .mode = 0400 },
  .showContextStat = indexShowDeletionsNotFound,
};

static IndexAttribute diskUsedAttr = {
  .attr          = { .name = "disk_used", .mode = 0400 },
  .showIndexStat = indexShowDiskUsed,
};

static IndexAttribute entriesDiscardedAttr = {
  .attr          = { .name = "entries_discarded", .mode = 0400 },
  .showIndexStat = indexShowEntriesDiscarded,
};

static IndexAttribute fillAttr = {
  .attr     = { .name = "fill", .mode = 0200 },
  .storeAny = indexStoreFill,
};

static IndexAttribute entriesIndexedAttr = {
  .attr          = { .name = "entries_indexed", .mode = 0400 },
  .showIndexStat = indexShowEntriesIndexed,
};

static IndexAttribute memoryUsedAttr = {
  .attr          = { .name = "memory_used", .mode = 0400 },
  .showIndexStat = indexShowMemoryUsed,
};

static IndexAttribute nameAttr = {
  .attr       = { .name = "name", .mode = 0400 },
  .showString = indexShowName,
};

static IndexAttribute postsFoundAttr = {
  .attr            = { .name = "posts_found", .mode = 0400 },
  .showContextStat = indexShowPostsFound,
};

static IndexAttribute postsNotFoundAttr = {
  .attr            = { .name = "posts_not_found", .mode = 0400 },
  .showContextStat = indexShowPostsNotFound,
};

static IndexAttribute queriesFoundAttr = {
  .attr            = { .name = "queries_found", .mode = 0400 },
  .showContextStat = indexShowQueriesFound,
};

static IndexAttribute queriesNotFoundAttr = {
  .attr            = { .name = "queries_not_found", .mode = 0400 },
  .showContextStat = indexShowQueriesNotFound,
};

static IndexAttribute updatesFoundAttr = {
  .attr            = { .name = "updates_found", .mode = 0400 },
  .showContextStat = indexShowUpdatesFound,
};

static IndexAttribute updatesNotFoundAttr = {
  .attr            = { .name = "updates_not_found", .mode = 0400 },
  .showContextStat = indexShowUpdatesNotFound,
};

static struct attribute *indexAttrs[] = {
  &checkpointsAttr.attr,
  &collisionsAttr.attr,
  &contextAttr.attr,
  &deletionsFoundAttr.attr,
  &deletionsNotFoundAttr.attr,
  &diskUsedAttr.attr,
  &entriesDiscardedAttr.attr,
  &entriesIndexedAttr.attr,
  &fillAttr.attr,
  &memoryUsedAttr.attr,
  &nameAttr.attr,
  &postsFoundAttr.attr,
  &postsNotFoundAttr.attr,
  &queriesFoundAttr.attr,
  &queriesNotFoundAttr.attr,
  &updatesFoundAttr.attr,
  &updatesNotFoundAttr.attr,
  NULL,
};

static struct sysfs_ops indexOps = {
  .show  = indexShow,
  .store = indexStore,
};

static struct kobj_type indexObjectType = {
  .release       = indexRelease,
  .sysfs_ops     = &indexOps,
  .default_attrs = indexAttrs,
};

/**********************************************************************/
// This is the the code for the /sys/<module_name>/parameter directory.
//
// <dir>/log_level                 UDS_LOG_LEVEL
// <dir>/parallel_factor           UDS_PARALLEL_FACTOR
// <dir>/time_request_turnaround   UDS_TIME_REQUEST_TURNAROUND
// <dir>/volume_read_threads       UDS_VOLUME_READ_THREADS
//
/**********************************************************************/

typedef struct {
  struct attribute  attr;
  const char       *name;
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
    UdsParameterValue value;
    int result = udsGetParameter(pa->name, &value);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "error getting parameter %s", pa->name);
      return result < UDS_ERROR_CODE_BASE ? -result : -EINVAL;
    }
    switch (value.type) {
    case UDS_PARAM_TYPE_BOOL:
      return sprintf(buf, "%s\n", value.value.u_bool ? "true" : "false");
    case UDS_PARAM_TYPE_UNSIGNED_INT:
      return sprintf(buf, "%u\n", value.value.u_uint);
    case UDS_PARAM_TYPE_STRING:
      return sprintf(buf, "%s\n", value.value.u_string);
    default:
      return -EINVAL;
    }
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
    UdsParameterValue parameter = {
      .type  = UDS_PARAM_TYPE_STRING,
      .value = { .u_string = string },
    };
    result = udsSetParameter(pa->name, parameter);
    if (result != UDS_SUCCESS) {
      logErrorWithStringError(result, "error setting parameter %s to %s",
                              pa->name, string);
    }
    result = result < UDS_ERROR_CODE_BASE ? -result : -EINVAL;
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

static ParameterAttribute parallelFactorAttr = {
  .attr = { .name = "parallel_factor", .mode = 0600 },
  .name = "UDS_PARALLEL_FACTOR",
};

static ParameterAttribute timeRequestTurnaroundAttr = {
  .attr = { .name = "time_request_turnaround", .mode = 0600 },
  .name = "UDS_TIME_REQUEST_TURNAROUND",
};

static ParameterAttribute volumeReadThreadsAttr = {
  .attr = { .name = "volume_read_threads", .mode = 0600 },
  .name = "UDS_VOLUME_READ_THREADS",
};

static struct attribute *parameterAttrs[] = {
  &logLevelAttr.attr,
  &parallelFactorAttr.attr,
  &timeRequestTurnaroundAttr.attr,
  &volumeReadThreadsAttr.attr,
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
void notifyBlockContextClosed(UdsBlockContext context)
{
  IndexObject *io, **pio;
  spin_lock_irq(&objectRoot.lock);
  for (pio = &objectRoot.indexHead; (io = *pio) != NULL; pio = &io->next) {
    if (io->blockContext.id == context.id) {
      *pio = io->next;
      break;
    }
  }
  spin_unlock_irq(&objectRoot.lock);
  if (io != NULL) {
    kobject_put(io->configurationKobj);
    kobject_put(&io->kobj);
  }
}

/**********************************************************************/
void notifyBlockContextOpened(UdsIndexSession session, UdsBlockContext context)
{
  spin_lock_irq(&objectRoot.lock);
  SessionName *sn = *findSessionName(session);
  const char *namePtr = sn != NULL ? sn->name : "unknown name";
  spin_unlock_irq(&objectRoot.lock);
  char *name;
  if (duplicateString(namePtr, __func__, &name) != UDS_SUCCESS) {
    return;
  }
  IndexObject *io;
  if (ALLOCATE(1, IndexObject, __func__, &io) != UDS_SUCCESS) {
    FREE(name);
    return;
  }
  io->indexSession = session;
  io->blockContext = context;
  io->name = name;
  ConfigurationObject *co;
  if (ALLOCATE(1, ConfigurationObject, __func__, &co) != UDS_SUCCESS) {
    indexRelease(&io->kobj);
    return;
  }
  io->configurationKobj = &co->kobj;
  int result = udsGetBlockContextConfiguration(context, &co->userConfig);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "error getting index \"%s\" configuration",
                            name);
    FREE(co);
    indexRelease(&io->kobj);
    return;
  }
  kobject_init(&io->kobj, &indexObjectType);
  result = kobject_add(&io->kobj, &objectRoot.indexKobj, "%u", context.id);
  if (result != 0) {
    configurationRelease(&co->kobj);
    indexRelease(&io->kobj);
    return;
  }
  kobject_init(&co->kobj, &configurationRoObjectType);
  result = kobject_add(&co->kobj, &io->kobj, "configuration");
  if (result != 0) {
    configurationRelease(&co->kobj);
    kobject_put(&io->kobj);
    return;
  }
  spin_lock_irq(&objectRoot.lock);
  io->next = objectRoot.indexHead;
  objectRoot.indexHead = io;
  spin_unlock_irq(&objectRoot.lock);
}

/**********************************************************************/
int initSysfs(void)
{
  memset(&objectRoot, 0, sizeof(objectRoot));
  spin_lock_init(&objectRoot.lock);
  kobject_init(&objectRoot.kobj, &emptyObjectType);
  kobject_init(&objectRoot.indexKobj, &emptyObjectType);
  kobject_init(&objectRoot.parameterKobj, &parameterObjectType);
  int result = kobject_add(&objectRoot.kobj, NULL, THIS_MODULE->name);
  if (result == 0) {
    objectRoot.flag = true;
    result = kobject_add(&objectRoot.indexKobj, &objectRoot.kobj, "index");
  }
  if (result == 0) {
    objectRoot.indexFlag = true;
    result = kobject_add(&objectRoot.parameterKobj, &objectRoot.kobj,
                         "parameter");
  }
  if (result == 0) {
    objectRoot.parameterFlag = true;
  }
  if (result != 0) {
    putSysfs();
  }
  return result;
}

/**********************************************************************/
void putSysfs()
{
  spin_lock_irq(&objectRoot.lock);
  // Free indices
  while (objectRoot.indexHead != NULL) {
    IndexObject *io = objectRoot.indexHead;
    objectRoot.indexHead = io->next;
    io->next = NULL;
    spin_unlock_irq(&objectRoot.lock);
    kobject_put(io->configurationKobj);
    kobject_put(&io->kobj);
    spin_lock_irq(&objectRoot.lock);
  }
  spin_unlock_irq(&objectRoot.lock);
  // Then the directories can go
  if (objectRoot.indexFlag) {
    kobject_put(&objectRoot.indexKobj);
  }
  if (objectRoot.parameterFlag) {
    kobject_put(&objectRoot.parameterKobj);
  }
  if (objectRoot.flag) {
    kobject_put(&objectRoot.kobj);
  }
}
