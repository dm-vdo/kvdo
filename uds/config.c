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
 * $Id: //eng/uds-releases/krusty/src/uds/config.c#4 $
 */

#include "config.h"

#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

/**********************************************************************/
void free_index_location(struct index_location *loc)
{
	if (loc == NULL) {
		return;
	}

	FREE(loc->host);
	FREE(loc->port);
	FREE(loc->directory);
}

/**********************************************************************/
bool are_uds_configurations_equal(struct uds_configuration *a,
				  struct uds_configuration *b)
{
	bool result = true;
	if (a->record_pages_per_chapter != b->record_pages_per_chapter) {
		logError("Record pages per chapter (%u) does not match (%u)",
			 a->record_pages_per_chapter,
			 b->record_pages_per_chapter);
		result = false;
	}
	if (a->chapters_per_volume != b->chapters_per_volume) {
		logError("Chapter count (%u) does not match (%u)",
			 a->chapters_per_volume,
			 b->chapters_per_volume);
		result = false;
	}
	if (a->sparse_chapters_per_volume != b->sparse_chapters_per_volume) {
		logError("Sparse chapter count (%u) does not match (%u)",
			 a->sparse_chapters_per_volume,
			 b->sparse_chapters_per_volume);
		result = false;
	}
	if (a->cache_chapters != b->cache_chapters) {
		logError("Cache size (%u) does not match (%u)",
			 a->cache_chapters,
			 b->cache_chapters);
		result = false;
	}
	if (a->master_index_mean_delta != b->master_index_mean_delta) {
		logError("Master index mean delta (%u) does not match (%u)",
			 a->master_index_mean_delta,
			 b->master_index_mean_delta);
		result = false;
	}
	if (a->bytes_per_page != b->bytes_per_page) {
		logError("Bytes per page value (%u) does not match (%u)",
			 a->bytes_per_page,
			 b->bytes_per_page);
		result = false;
	}
	if (a->sparse_sample_rate != b->sparse_sample_rate) {
		logError("Sparse sample rate (%u) does not match (%u)",
			 a->sparse_sample_rate,
			 b->sparse_sample_rate);
		result = false;
	}
	if (a->nonce != b->nonce) {
		logError("Nonce (%llu) does not match (%llu)",
			 a->nonce,
			 b->nonce);
		result = false;
	}
	return result;
}

/**********************************************************************/
void log_uds_configuration(struct uds_configuration *conf)
{
	logDebug("Configuration:");
	logDebug("  Record pages per chapter:   %10u",
		 conf->record_pages_per_chapter);
	logDebug("  Chapters per volume:        %10u",
		 conf->chapters_per_volume);
	logDebug("  Sparse chapters per volume: %10u",
		 conf->sparse_chapters_per_volume);
	logDebug("  Cache size (chapters):      %10u", conf->cache_chapters);
	logDebug("  Master index mean delta:    %10u",
		 conf->master_index_mean_delta);
	logDebug("  Bytes per page:             %10u", conf->bytes_per_page);
	logDebug("  Sparse sample rate:         %10u",
		 conf->sparse_sample_rate);
	logDebug("  Nonce:                      %llu", conf->nonce);
}
