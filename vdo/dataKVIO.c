// SPDX-License-Identifier: GPL-2.0-only
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
 */

#include "dataKVIO.h"

#include <asm/unaligned.h>
#include <linux/lz4.h>

#include "logger.h"
#include "memory-alloc.h"
#include "permassert.h"

#include "atomic-stats.h"
#include "comparisons.h"
#include "compressed-block.h"
#include "constants.h"
#include "data-vio.h"
#include "hash-lock.h"
#include "io-submitter.h"
#include "vdo.h"

#include "bio.h"
#include "dedupe-index.h"
#include "dump.h"
#include "kvio.h"

/**********************************************************************/
void check_data_vio_for_duplication(struct data_vio *data_vio)
{

	ASSERT_LOG_ONLY(!data_vio->is_zero_block,
			"zero block not checked for duplication");
	ASSERT_LOG_ONLY(data_vio->new_mapped.state != VDO_MAPPING_STATE_UNMAPPED,
			"discard not checked for duplication");

	if (data_vio_has_allocation(data_vio)) {
		vdo_post_dedupe_advice(data_vio);
	} else {
		/*
		 * This block has not actually been written (presumably because
		 * we are full), so attempt to dedupe without posting bogus
		 * advice.
		 */
		vdo_query_dedupe_advice(data_vio);
	}
}

/**********************************************************************/
void vdo_update_dedupe_index(struct data_vio *data_vio)
{
	vdo_update_dedupe_advice(data_vio);
}

/*
 * Get the state needed to generate UDS metadata from the data_vio wrapping a
 * dedupe_context.
 *
 * FIXME: the name says nothing about data vios or dedupe_contexts or data
 * locations, just about VDOs, maybe too generic.
 */
struct data_location
vdo_get_dedupe_advice(const struct dedupe_context *context)
{
	struct data_vio *data_vio = container_of(context,
						 struct data_vio,
						 dedupe_context);
	return (struct data_location) {
		.state = data_vio->new_mapped.state,
		.pbn = data_vio->new_mapped.pbn,
	};
}

void vdo_set_dedupe_advice(struct dedupe_context *context,
			   const struct data_location *advice)
{
	receive_data_vio_dedupe_advice(container_of(context,
						    struct data_vio,
						    dedupe_context),
				       advice);
}
