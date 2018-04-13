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
 * $Id: //eng/uds-releases/gloria/src/uds/indexConfig.h#1 $
 */

#ifndef INDEX_CONFIG_H
#define INDEX_CONFIG_H 1

#include "config.h"
#include "geometry.h"

/**
 * A set of configuration parameters for the indexer.
 **/
struct configuration {
  /* Parameters for the volume */

  /* The volume layout */
  Geometry *geometry;

  /* Size of the page cache and sparse chapter index cache, in chapters */
  unsigned int cacheChapters;

  /* The number of chapters to write before checkpointing */
  unsigned int checkpointFrequency;

  /** Parameters for the master index */

  /* The mean delta for the master index */
  unsigned int masterIndexMeanDelta;

  /* Sampling rate for sparse indexing */
  unsigned int sparseSampleRate;
};

#endif /* INDEX_CONFIG_H */
