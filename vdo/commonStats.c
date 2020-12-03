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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/commonStats.c#6 $
 *
 * Common stat functions
 *
 */
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

/**********************************************************************/
static void copy_bio_stat(struct bio_stats *b,
			  const struct atomic_bio_stats *a)
{
	b->read = atomic64_read(&a->read);
	b->write = atomic64_read(&a->write);
	b->discard = atomic64_read(&a->discard);
	b->flush = atomic64_read(&a->flush);
	b->fua = atomic64_read(&a->fua);
}

/**********************************************************************/
static struct bio_stats subtract_bio_stats(struct bio_stats minuend,
					   struct bio_stats subtrahend)
{
	return (struct bio_stats) {
		.read = minuend.read - subtrahend.read,
		.write = minuend.write - subtrahend.write,
		.discard = minuend.discard - subtrahend.discard,
		.flush = minuend.flush - subtrahend.flush,
		.fua = minuend.fua - subtrahend.fua,
	};
}

/**********************************************************************/
void get_kernel_statistics(struct kernel_layer *layer,
			   struct kernel_statistics *stats)
{
	stats->version = STATISTICS_VERSION;
	stats->release_version = CURRENT_RELEASE_VERSION_NUMBER;
	stats->instance = layer->instance;
	get_limiter_values_atomically(&layer->request_limiter,
				      &stats->current_vios_in_progress,
				      &stats->max_vios);
	// albireoTimeoutReport gives the number of timeouts, and
	// dedupeContextBusy gives the number of queries not made because of
	// earlier timeouts.
	stats->dedupe_advice_timeouts =
		(get_dedupe_timeout_count(layer->dedupe_index) +
		 atomic64_read(&layer->dedupeContextBusy));
	stats->flush_out = atomic64_read(&layer->flushOut);
	stats->logical_block_size = layer->device_config->logical_block_size;
	copy_bio_stat(&stats->bios_in, &layer->biosIn);
	copy_bio_stat(&stats->bios_in_partial, &layer->biosInPartial);
	copy_bio_stat(&stats->bios_out, &layer->biosOut);
	copy_bio_stat(&stats->bios_meta, &layer->biosMeta);
	copy_bio_stat(&stats->bios_journal, &layer->biosJournal);
	copy_bio_stat(&stats->bios_page_cache, &layer->biosPageCache);
	copy_bio_stat(&stats->bios_out_completed, &layer->biosOutCompleted);
	copy_bio_stat(&stats->bios_meta_completed, &layer->biosMetaCompleted);
	copy_bio_stat(&stats->bios_journal_completed,
		      &layer->biosJournalCompleted);
	copy_bio_stat(&stats->bios_page_cache_completed,
		      &layer->biosPageCacheCompleted);
	copy_bio_stat(&stats->bios_acknowledged, &layer->biosAcknowledged);
	copy_bio_stat(&stats->bios_acknowledged_partial,
		      &layer->biosAcknowledgedPartial);
	stats->bios_in_progress =
		subtract_bio_stats(stats->bios_in, stats->bios_acknowledged);
	stats->memory_usage = get_memory_usage();
	get_index_statistics(layer->dedupe_index, &stats->index);
}
