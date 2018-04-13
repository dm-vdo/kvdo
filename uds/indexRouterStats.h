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
 * $Id: //eng/uds-releases/gloria/src/uds/indexRouterStats.h#1 $
 */

#ifndef INDEX_ROUTER_STATS_H
#define INDEX_ROUTER_STATS_H

#include "cacheCounters.h"

struct indexRouterStatCounters {
  uint64_t      entriesIndexed;
  uint64_t      memoryUsed;
  uint64_t      diskUsed;
  uint64_t      collisions;
  uint64_t      entriesDiscarded;
  uint64_t      checkpoints;
  CacheCounters volumeCache;
};

#endif /* INDEX_ROUTER_STATS_H */
