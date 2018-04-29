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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/statusProcfs.c#1 $
 *
 * Proc filesystem interface to the old GET_DEDUPE_STATS and
 * GET_KERNEL_STATS ioctls, which can no longer be supported in 4.4
 * and later kernels. These files return the same data as the old
 * ioctls do, in order to require minimal changes to our (and
 * customers') utilties and test code.
 *
 * +--+-----  /proc/vdo           procfsRoot
 *    |
 *    +-+-----  vdo<n>            config->poolName
 *      |
 *      +-------  dedupe_stats    GET_DEDUPE_STATS ioctl
 *      +-------  kernel_stats    GET_KERNEL_STATS ioctl
 *
 */
#include "statusProcfs.h"

#include <linux/version.h>

#include "memoryAlloc.h"

#include "releaseVersions.h"
#include "statistics.h"
#include "vdo.h"

#include "dedupeIndex.h"
#include "ioSubmitter.h"
#include "kernelStatistics.h"
#include "logger.h"
#include "memoryUsage.h"
#include "threadDevice.h"
#include "vdoCommon.h"

static struct proc_dir_entry *procfsRoot = NULL;

/**********************************************************************/
static int statusDedupeShow(struct seq_file *m, void *v)
{
  KernelLayer *layer = (KernelLayer *) m->private;
  VDOStatistics *stats;
  size_t len = sizeof(VDOStatistics);
  RegisteredThread allocatingThread, instanceThread;
  registerAllocatingThread(&allocatingThread, NULL);
  registerThreadDevice(&instanceThread, layer);
  int result = ALLOCATE(1, VDOStatistics, __func__, &stats);
  if (result == VDO_SUCCESS) {
    getKVDOStatistics(&layer->kvdo, stats);
    seq_write(m, stats, len);
    FREE(stats);
  }
  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return result;
}

/**********************************************************************/
static int statusDedupeOpen(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
  return single_open(file, statusDedupeShow, PDE_DATA(inode));
#else
  return single_open(file, statusDedupeShow, PDE(inode)->data);
#endif
}

static const struct file_operations vdoProcfsDedupeOps = {
  .open = statusDedupeOpen,
  .read = seq_read,
  .llseek = seq_lseek,
  .release = single_release,
};

/**********************************************************************/
static void copyBioStat(BioStats *b, const AtomicBioStats *a)
{
  b->read    = atomic64_read(&a->read);
  b->write   = atomic64_read(&a->write);
  b->discard = atomic64_read(&a->discard);
  b->flush   = atomic64_read(&a->flush);
  b->fua     = atomic64_read(&a->fua);
}

/**********************************************************************/
static BioStats subtractBioStats(BioStats minuend, BioStats subtrahend)
{
  return (BioStats) {
    .read    = minuend.read - subtrahend.read,
    .write   = minuend.write - subtrahend.write,
    .discard = minuend.discard - subtrahend.discard,
    .flush   = minuend.flush - subtrahend.flush,
    .fua     = minuend.fua - subtrahend.fua,
  };
}

/**********************************************************************/
void getKernelStats(KernelLayer *layer, KernelStatistics *stats)
{
  stats->version        = STATISTICS_VERSION;
  stats->releaseVersion = CURRENT_RELEASE_VERSION_NUMBER;
  stats->instance       = layer->instance;
  getLimiterValuesAtomically(&layer->requestLimiter,
                             &stats->currentVIOsInProgress, &stats->maxVIOs);
  stats->dedupeAdviceValid = atomic64_read(&layer->dedupeAdviceValid);
  stats->dedupeAdviceStale = atomic64_read(&layer->dedupeAdviceStale);
  stats->dedupeAdviceTimeouts
    = getEventCount(&layer->albireoTimeoutReporter);
  stats->flushOut     = atomic64_read(&layer->flushOut);
  stats->logicalBlockSize = layer->logicalBlockSize;
  copyBioStat(&stats->biosIn, &layer->biosIn);
  copyBioStat(&stats->biosInPartial, &layer->biosInPartial);
  copyBioStat(&stats->biosOut, &layer->biosOut);
  copyBioStat(&stats->biosMeta, &layer->biosMeta);
  copyBioStat(&stats->biosJournal, &layer->biosJournal);
  copyBioStat(&stats->biosPageCache, &layer->biosPageCache);
  copyBioStat(&stats->biosOutCompleted, &layer->biosOutCompleted);
  copyBioStat(&stats->biosMetaCompleted, &layer->biosMetaCompleted);
  copyBioStat(&stats->biosJournalCompleted, &layer->biosJournalCompleted);
  copyBioStat(&stats->biosPageCacheCompleted,
              &layer->biosPageCacheCompleted);
  copyBioStat(&stats->biosAcknowledged, &layer->biosAcknowledged);
  copyBioStat(&stats->biosAcknowledgedPartial,
              &layer->biosAcknowledgedPartial);
  stats->biosInProgress = subtractBioStats(stats->biosIn,
                                           stats->biosAcknowledged);
  getBioWorkQueueReadCacheStats(layer->ioSubmitter, &stats->readCache);
  stats->memoryUsage = getMemoryUsage();
  getIndexStatistics(layer->dedupeIndex, &stats->index);
}

/**********************************************************************/
static int statusKernelShow(struct seq_file *m, void *v)
{
  KernelLayer *layer = (KernelLayer *) m->private;
  KernelStatistics *stats;
  size_t len = sizeof(KernelStatistics);
  RegisteredThread allocatingThread, instanceThread;
  registerAllocatingThread(&allocatingThread, NULL);
  registerThreadDevice(&instanceThread, layer);
  int result = ALLOCATE(1, KernelStatistics, __func__, &stats);
  if (result == VDO_SUCCESS) {
    getKernelStats(layer, stats);
    seq_write(m, stats, len);
    FREE(stats);
  }
  unregisterThreadDeviceID();
  unregisterAllocatingThread();
  return result;
}

/**********************************************************************/
static int statusKernelOpen(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
  return single_open(file, statusKernelShow, PDE_DATA(inode));
#else
  return single_open(file, statusKernelShow, PDE(inode)->data);
#endif
}

static const struct file_operations vdoProcfsKernelOps = {
  .open = statusKernelOpen,
  .read = seq_read,
  .llseek = seq_lseek,
  .release = single_release,
};

/**********************************************************************/
int vdoInitProcfs()
{
  const char *procfsName = getProcRoot();
  procfsRoot = proc_mkdir(procfsName, NULL);
  if (procfsRoot == NULL) {
    logWarning("Could not create proc filesystem root %s\n", procfsName);
    return -ENOMEM;
  }
  return VDO_SUCCESS;
}

/**********************************************************************/
void vdoDestroyProcfs()
{
  remove_proc_entry(getProcRoot(), NULL);
  procfsRoot = NULL;
}

/**********************************************************************/
int vdoCreateProcfsEntry(KernelLayer *layer, const char *name, void **private)
{
  int result = VDO_SUCCESS;

  if (procfsRoot != NULL) {
    struct proc_dir_entry *fsDir;
    fsDir = proc_mkdir(name, procfsRoot);
    if (fsDir == NULL) {
      result = -ENOMEM;
    } else {
      if (proc_create_data(getVDOStatisticsProcFile(), 0644, fsDir,
                           &vdoProcfsDedupeOps, layer) == NULL) {
        result = -ENOMEM;
      } else if (proc_create_data(getKernelStatisticsProcFile(), 0644, fsDir,
                                  &vdoProcfsKernelOps, layer) == NULL) {
        result = -ENOMEM;
      }
    }
    if (result < 0) {
      vdoDestroyProcfsEntry(name, fsDir);
    } else {
      *private = fsDir;
    }
  } else {
    logWarning("No proc filesystem root set, skipping %s\n", name);
  }
  return result;
}

/**********************************************************************/
void vdoDestroyProcfsEntry(const char *name, void *private)
{
  if (procfsRoot != NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0)
    remove_proc_subtree(name, procfsRoot);
#else
    struct proc_dir_entry *fsDir = (struct proc_dir_entry *) private;
    remove_proc_entry(getVDOStatisticsProcFile(), fsDir);
    remove_proc_entry(getKernelStatisticsProcFile(), fsDir);
    remove_proc_entry(name, procfsRoot);
#endif
  }
}
