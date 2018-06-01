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
 * $Id: //eng/uds-releases/gloria/src/uds/grid.h#3 $
 */

#ifndef GRID_H
#define GRID_H

#include "compiler.h"
#include "config.h"
#include "loadType.h"
#include "indexLayout.h"
#include "indexRouter.h"

struct grid {
  struct udsConfiguration   userConfig;
  unsigned int              numRouters;
  IndexRouter             **routers;
  IndexLayout              *layout;
};

/**
 * Create a local grid structure from the given configuration.
 *
 * @param layout         The index layout for the new index.
 * @param loadType       How to load the index
 * @param userConfig     The udsConfiguration to use
 * @param indexCallback  The callback handler for index requests
 * @param grid           The new grid structure
 *
 * @return               Either UDS_SUCCESS or an error code.
 *
 * @note The layout becomes owned by the grid and will be destroyed
 *       when the grid is freed.
 **/
int makeLocalGrid(IndexLayout          *layout,
                  LoadType              loadType,
                  UdsConfiguration      userConfig,
                  IndexRouterCallback   indexCallback,
                  Grid                **grid)
  __attribute__((warn_unused_result));

/**
 * Create a new remote grid structure from the given configuration.
 *
 * @param gridConfig     The UdsGridConfig to use
 * @param indexCallback  The callback handler for index requests
 * @param grid           The new grid structure
 *
 * @return               Either UDS_SUCCESS or an error code.
 **/
int makeRemoteGrid(UdsGridConfig         gridConfig,
                   IndexRouterCallback   indexCallback,
                   Grid                **grid)
  __attribute__((warn_unused_result));

/**
 * Select a router to handle the request for this name.
 *
 * @param grid   The index grid to use
 * @param name   The name to dispatch
 *
 * @return       The IndexRouter to use for this name
 **/
IndexRouter *selectGridRouter(Grid *grid, UdsChunkName *name)
  __attribute__((warn_unused_result));

/**
 * Call getStatistics() on all routers in this grid.
 *
 * @param grid      The index grid to use
 * @param stats     The aggregate stats for this grid
 *
 * @return          Either UDS_SUCCESS or an error code
 **/
int getGridStatistics(Grid *grid, IndexRouterStatCounters *stats)
  __attribute__((warn_unused_result));

/**
 * Call setCheckpointFrequency() on all routers in this grid.
 *
 * @param grid      The index grid to use
 * @param frequency The new frequency.
 *
 * @return          Either UDS_SUCCESS or an error code
 **/
int setGridCheckpointFrequency(Grid *grid, unsigned int frequency)
  __attribute__((warn_unused_result));

/**
 * Flush all the routers, optionally save state for all the routers, and free
 * all components of the grid.
 *
 * @param grid      The index grid to save and free.
 * @param saveFlag  True to save the grid.
 *
 * @return Either UDS_SUCCESS or an error code
 **/
int saveAndFreeGrid(Grid *grid, bool saveFlag);

/**
 * Free all components of the grid.
 *
 * @param grid   The index grid to free
 **/
static INLINE void freeGrid(Grid *grid)
{
  saveAndFreeGrid(grid, false);
}

#endif /* GRID_H */
