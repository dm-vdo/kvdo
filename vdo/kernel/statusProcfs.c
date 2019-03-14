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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/statusProcfs.c#6 $
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

static struct proc_dir_entry *procfs_root = NULL;

/**********************************************************************/
static int status_dedupe_show(struct seq_file *m, void *v)
{
	KernelLayer *layer = (KernelLayer *)m->private;
	VDOStatistics *stats;
	size_t len = sizeof(VDOStatistics);
	RegisteredThread allocating_thread, instance_thread;
	registerAllocatingThread(&allocating_thread, NULL);
	registerThreadDevice(&instance_thread, layer);
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
static int status_dedupe_open(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
	return single_open(file, status_dedupe_show, PDE_DATA(inode));
#else
	return single_open(file, status_dedupe_show, PDE(inode)->data);
#endif
}

static const struct file_operations vdo_procfs_dedupe_ops = {
	.open = status_dedupe_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**********************************************************************/
static void copy_bio_stat(BioStats *b, const AtomicBioStats *a)
{
	b->read = atomic64_read(&a->read);
	b->write = atomic64_read(&a->write);
	b->discard = atomic64_read(&a->discard);
	b->flush = atomic64_read(&a->flush);
	b->fua = atomic64_read(&a->fua);
}

/**********************************************************************/
static BioStats subtract_bio_stats(BioStats minuend, BioStats subtrahend)
{
	return (BioStats){
		.read = minuend.read - subtrahend.read,
		.write = minuend.write - subtrahend.write,
		.discard = minuend.discard - subtrahend.discard,
		.flush = minuend.flush - subtrahend.flush,
		.fua = minuend.fua - subtrahend.fua,
	};
}

/**********************************************************************/
void get_kernel_stats(KernelLayer *layer, KernelStatistics *stats)
{
	stats->version = STATISTICS_VERSION;
	stats->releaseVersion = CURRENT_RELEASE_VERSION_NUMBER;
	stats->instance = layer->instance;
	get_limiter_values_atomically(&layer->requestLimiter,
				      &stats->currentVIOsInProgress,
				      &stats->maxVIOs);
	// albireoTimeoutReport gives the number of timeouts, and
	// dedupeContextBusy gives the number of queries not made because of
	// earlier timeouts.
	stats->dedupeAdviceTimeouts =
		(getDedupeTimeoutCount(layer->dedupeIndex) +
		 atomic64_read(&layer->dedupeContextBusy));
	stats->flushOut = atomic64_read(&layer->flushOut);
	stats->logicalBlockSize = layer->deviceConfig->logical_block_size;
	copy_bio_stat(&stats->biosIn, &layer->biosIn);
	copy_bio_stat(&stats->biosInPartial, &layer->biosInPartial);
	copy_bio_stat(&stats->biosOut, &layer->biosOut);
	copy_bio_stat(&stats->biosMeta, &layer->biosMeta);
	copy_bio_stat(&stats->biosJournal, &layer->biosJournal);
	copy_bio_stat(&stats->biosPageCache, &layer->biosPageCache);
	copy_bio_stat(&stats->biosOutCompleted, &layer->biosOutCompleted);
	copy_bio_stat(&stats->biosMetaCompleted, &layer->biosMetaCompleted);
	copy_bio_stat(&stats->biosJournalCompleted,
		      &layer->biosJournalCompleted);
	copy_bio_stat(&stats->biosPageCacheCompleted,
		      &layer->biosPageCacheCompleted);
	copy_bio_stat(&stats->biosAcknowledged, &layer->biosAcknowledged);
	copy_bio_stat(&stats->biosAcknowledgedPartial,
		      &layer->biosAcknowledgedPartial);
	stats->biosInProgress =
		subtract_bio_stats(stats->biosIn, stats->biosAcknowledged);
	stats->memoryUsage = get_memory_usage();
	getIndexStatistics(layer->dedupeIndex, &stats->index);
}

/**********************************************************************/
static int status_kernel_show(struct seq_file *m, void *v)
{
	KernelLayer *layer = (KernelLayer *)m->private;
	KernelStatistics *stats;
	size_t len = sizeof(KernelStatistics);
	RegisteredThread allocating_thread, instance_thread;
	registerAllocatingThread(&allocating_thread, NULL);
	registerThreadDevice(&instance_thread, layer);
	int result = ALLOCATE(1, KernelStatistics, __func__, &stats);
	if (result == VDO_SUCCESS) {
		get_kernel_stats(layer, stats);
		seq_write(m, stats, len);
		FREE(stats);
	}
	unregisterThreadDeviceID();
	unregisterAllocatingThread();
	return result;
}

/**********************************************************************/
static int status_kernel_open(struct inode *inode, struct file *file)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
	return single_open(file, status_kernel_show, PDE_DATA(inode));
#else
	return single_open(file, status_kernel_show, PDE(inode)->data);
#endif
}

static const struct file_operations vdo_procfs_kernel_ops = {
	.open = status_kernel_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

/**********************************************************************/
int vdo_init_procfs()
{
	const char *procfs_name = getProcRoot();
	procfs_root = proc_mkdir(procfs_name, NULL);
	if (procfs_root == NULL) {
		logWarning("Could not create proc filesystem root %s\n",
			   procfs_name);
		return -ENOMEM;
	}
	return VDO_SUCCESS;
}

/**********************************************************************/
void vdo_destroy_procfs()
{
	remove_proc_entry(getProcRoot(), NULL);
	procfs_root = NULL;
}

/**********************************************************************/
int vdo_create_procfs_entry(KernelLayer *layer,
			    const char *name,
			    void **private)
{
	int result = VDO_SUCCESS;

	if (procfs_root != NULL) {
		struct proc_dir_entry *fs_dir;
		fs_dir = proc_mkdir(name, procfs_root);
		if (fs_dir == NULL) {
			result = -ENOMEM;
		} else {
			if (proc_create_data(getVDOStatisticsProcFile(),
					     0644,
					     fs_dir,
					     &vdo_procfs_dedupe_ops,
					     layer) == NULL) {
				result = -ENOMEM;
			} else if (proc_create_data(getKernelStatisticsProcFile(),
						    0644,
						    fs_dir,
						    &vdo_procfs_kernel_ops,
						    layer) == NULL) {
				result = -ENOMEM;
			}
		}
		if (result < 0) {
			vdo_destroy_procfs_entry(name, fs_dir);
		} else {
			*private = fs_dir;
		}
	} else {
		logWarning("No proc filesystem root set, skipping %s\n", name);
	}
	return result;
}

/**********************************************************************/
void vdo_destroy_procfs_entry(const char *name, void *private)
{
	if (procfs_root != NULL) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 10, 0)
		remove_proc_subtree(name, procfs_root);
#else
		struct proc_dir_entry *fs_dir =
			(struct proc_dir_entry *)private;
		remove_proc_entry(getVDOStatisticsProcFile(), fs_dir);
		remove_proc_entry(getKernelStatisticsProcFile(), fs_dir);
		remove_proc_entry(name, procfs_root);
#endif
	}
}
