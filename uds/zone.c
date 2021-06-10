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
 * $Id: //eng/uds-releases/krusty/src/uds/zone.c#5 $
 */

#include "zone.h"

#include "logger.h"
#include "threads.h"

/**********************************************************************/
unsigned int get_zone_count(const struct uds_parameters *user_params)
{
	unsigned int zone_count =
		(user_params == NULL) ? 0 : user_params->zone_count;
	if (zone_count == 0) {
		zone_count = get_num_cores() / 2;
	}
	if (zone_count < 1) {
		zone_count = 1;
	}
	if (zone_count > MAX_ZONES) {
		zone_count = MAX_ZONES;
	}
	uds_log_info("Using %u indexing zone%s for concurrency.",
		     zone_count,
		     zone_count == 1 ? "" : "s");
	return zone_count;
}
