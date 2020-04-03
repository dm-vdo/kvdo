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
 */

#include "commonStats.h"
#include "dedupeIndex.h"
#include "logger.h"
#include "poolSysfs.h"
#include "statistics.h"
#include "threadDevice.h"
#include "vdo.h"

struct pool_stats_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kernel_layer *layer, char *buf);
};

static ssize_t pool_stats_attr_show(struct kobject *kobj,
				    struct attribute *attr,
				    char *buf)
{
	struct pool_stats_attribute *pool_stats_attr = container_of(attr,
								    struct pool_stats_attribute,
								    attr);

	if (pool_stats_attr->show == NULL) {
		return -EINVAL;
	}
	struct kernel_layer *layer = container_of(kobj,
						  struct kernel_layer,
						  statsDirectory);
	return pool_stats_attr->show(layer, buf);
}

struct sysfs_ops pool_stats_sysfs_ops = {
	.show = pool_stats_attr_show,
	.store = NULL,
};

/**********************************************************************/
/** Number of blocks used for data */
static ssize_t pool_stats_data_blocks_used_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.data_blocks_used);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_data_blocks_used_attr = {
	.attr  = { .name = "data_blocks_used", .mode = 0444, },
	.show  = pool_stats_data_blocks_used_show,
};

/**********************************************************************/
/** Number of blocks used for VDO metadata */
static ssize_t pool_stats_overhead_blocks_used_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.overhead_blocks_used);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_overhead_blocks_used_attr = {
	.attr  = { .name = "overhead_blocks_used", .mode = 0444, },
	.show  = pool_stats_overhead_blocks_used_show,
};

/**********************************************************************/
/** Number of logical blocks that are currently mapped to physical blocks */
static ssize_t pool_stats_logical_blocks_used_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.logical_blocks_used);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_logical_blocks_used_attr = {
	.attr  = { .name = "logical_blocks_used", .mode = 0444, },
	.show  = pool_stats_logical_blocks_used_show,
};

/**********************************************************************/
/** number of physical blocks */
static ssize_t pool_stats_physical_blocks_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.physical_blocks);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_physical_blocks_attr = {
	.attr  = { .name = "physical_blocks", .mode = 0444, },
	.show  = pool_stats_physical_blocks_show,
};

/**********************************************************************/
/** number of logical blocks */
static ssize_t pool_stats_logical_blocks_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.logical_blocks);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_logical_blocks_attr = {
	.attr  = { .name = "logical_blocks", .mode = 0444, },
	.show  = pool_stats_logical_blocks_show,
};

/**********************************************************************/
/** Size of the block map page cache, in bytes */
static ssize_t pool_stats_block_map_cache_size_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map_cache_size);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_cache_size_attr = {
	.attr  = { .name = "block_map_cache_size", .mode = 0444, },
	.show  = pool_stats_block_map_cache_size_show,
};

/**********************************************************************/
/** String describing the active write policy of the VDO */
static ssize_t pool_stats_write_policy_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%s\n", layer->vdo_stats_storage.write_policy);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_write_policy_attr = {
	.attr  = { .name = "write_policy", .mode = 0444, },
	.show  = pool_stats_write_policy_show,
};

/**********************************************************************/
/** The physical block size */
static ssize_t pool_stats_block_size_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_size);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_size_attr = {
	.attr  = { .name = "block_size", .mode = 0444, },
	.show  = pool_stats_block_size_show,
};

/**********************************************************************/
/** Number of times the VDO has successfully recovered */
static ssize_t pool_stats_complete_recoveries_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.complete_recoveries);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_complete_recoveries_attr = {
	.attr  = { .name = "complete_recoveries", .mode = 0444, },
	.show  = pool_stats_complete_recoveries_show,
};

/**********************************************************************/
/** Number of times the VDO has recovered from read-only mode */
static ssize_t pool_stats_read_only_recoveries_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.read_only_recoveries);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_read_only_recoveries_attr = {
	.attr  = { .name = "read_only_recoveries", .mode = 0444, },
	.show  = pool_stats_read_only_recoveries_show,
};

/**********************************************************************/
/** String describing the operating mode of the VDO */
static ssize_t pool_stats_mode_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%s\n", layer->vdo_stats_storage.mode);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_mode_attr = {
	.attr  = { .name = "mode", .mode = 0444, },
	.show  = pool_stats_mode_show,
};

/**********************************************************************/
/** Whether the VDO is in recovery mode */
static ssize_t pool_stats_in_recovery_mode_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%d\n", layer->vdo_stats_storage.in_recovery_mode);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_in_recovery_mode_attr = {
	.attr  = { .name = "in_recovery_mode", .mode = 0444, },
	.show  = pool_stats_in_recovery_mode_show,
};

/**********************************************************************/
/** What percentage of recovery mode work has been completed */
static ssize_t pool_stats_recovery_percentage_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%u\n", layer->vdo_stats_storage.recovery_percentage);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_recovery_percentage_attr = {
	.attr  = { .name = "recovery_percentage", .mode = 0444, },
	.show  = pool_stats_recovery_percentage_show,
};

/**********************************************************************/
/** Number of compressed data items written since startup */
static ssize_t pool_stats_packer_compressed_fragments_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.packer.compressed_fragments_written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_packer_compressed_fragments_written_attr = {
	.attr  = { .name = "packer_compressed_fragments_written", .mode = 0444, },
	.show  = pool_stats_packer_compressed_fragments_written_show,
};

/**********************************************************************/
/** Number of blocks containing compressed items written since startup */
static ssize_t pool_stats_packer_compressed_blocks_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.packer.compressed_blocks_written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_packer_compressed_blocks_written_attr = {
	.attr  = { .name = "packer_compressed_blocks_written", .mode = 0444, },
	.show  = pool_stats_packer_compressed_blocks_written_show,
};

/**********************************************************************/
/** Number of VIOs that are pending in the packer */
static ssize_t pool_stats_packer_compressed_fragments_in_packer_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.packer.compressed_fragments_in_packer);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_packer_compressed_fragments_in_packer_attr = {
	.attr  = { .name = "packer_compressed_fragments_in_packer", .mode = 0444, },
	.show  = pool_stats_packer_compressed_fragments_in_packer_show,
};

/**********************************************************************/
/** The total number of slabs from which blocks may be allocated */
static ssize_t pool_stats_allocator_slab_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.allocator.slabCount);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_allocator_slab_count_attr = {
	.attr  = { .name = "allocator_slab_count", .mode = 0444, },
	.show  = pool_stats_allocator_slab_count_show,
};

/**********************************************************************/
/** The total number of slabs from which blocks have ever been allocated */
static ssize_t pool_stats_allocator_slabs_opened_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.allocator.slabsOpened);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_allocator_slabs_opened_attr = {
	.attr  = { .name = "allocator_slabs_opened", .mode = 0444, },
	.show  = pool_stats_allocator_slabs_opened_show,
};

/**********************************************************************/
/** The number of times since loading that a slab has been re-opened */
static ssize_t pool_stats_allocator_slabs_reopened_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.allocator.slabsReopened);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_allocator_slabs_reopened_attr = {
	.attr  = { .name = "allocator_slabs_reopened", .mode = 0444, },
	.show  = pool_stats_allocator_slabs_reopened_show,
};

/**********************************************************************/
/** Number of times the on-disk journal was full */
static ssize_t pool_stats_journal_disk_full_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.disk_full);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_disk_full_attr = {
	.attr  = { .name = "journal_disk_full", .mode = 0444, },
	.show  = pool_stats_journal_disk_full_show,
};

/**********************************************************************/
/** Number of times the recovery journal requested slab journal commits. */
static ssize_t pool_stats_journal_slab_journal_commits_requested_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.slab_journal_commits_requested);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_slab_journal_commits_requested_attr = {
	.attr  = { .name = "journal_slab_journal_commits_requested", .mode = 0444, },
	.show  = pool_stats_journal_slab_journal_commits_requested_show,
};

/**********************************************************************/
/** The total number of items on which processing has started */
static ssize_t pool_stats_journal_entries_started_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.entries.started);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_entries_started_attr = {
	.attr  = { .name = "journal_entries_started", .mode = 0444, },
	.show  = pool_stats_journal_entries_started_show,
};

/**********************************************************************/
/** The total number of items for which a write operation has been issued */
static ssize_t pool_stats_journal_entries_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.entries.written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_entries_written_attr = {
	.attr  = { .name = "journal_entries_written", .mode = 0444, },
	.show  = pool_stats_journal_entries_written_show,
};

/**********************************************************************/
/** The total number of items for which a write operation has completed */
static ssize_t pool_stats_journal_entries_committed_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.entries.committed);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_entries_committed_attr = {
	.attr  = { .name = "journal_entries_committed", .mode = 0444, },
	.show  = pool_stats_journal_entries_committed_show,
};

/**********************************************************************/
/** The total number of items on which processing has started */
static ssize_t pool_stats_journal_blocks_started_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.blocks.started);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_blocks_started_attr = {
	.attr  = { .name = "journal_blocks_started", .mode = 0444, },
	.show  = pool_stats_journal_blocks_started_show,
};

/**********************************************************************/
/** The total number of items for which a write operation has been issued */
static ssize_t pool_stats_journal_blocks_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.blocks.written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_blocks_written_attr = {
	.attr  = { .name = "journal_blocks_written", .mode = 0444, },
	.show  = pool_stats_journal_blocks_written_show,
};

/**********************************************************************/
/** The total number of items for which a write operation has completed */
static ssize_t pool_stats_journal_blocks_committed_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.journal.blocks.committed);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_journal_blocks_committed_attr = {
	.attr  = { .name = "journal_blocks_committed", .mode = 0444, },
	.show  = pool_stats_journal_blocks_committed_show,
};

/**********************************************************************/
/** Number of times the on-disk journal was full */
static ssize_t pool_stats_slab_journal_disk_full_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_journal.disk_full_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_journal_disk_full_count_attr = {
	.attr  = { .name = "slab_journal_disk_full_count", .mode = 0444, },
	.show  = pool_stats_slab_journal_disk_full_count_show,
};

/**********************************************************************/
/** Number of times an entry was added over the flush threshold */
static ssize_t pool_stats_slab_journal_flush_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_journal.flush_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_journal_flush_count_attr = {
	.attr  = { .name = "slab_journal_flush_count", .mode = 0444, },
	.show  = pool_stats_slab_journal_flush_count_show,
};

/**********************************************************************/
/** Number of times an entry was added over the block threshold */
static ssize_t pool_stats_slab_journal_blocked_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_journal.blocked_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_journal_blocked_count_attr = {
	.attr  = { .name = "slab_journal_blocked_count", .mode = 0444, },
	.show  = pool_stats_slab_journal_blocked_count_show,
};

/**********************************************************************/
/** Number of times a tail block was written */
static ssize_t pool_stats_slab_journal_blocks_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_journal.blocks_written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_journal_blocks_written_attr = {
	.attr  = { .name = "slab_journal_blocks_written", .mode = 0444, },
	.show  = pool_stats_slab_journal_blocks_written_show,
};

/**********************************************************************/
/** Number of times we had to wait for the tail to write */
static ssize_t pool_stats_slab_journal_tail_busy_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_journal.tail_busy_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_journal_tail_busy_count_attr = {
	.attr  = { .name = "slab_journal_tail_busy_count", .mode = 0444, },
	.show  = pool_stats_slab_journal_tail_busy_count_show,
};

/**********************************************************************/
/** Number of blocks written */
static ssize_t pool_stats_slab_summary_blocks_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.slab_summary.blocks_written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_slab_summary_blocks_written_attr = {
	.attr  = { .name = "slab_summary_blocks_written", .mode = 0444, },
	.show  = pool_stats_slab_summary_blocks_written_show,
};

/**********************************************************************/
/** Number of reference blocks written */
static ssize_t pool_stats_ref_counts_blocks_written_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.ref_counts.blocks_written);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_ref_counts_blocks_written_attr = {
	.attr  = { .name = "ref_counts_blocks_written", .mode = 0444, },
	.show  = pool_stats_ref_counts_blocks_written_show,
};

/**********************************************************************/
/** number of dirty (resident) pages */
static ssize_t pool_stats_block_map_dirty_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.dirty_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_dirty_pages_attr = {
	.attr  = { .name = "block_map_dirty_pages", .mode = 0444, },
	.show  = pool_stats_block_map_dirty_pages_show,
};

/**********************************************************************/
/** number of clean (resident) pages */
static ssize_t pool_stats_block_map_clean_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.clean_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_clean_pages_attr = {
	.attr  = { .name = "block_map_clean_pages", .mode = 0444, },
	.show  = pool_stats_block_map_clean_pages_show,
};

/**********************************************************************/
/** number of free pages */
static ssize_t pool_stats_block_map_free_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.free_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_free_pages_attr = {
	.attr  = { .name = "block_map_free_pages", .mode = 0444, },
	.show  = pool_stats_block_map_free_pages_show,
};

/**********************************************************************/
/** number of pages in failed state */
static ssize_t pool_stats_block_map_failed_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.failed_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_failed_pages_attr = {
	.attr  = { .name = "block_map_failed_pages", .mode = 0444, },
	.show  = pool_stats_block_map_failed_pages_show,
};

/**********************************************************************/
/** number of pages incoming */
static ssize_t pool_stats_block_map_incoming_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.incoming_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_incoming_pages_attr = {
	.attr  = { .name = "block_map_incoming_pages", .mode = 0444, },
	.show  = pool_stats_block_map_incoming_pages_show,
};

/**********************************************************************/
/** number of pages outgoing */
static ssize_t pool_stats_block_map_outgoing_pages_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.outgoing_pages);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_outgoing_pages_attr = {
	.attr  = { .name = "block_map_outgoing_pages", .mode = 0444, },
	.show  = pool_stats_block_map_outgoing_pages_show,
};

/**********************************************************************/
/** how many times free page not avail */
static ssize_t pool_stats_block_map_cache_pressure_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->vdo_stats_storage.block_map.cache_pressure);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_cache_pressure_attr = {
	.attr  = { .name = "block_map_cache_pressure", .mode = 0444, },
	.show  = pool_stats_block_map_cache_pressure_show,
};

/**********************************************************************/
/** number of getVDOPageAsync() for read */
static ssize_t pool_stats_block_map_read_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.read_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_read_count_attr = {
	.attr  = { .name = "block_map_read_count", .mode = 0444, },
	.show  = pool_stats_block_map_read_count_show,
};

/**********************************************************************/
/** number or getVDOPageAsync() for write */
static ssize_t pool_stats_block_map_write_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.write_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_write_count_attr = {
	.attr  = { .name = "block_map_write_count", .mode = 0444, },
	.show  = pool_stats_block_map_write_count_show,
};

/**********************************************************************/
/** number of times pages failed to read */
static ssize_t pool_stats_block_map_failed_reads_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.failed_reads);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_failed_reads_attr = {
	.attr  = { .name = "block_map_failed_reads", .mode = 0444, },
	.show  = pool_stats_block_map_failed_reads_show,
};

/**********************************************************************/
/** number of times pages failed to write */
static ssize_t pool_stats_block_map_failed_writes_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.failed_writes);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_failed_writes_attr = {
	.attr  = { .name = "block_map_failed_writes", .mode = 0444, },
	.show  = pool_stats_block_map_failed_writes_show,
};

/**********************************************************************/
/** number of gets that are reclaimed */
static ssize_t pool_stats_block_map_reclaimed_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.reclaimed);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_reclaimed_attr = {
	.attr  = { .name = "block_map_reclaimed", .mode = 0444, },
	.show  = pool_stats_block_map_reclaimed_show,
};

/**********************************************************************/
/** number of gets for outgoing pages */
static ssize_t pool_stats_block_map_read_outgoing_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.read_outgoing);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_read_outgoing_attr = {
	.attr  = { .name = "block_map_read_outgoing", .mode = 0444, },
	.show  = pool_stats_block_map_read_outgoing_show,
};

/**********************************************************************/
/** number of gets that were already there */
static ssize_t pool_stats_block_map_found_in_cache_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.found_in_cache);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_found_in_cache_attr = {
	.attr  = { .name = "block_map_found_in_cache", .mode = 0444, },
	.show  = pool_stats_block_map_found_in_cache_show,
};

/**********************************************************************/
/** number of gets requiring discard */
static ssize_t pool_stats_block_map_discard_required_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.discard_required);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_discard_required_attr = {
	.attr  = { .name = "block_map_discard_required", .mode = 0444, },
	.show  = pool_stats_block_map_discard_required_show,
};

/**********************************************************************/
/** number of gets enqueued for their page */
static ssize_t pool_stats_block_map_wait_for_page_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.wait_for_page);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_wait_for_page_attr = {
	.attr  = { .name = "block_map_wait_for_page", .mode = 0444, },
	.show  = pool_stats_block_map_wait_for_page_show,
};

/**********************************************************************/
/** number of gets that have to fetch */
static ssize_t pool_stats_block_map_fetch_required_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.fetch_required);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_fetch_required_attr = {
	.attr  = { .name = "block_map_fetch_required", .mode = 0444, },
	.show  = pool_stats_block_map_fetch_required_show,
};

/**********************************************************************/
/** number of page fetches */
static ssize_t pool_stats_block_map_pages_loaded_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.pages_loaded);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_pages_loaded_attr = {
	.attr  = { .name = "block_map_pages_loaded", .mode = 0444, },
	.show  = pool_stats_block_map_pages_loaded_show,
};

/**********************************************************************/
/** number of page saves */
static ssize_t pool_stats_block_map_pages_saved_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.pages_saved);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_pages_saved_attr = {
	.attr  = { .name = "block_map_pages_saved", .mode = 0444, },
	.show  = pool_stats_block_map_pages_saved_show,
};

/**********************************************************************/
/** the number of flushes issued */
static ssize_t pool_stats_block_map_flush_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.block_map.flush_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_block_map_flush_count_attr = {
	.attr  = { .name = "block_map_flush_count", .mode = 0444, },
	.show  = pool_stats_block_map_flush_count_show,
};

/**********************************************************************/
/** Number of times the UDS advice proved correct */
static ssize_t pool_stats_hash_lock_dedupe_advice_valid_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.hash_lock.dedupe_advice_valid);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_hash_lock_dedupe_advice_valid_attr = {
	.attr  = { .name = "hash_lock_dedupe_advice_valid", .mode = 0444, },
	.show  = pool_stats_hash_lock_dedupe_advice_valid_show,
};

/**********************************************************************/
/** Number of times the UDS advice proved incorrect */
static ssize_t pool_stats_hash_lock_dedupe_advice_stale_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.hash_lock.dedupe_advice_stale);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_hash_lock_dedupe_advice_stale_attr = {
	.attr  = { .name = "hash_lock_dedupe_advice_stale", .mode = 0444, },
	.show  = pool_stats_hash_lock_dedupe_advice_stale_show,
};

/**********************************************************************/
/** Number of writes with the same data as another in-flight write */
static ssize_t pool_stats_hash_lock_concurrent_data_matches_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.hash_lock.concurrent_data_matches);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_hash_lock_concurrent_data_matches_attr = {
	.attr  = { .name = "hash_lock_concurrent_data_matches", .mode = 0444, },
	.show  = pool_stats_hash_lock_concurrent_data_matches_show,
};

/**********************************************************************/
/** Number of writes whose hash collided with an in-flight write */
static ssize_t pool_stats_hash_lock_concurrent_hash_collisions_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.hash_lock.concurrent_hash_collisions);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_hash_lock_concurrent_hash_collisions_attr = {
	.attr  = { .name = "hash_lock_concurrent_hash_collisions", .mode = 0444, },
	.show  = pool_stats_hash_lock_concurrent_hash_collisions_show,
};

/**********************************************************************/
/** number of times VDO got an invalid dedupe advice PBN from UDS */
static ssize_t pool_stats_errors_invalid_advicePBNCount_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.errors.invalid_advice_pbn_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_errors_invalid_advicePBNCount_attr = {
	.attr  = { .name = "errors_invalid_advicePBNCount", .mode = 0444, },
	.show  = pool_stats_errors_invalid_advicePBNCount_show,
};

/**********************************************************************/
/** number of times a VIO completed with a VDO_NO_SPACE error */
static ssize_t pool_stats_errors_no_space_error_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.errors.no_space_error_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_errors_no_space_error_count_attr = {
	.attr  = { .name = "errors_no_space_error_count", .mode = 0444, },
	.show  = pool_stats_errors_no_space_error_count_show,
};

/**********************************************************************/
/** number of times a VIO completed with a VDO_READ_ONLY error */
static ssize_t pool_stats_errors_read_only_error_count_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kvdo_statistics(&layer->kvdo, &layer->vdo_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->vdo_stats_storage.errors.read_only_error_count);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_errors_read_only_error_count_attr = {
	.attr  = { .name = "errors_read_only_error_count", .mode = 0444, },
	.show  = pool_stats_errors_read_only_error_count_show,
};

/**********************************************************************/
/** The VDO instance */
static ssize_t pool_stats_instance_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->kernel_stats_storage.instance);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_instance_attr = {
	.attr  = { .name = "instance", .mode = 0444, },
	.show  = pool_stats_instance_show,
};

/**********************************************************************/
/** Current number of active VIOs */
static ssize_t pool_stats_currentVIOs_in_progress_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->kernel_stats_storage.current_vios_in_progress);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_currentVIOs_in_progress_attr = {
	.attr  = { .name = "currentVIOs_in_progress", .mode = 0444, },
	.show  = pool_stats_currentVIOs_in_progress_show,
};

/**********************************************************************/
/** Maximum number of active VIOs */
static ssize_t pool_stats_maxVIOs_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->kernel_stats_storage.max_vios);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_maxVIOs_attr = {
	.attr  = { .name = "maxVIOs", .mode = 0444, },
	.show  = pool_stats_maxVIOs_show,
};

/**********************************************************************/
/** Number of times the UDS index was too slow in responding */
static ssize_t pool_stats_dedupe_advice_timeouts_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.dedupe_advice_timeouts);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_dedupe_advice_timeouts_attr = {
	.attr  = { .name = "dedupe_advice_timeouts", .mode = 0444, },
	.show  = pool_stats_dedupe_advice_timeouts_show,
};

/**********************************************************************/
/** Number of flush requests submitted to the storage device */
static ssize_t pool_stats_flush_out_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.flush_out);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_flush_out_attr = {
	.attr  = { .name = "flush_out", .mode = 0444, },
	.show  = pool_stats_flush_out_show,
};

/**********************************************************************/
/** Logical block size */
static ssize_t pool_stats_logical_block_size_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.logical_block_size);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_logical_block_size_attr = {
	.attr  = { .name = "logical_block_size", .mode = 0444, },
	.show  = pool_stats_logical_block_size_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_in_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_read_attr = {
	.attr  = { .name = "bios_in_read", .mode = 0444, },
	.show  = pool_stats_bios_in_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_in_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_write_attr = {
	.attr  = { .name = "bios_in_write", .mode = 0444, },
	.show  = pool_stats_bios_in_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_in_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_discard_attr = {
	.attr  = { .name = "bios_in_discard", .mode = 0444, },
	.show  = pool_stats_bios_in_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_in_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_flush_attr = {
	.attr  = { .name = "bios_in_flush", .mode = 0444, },
	.show  = pool_stats_bios_in_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_in_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_fua_attr = {
	.attr  = { .name = "bios_in_fua", .mode = 0444, },
	.show  = pool_stats_bios_in_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_in_partial_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_partial.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_partial_read_attr = {
	.attr  = { .name = "bios_in_partial_read", .mode = 0444, },
	.show  = pool_stats_bios_in_partial_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_in_partial_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_partial.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_partial_write_attr = {
	.attr  = { .name = "bios_in_partial_write", .mode = 0444, },
	.show  = pool_stats_bios_in_partial_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_in_partial_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_partial.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_partial_discard_attr = {
	.attr  = { .name = "bios_in_partial_discard", .mode = 0444, },
	.show  = pool_stats_bios_in_partial_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_in_partial_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_partial.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_partial_flush_attr = {
	.attr  = { .name = "bios_in_partial_flush", .mode = 0444, },
	.show  = pool_stats_bios_in_partial_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_in_partial_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_partial.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_partial_fua_attr = {
	.attr  = { .name = "bios_in_partial_fua", .mode = 0444, },
	.show  = pool_stats_bios_in_partial_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_out_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_read_attr = {
	.attr  = { .name = "bios_out_read", .mode = 0444, },
	.show  = pool_stats_bios_out_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_out_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_write_attr = {
	.attr  = { .name = "bios_out_write", .mode = 0444, },
	.show  = pool_stats_bios_out_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_out_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_discard_attr = {
	.attr  = { .name = "bios_out_discard", .mode = 0444, },
	.show  = pool_stats_bios_out_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_out_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_flush_attr = {
	.attr  = { .name = "bios_out_flush", .mode = 0444, },
	.show  = pool_stats_bios_out_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_out_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_fua_attr = {
	.attr  = { .name = "bios_out_fua", .mode = 0444, },
	.show  = pool_stats_bios_out_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_meta_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_read_attr = {
	.attr  = { .name = "bios_meta_read", .mode = 0444, },
	.show  = pool_stats_bios_meta_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_meta_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_write_attr = {
	.attr  = { .name = "bios_meta_write", .mode = 0444, },
	.show  = pool_stats_bios_meta_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_meta_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_discard_attr = {
	.attr  = { .name = "bios_meta_discard", .mode = 0444, },
	.show  = pool_stats_bios_meta_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_meta_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_flush_attr = {
	.attr  = { .name = "bios_meta_flush", .mode = 0444, },
	.show  = pool_stats_bios_meta_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_meta_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_fua_attr = {
	.attr  = { .name = "bios_meta_fua", .mode = 0444, },
	.show  = pool_stats_bios_meta_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_journal_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_read_attr = {
	.attr  = { .name = "bios_journal_read", .mode = 0444, },
	.show  = pool_stats_bios_journal_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_journal_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_write_attr = {
	.attr  = { .name = "bios_journal_write", .mode = 0444, },
	.show  = pool_stats_bios_journal_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_journal_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_discard_attr = {
	.attr  = { .name = "bios_journal_discard", .mode = 0444, },
	.show  = pool_stats_bios_journal_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_journal_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_flush_attr = {
	.attr  = { .name = "bios_journal_flush", .mode = 0444, },
	.show  = pool_stats_bios_journal_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_journal_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_fua_attr = {
	.attr  = { .name = "bios_journal_fua", .mode = 0444, },
	.show  = pool_stats_bios_journal_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_page_cache_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_read_attr = {
	.attr  = { .name = "bios_page_cache_read", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_page_cache_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_write_attr = {
	.attr  = { .name = "bios_page_cache_write", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_page_cache_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_discard_attr = {
	.attr  = { .name = "bios_page_cache_discard", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_page_cache_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_flush_attr = {
	.attr  = { .name = "bios_page_cache_flush", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_page_cache_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_fua_attr = {
	.attr  = { .name = "bios_page_cache_fua", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_out_completed_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out_completed.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_completed_read_attr = {
	.attr  = { .name = "bios_out_completed_read", .mode = 0444, },
	.show  = pool_stats_bios_out_completed_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_out_completed_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out_completed.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_completed_write_attr = {
	.attr  = { .name = "bios_out_completed_write", .mode = 0444, },
	.show  = pool_stats_bios_out_completed_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_out_completed_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out_completed.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_completed_discard_attr = {
	.attr  = { .name = "bios_out_completed_discard", .mode = 0444, },
	.show  = pool_stats_bios_out_completed_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_out_completed_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out_completed.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_completed_flush_attr = {
	.attr  = { .name = "bios_out_completed_flush", .mode = 0444, },
	.show  = pool_stats_bios_out_completed_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_out_completed_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_out_completed.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_out_completed_fua_attr = {
	.attr  = { .name = "bios_out_completed_fua", .mode = 0444, },
	.show  = pool_stats_bios_out_completed_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_meta_completed_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta_completed.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_completed_read_attr = {
	.attr  = { .name = "bios_meta_completed_read", .mode = 0444, },
	.show  = pool_stats_bios_meta_completed_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_meta_completed_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta_completed.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_completed_write_attr = {
	.attr  = { .name = "bios_meta_completed_write", .mode = 0444, },
	.show  = pool_stats_bios_meta_completed_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_meta_completed_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta_completed.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_completed_discard_attr = {
	.attr  = { .name = "bios_meta_completed_discard", .mode = 0444, },
	.show  = pool_stats_bios_meta_completed_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_meta_completed_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta_completed.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_completed_flush_attr = {
	.attr  = { .name = "bios_meta_completed_flush", .mode = 0444, },
	.show  = pool_stats_bios_meta_completed_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_meta_completed_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_meta_completed.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_meta_completed_fua_attr = {
	.attr  = { .name = "bios_meta_completed_fua", .mode = 0444, },
	.show  = pool_stats_bios_meta_completed_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_journal_completed_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal_completed.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_completed_read_attr = {
	.attr  = { .name = "bios_journal_completed_read", .mode = 0444, },
	.show  = pool_stats_bios_journal_completed_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_journal_completed_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal_completed.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_completed_write_attr = {
	.attr  = { .name = "bios_journal_completed_write", .mode = 0444, },
	.show  = pool_stats_bios_journal_completed_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_journal_completed_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal_completed.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_completed_discard_attr = {
	.attr  = { .name = "bios_journal_completed_discard", .mode = 0444, },
	.show  = pool_stats_bios_journal_completed_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_journal_completed_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal_completed.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_completed_flush_attr = {
	.attr  = { .name = "bios_journal_completed_flush", .mode = 0444, },
	.show  = pool_stats_bios_journal_completed_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_journal_completed_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_journal_completed.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_journal_completed_fua_attr = {
	.attr  = { .name = "bios_journal_completed_fua", .mode = 0444, },
	.show  = pool_stats_bios_journal_completed_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_page_cache_completed_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache_completed.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_completed_read_attr = {
	.attr  = { .name = "bios_page_cache_completed_read", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_completed_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_page_cache_completed_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache_completed.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_completed_write_attr = {
	.attr  = { .name = "bios_page_cache_completed_write", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_completed_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_page_cache_completed_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache_completed.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_completed_discard_attr = {
	.attr  = { .name = "bios_page_cache_completed_discard", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_completed_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_page_cache_completed_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache_completed.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_completed_flush_attr = {
	.attr  = { .name = "bios_page_cache_completed_flush", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_completed_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_page_cache_completed_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_page_cache_completed.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_page_cache_completed_fua_attr = {
	.attr  = { .name = "bios_page_cache_completed_fua", .mode = 0444, },
	.show  = pool_stats_bios_page_cache_completed_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_acknowledged_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_read_attr = {
	.attr  = { .name = "bios_acknowledged_read", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_acknowledged_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_write_attr = {
	.attr  = { .name = "bios_acknowledged_write", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_acknowledged_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_discard_attr = {
	.attr  = { .name = "bios_acknowledged_discard", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_acknowledged_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_flush_attr = {
	.attr  = { .name = "bios_acknowledged_flush", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_acknowledged_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_fua_attr = {
	.attr  = { .name = "bios_acknowledged_fua", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_acknowledged_partial_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged_partial.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_partial_read_attr = {
	.attr  = { .name = "bios_acknowledged_partial_read", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_partial_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_acknowledged_partial_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged_partial.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_partial_write_attr = {
	.attr  = { .name = "bios_acknowledged_partial_write", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_partial_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_acknowledged_partial_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged_partial.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_partial_discard_attr = {
	.attr  = { .name = "bios_acknowledged_partial_discard", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_partial_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_acknowledged_partial_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged_partial.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_partial_flush_attr = {
	.attr  = { .name = "bios_acknowledged_partial_flush", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_partial_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_acknowledged_partial_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_acknowledged_partial.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_acknowledged_partial_fua_attr = {
	.attr  = { .name = "bios_acknowledged_partial_fua", .mode = 0444, },
	.show  = pool_stats_bios_acknowledged_partial_fua_show,
};

/**********************************************************************/
/** Number of not REQ_WRITE bios */
static ssize_t pool_stats_bios_in_progress_read_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_progress.read);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_progress_read_attr = {
	.attr  = { .name = "bios_in_progress_read", .mode = 0444, },
	.show  = pool_stats_bios_in_progress_read_show,
};

/**********************************************************************/
/** Number of REQ_WRITE bios */
static ssize_t pool_stats_bios_in_progress_write_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_progress.write);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_progress_write_attr = {
	.attr  = { .name = "bios_in_progress_write", .mode = 0444, },
	.show  = pool_stats_bios_in_progress_write_show,
};

/**********************************************************************/
/** Number of REQ_DISCARD bios */
static ssize_t pool_stats_bios_in_progress_discard_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_progress.discard);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_progress_discard_attr = {
	.attr  = { .name = "bios_in_progress_discard", .mode = 0444, },
	.show  = pool_stats_bios_in_progress_discard_show,
};

/**********************************************************************/
/** Number of REQ_FLUSH bios */
static ssize_t pool_stats_bios_in_progress_flush_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_progress.flush);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_progress_flush_attr = {
	.attr  = { .name = "bios_in_progress_flush", .mode = 0444, },
	.show  = pool_stats_bios_in_progress_flush_show,
};

/**********************************************************************/
/** Number of REQ_FUA bios */
static ssize_t pool_stats_bios_in_progress_fua_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.bios_in_progress.fua);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_bios_in_progress_fua_attr = {
	.attr  = { .name = "bios_in_progress_fua", .mode = 0444, },
	.show  = pool_stats_bios_in_progress_fua_show,
};

/**********************************************************************/
/** Tracked bytes currently allocated. */
static ssize_t pool_stats_memory_usage_bytes_used_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.memory_usage.bytes_used);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_memory_usage_bytes_used_attr = {
	.attr  = { .name = "memory_usage_bytes_used", .mode = 0444, },
	.show  = pool_stats_memory_usage_bytes_used_show,
};

/**********************************************************************/
/** Maximum tracked bytes allocated. */
static ssize_t pool_stats_memory_usage_peak_bytes_used_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.memory_usage.peak_bytes_used);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_memory_usage_peak_bytes_used_attr = {
	.attr  = { .name = "memory_usage_peak_bytes_used", .mode = 0444, },
	.show  = pool_stats_memory_usage_peak_bytes_used_show,
};

/**********************************************************************/
/** Number of chunk names stored in the index */
static ssize_t pool_stats_index_entries_indexed_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.entries_indexed);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_entries_indexed_attr = {
	.attr  = { .name = "index_entries_indexed", .mode = 0444, },
	.show  = pool_stats_index_entries_indexed_show,
};

/**********************************************************************/
/** Number of post calls that found an existing entry */
static ssize_t pool_stats_index_posts_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.posts_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_posts_found_attr = {
	.attr  = { .name = "index_posts_found", .mode = 0444, },
	.show  = pool_stats_index_posts_found_show,
};

/**********************************************************************/
/** Number of post calls that added a new entry */
static ssize_t pool_stats_index_posts_not_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.posts_not_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_posts_not_found_attr = {
	.attr  = { .name = "index_posts_not_found", .mode = 0444, },
	.show  = pool_stats_index_posts_not_found_show,
};

/**********************************************************************/
/** Number of query calls that found an existing entry */
static ssize_t pool_stats_index_queries_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.queries_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_queries_found_attr = {
	.attr  = { .name = "index_queries_found", .mode = 0444, },
	.show  = pool_stats_index_queries_found_show,
};

/**********************************************************************/
/** Number of query calls that added a new entry */
static ssize_t pool_stats_index_queries_not_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.queries_not_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_queries_not_found_attr = {
	.attr  = { .name = "index_queries_not_found", .mode = 0444, },
	.show  = pool_stats_index_queries_not_found_show,
};

/**********************************************************************/
/** Number of update calls that found an existing entry */
static ssize_t pool_stats_index_updates_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.updates_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_updates_found_attr = {
	.attr  = { .name = "index_updates_found", .mode = 0444, },
	.show  = pool_stats_index_updates_found_show,
};

/**********************************************************************/
/** Number of update calls that added a new entry */
static ssize_t pool_stats_index_updates_not_found_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%llu\n", layer->kernel_stats_storage.index.updates_not_found);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_updates_not_found_attr = {
	.attr  = { .name = "index_updates_not_found", .mode = 0444, },
	.show  = pool_stats_index_updates_not_found_show,
};

/**********************************************************************/
/** Current number of dedupe queries that are in flight */
static ssize_t pool_stats_index_curr_dedupe_queries_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->kernel_stats_storage.index.curr_dedupe_queries);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_curr_dedupe_queries_attr = {
	.attr  = { .name = "index_curr_dedupe_queries", .mode = 0444, },
	.show  = pool_stats_index_curr_dedupe_queries_show,
};

/**********************************************************************/
/** Maximum number of dedupe queries that have been in flight */
static ssize_t pool_stats_index_max_dedupe_queries_show(struct kernel_layer *layer, char *buf)
{
	ssize_t retval;
	mutex_lock(&layer->statsMutex);
	get_kernel_statistics(layer, &layer->kernel_stats_storage);
	retval = sprintf(buf, "%" PRIu32 "\n", layer->kernel_stats_storage.index.max_dedupe_queries);
	mutex_unlock(&layer->statsMutex);
	return retval;
}

static struct pool_stats_attribute pool_stats_index_max_dedupe_queries_attr = {
	.attr  = { .name = "index_max_dedupe_queries", .mode = 0444, },
	.show  = pool_stats_index_max_dedupe_queries_show,
};

struct attribute *pool_stats_attrs[] = {
	&pool_stats_data_blocks_used_attr.attr,
	&pool_stats_overhead_blocks_used_attr.attr,
	&pool_stats_logical_blocks_used_attr.attr,
	&pool_stats_physical_blocks_attr.attr,
	&pool_stats_logical_blocks_attr.attr,
	&pool_stats_block_map_cache_size_attr.attr,
	&pool_stats_write_policy_attr.attr,
	&pool_stats_block_size_attr.attr,
	&pool_stats_complete_recoveries_attr.attr,
	&pool_stats_read_only_recoveries_attr.attr,
	&pool_stats_mode_attr.attr,
	&pool_stats_in_recovery_mode_attr.attr,
	&pool_stats_recovery_percentage_attr.attr,
	&pool_stats_packer_compressed_fragments_written_attr.attr,
	&pool_stats_packer_compressed_blocks_written_attr.attr,
	&pool_stats_packer_compressed_fragments_in_packer_attr.attr,
	&pool_stats_allocator_slab_count_attr.attr,
	&pool_stats_allocator_slabs_opened_attr.attr,
	&pool_stats_allocator_slabs_reopened_attr.attr,
	&pool_stats_journal_disk_full_attr.attr,
	&pool_stats_journal_slab_journal_commits_requested_attr.attr,
	&pool_stats_journal_entries_started_attr.attr,
	&pool_stats_journal_entries_written_attr.attr,
	&pool_stats_journal_entries_committed_attr.attr,
	&pool_stats_journal_blocks_started_attr.attr,
	&pool_stats_journal_blocks_written_attr.attr,
	&pool_stats_journal_blocks_committed_attr.attr,
	&pool_stats_slab_journal_disk_full_count_attr.attr,
	&pool_stats_slab_journal_flush_count_attr.attr,
	&pool_stats_slab_journal_blocked_count_attr.attr,
	&pool_stats_slab_journal_blocks_written_attr.attr,
	&pool_stats_slab_journal_tail_busy_count_attr.attr,
	&pool_stats_slab_summary_blocks_written_attr.attr,
	&pool_stats_ref_counts_blocks_written_attr.attr,
	&pool_stats_block_map_dirty_pages_attr.attr,
	&pool_stats_block_map_clean_pages_attr.attr,
	&pool_stats_block_map_free_pages_attr.attr,
	&pool_stats_block_map_failed_pages_attr.attr,
	&pool_stats_block_map_incoming_pages_attr.attr,
	&pool_stats_block_map_outgoing_pages_attr.attr,
	&pool_stats_block_map_cache_pressure_attr.attr,
	&pool_stats_block_map_read_count_attr.attr,
	&pool_stats_block_map_write_count_attr.attr,
	&pool_stats_block_map_failed_reads_attr.attr,
	&pool_stats_block_map_failed_writes_attr.attr,
	&pool_stats_block_map_reclaimed_attr.attr,
	&pool_stats_block_map_read_outgoing_attr.attr,
	&pool_stats_block_map_found_in_cache_attr.attr,
	&pool_stats_block_map_discard_required_attr.attr,
	&pool_stats_block_map_wait_for_page_attr.attr,
	&pool_stats_block_map_fetch_required_attr.attr,
	&pool_stats_block_map_pages_loaded_attr.attr,
	&pool_stats_block_map_pages_saved_attr.attr,
	&pool_stats_block_map_flush_count_attr.attr,
	&pool_stats_hash_lock_dedupe_advice_valid_attr.attr,
	&pool_stats_hash_lock_dedupe_advice_stale_attr.attr,
	&pool_stats_hash_lock_concurrent_data_matches_attr.attr,
	&pool_stats_hash_lock_concurrent_hash_collisions_attr.attr,
	&pool_stats_errors_invalid_advicePBNCount_attr.attr,
	&pool_stats_errors_no_space_error_count_attr.attr,
	&pool_stats_errors_read_only_error_count_attr.attr,
	&pool_stats_instance_attr.attr,
	&pool_stats_currentVIOs_in_progress_attr.attr,
	&pool_stats_maxVIOs_attr.attr,
	&pool_stats_dedupe_advice_timeouts_attr.attr,
	&pool_stats_flush_out_attr.attr,
	&pool_stats_logical_block_size_attr.attr,
	&pool_stats_bios_in_read_attr.attr,
	&pool_stats_bios_in_write_attr.attr,
	&pool_stats_bios_in_discard_attr.attr,
	&pool_stats_bios_in_flush_attr.attr,
	&pool_stats_bios_in_fua_attr.attr,
	&pool_stats_bios_in_partial_read_attr.attr,
	&pool_stats_bios_in_partial_write_attr.attr,
	&pool_stats_bios_in_partial_discard_attr.attr,
	&pool_stats_bios_in_partial_flush_attr.attr,
	&pool_stats_bios_in_partial_fua_attr.attr,
	&pool_stats_bios_out_read_attr.attr,
	&pool_stats_bios_out_write_attr.attr,
	&pool_stats_bios_out_discard_attr.attr,
	&pool_stats_bios_out_flush_attr.attr,
	&pool_stats_bios_out_fua_attr.attr,
	&pool_stats_bios_meta_read_attr.attr,
	&pool_stats_bios_meta_write_attr.attr,
	&pool_stats_bios_meta_discard_attr.attr,
	&pool_stats_bios_meta_flush_attr.attr,
	&pool_stats_bios_meta_fua_attr.attr,
	&pool_stats_bios_journal_read_attr.attr,
	&pool_stats_bios_journal_write_attr.attr,
	&pool_stats_bios_journal_discard_attr.attr,
	&pool_stats_bios_journal_flush_attr.attr,
	&pool_stats_bios_journal_fua_attr.attr,
	&pool_stats_bios_page_cache_read_attr.attr,
	&pool_stats_bios_page_cache_write_attr.attr,
	&pool_stats_bios_page_cache_discard_attr.attr,
	&pool_stats_bios_page_cache_flush_attr.attr,
	&pool_stats_bios_page_cache_fua_attr.attr,
	&pool_stats_bios_out_completed_read_attr.attr,
	&pool_stats_bios_out_completed_write_attr.attr,
	&pool_stats_bios_out_completed_discard_attr.attr,
	&pool_stats_bios_out_completed_flush_attr.attr,
	&pool_stats_bios_out_completed_fua_attr.attr,
	&pool_stats_bios_meta_completed_read_attr.attr,
	&pool_stats_bios_meta_completed_write_attr.attr,
	&pool_stats_bios_meta_completed_discard_attr.attr,
	&pool_stats_bios_meta_completed_flush_attr.attr,
	&pool_stats_bios_meta_completed_fua_attr.attr,
	&pool_stats_bios_journal_completed_read_attr.attr,
	&pool_stats_bios_journal_completed_write_attr.attr,
	&pool_stats_bios_journal_completed_discard_attr.attr,
	&pool_stats_bios_journal_completed_flush_attr.attr,
	&pool_stats_bios_journal_completed_fua_attr.attr,
	&pool_stats_bios_page_cache_completed_read_attr.attr,
	&pool_stats_bios_page_cache_completed_write_attr.attr,
	&pool_stats_bios_page_cache_completed_discard_attr.attr,
	&pool_stats_bios_page_cache_completed_flush_attr.attr,
	&pool_stats_bios_page_cache_completed_fua_attr.attr,
	&pool_stats_bios_acknowledged_read_attr.attr,
	&pool_stats_bios_acknowledged_write_attr.attr,
	&pool_stats_bios_acknowledged_discard_attr.attr,
	&pool_stats_bios_acknowledged_flush_attr.attr,
	&pool_stats_bios_acknowledged_fua_attr.attr,
	&pool_stats_bios_acknowledged_partial_read_attr.attr,
	&pool_stats_bios_acknowledged_partial_write_attr.attr,
	&pool_stats_bios_acknowledged_partial_discard_attr.attr,
	&pool_stats_bios_acknowledged_partial_flush_attr.attr,
	&pool_stats_bios_acknowledged_partial_fua_attr.attr,
	&pool_stats_bios_in_progress_read_attr.attr,
	&pool_stats_bios_in_progress_write_attr.attr,
	&pool_stats_bios_in_progress_discard_attr.attr,
	&pool_stats_bios_in_progress_flush_attr.attr,
	&pool_stats_bios_in_progress_fua_attr.attr,
	&pool_stats_memory_usage_bytes_used_attr.attr,
	&pool_stats_memory_usage_peak_bytes_used_attr.attr,
	&pool_stats_index_entries_indexed_attr.attr,
	&pool_stats_index_posts_found_attr.attr,
	&pool_stats_index_posts_not_found_attr.attr,
	&pool_stats_index_queries_found_attr.attr,
	&pool_stats_index_queries_not_found_attr.attr,
	&pool_stats_index_updates_found_attr.attr,
	&pool_stats_index_updates_not_found_attr.attr,
	&pool_stats_index_curr_dedupe_queries_attr.attr,
	&pool_stats_index_max_dedupe_queries_attr.attr,
	NULL,
};
