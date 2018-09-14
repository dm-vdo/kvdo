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
 * $Id: //eng/uds-releases/gloria/src/uds/grid.c#4 $
 */

#include "grid.h"

#include "hashUtils.h"
#include "localIndexRouter.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "threads.h"

// Data exchanged with per-router filling threads.
struct fillData {
  // Router to fill from this thread
  IndexRouter *router;
  // Seed value for PRNG
  int          seed;
  // UDS_SUCCESS or error from router
  int          result;
};

/**********************************************************************/
static int printUserConfiguration(Grid *grid)
{
  char *udsConfigString = NULL;
  int result = printUdsConfiguration(&grid->userConfig, 2, &udsConfigString);
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "Failed allocate configuration string");
  }
  logDebug("Configuration:\n%s", udsConfigString);
  FREE(udsConfigString);
  return UDS_SUCCESS;
}

/**********************************************************************/
int makeLocalGrid(IndexLayout          *layout,
                  LoadType              loadType,
                  UdsConfiguration      userConfig,
                  IndexRouterCallback   indexCallback,
                  Grid                **grid)
{
  Grid *newGrid;
  int result = ALLOCATE(1, struct grid, "grid", &newGrid);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (userConfig != NULL) {
    newGrid->userConfig = *userConfig;
  }

  if (loadType == LOAD_CREATE) {
    result = writeIndexConfig(layout, &newGrid->userConfig);
  } else {
    result = readIndexConfig(layout, &newGrid->userConfig);
  }

  if (result != UDS_SUCCESS) {
    freeGrid(newGrid);
    return result;
  }

  newGrid->numRouters = 1;
  result = ALLOCATE(1,
                    IndexRouter *,
                    "create routers",
                    &newGrid->routers);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to allocate routers");
    freeGrid(newGrid);
    return result;
  }

  Configuration *indexConfig;
  result = makeConfiguration(&newGrid->userConfig, &indexConfig);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to allocate config");
    freeGrid(newGrid);
    return result;
  }

  result = makeLocalIndexRouter(layout,
                                indexConfig,
                                loadType,
                                indexCallback,
                                &newGrid->routers[0]);
  freeConfiguration(indexConfig);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "Failed to make router");
    freeGrid(newGrid);
    return result;
  }

  result = printUserConfiguration(newGrid);
  if (result != UDS_SUCCESS) {
    freeGrid(newGrid);
    return result;
  }

  // Do this last so that freeGrid does not free the layout until and unless
  // this function returns successfully.
  newGrid->layout = layout;
  *grid = newGrid;
  return UDS_SUCCESS;
}

/**********************************************************************/
IndexRouter *selectGridRouter(Grid *grid, UdsChunkName *name)
{
  unsigned int gridBytes = extractGridRouterBytes(name);
  unsigned int numRouters = grid->numRouters;
  unsigned int routerNumber = (numRouters == 1) ? 0 : (gridBytes % numRouters);
  return grid->routers[routerNumber];
}

/**********************************************************************/
int getGridStatistics(Grid *grid, IndexRouterStatCounters *counters)
{
  memset(counters, 0, sizeof(IndexRouterStatCounters));
  for (unsigned int i = 0; i < grid->numRouters; i++) {
    IndexRouter *router = grid->routers[i];
    IndexRouterStatCounters routerStats;
    int result = router->methods->getStatistics(router, &routerStats);
    if (result != UDS_SUCCESS) {
      return result;
    }
    counters->entriesIndexed   += routerStats.entriesIndexed;
    counters->memoryUsed       += routerStats.memoryUsed;
    counters->diskUsed         += routerStats.diskUsed;
    counters->collisions       += routerStats.collisions;
    counters->entriesDiscarded += routerStats.entriesDiscarded;
    counters->checkpoints      += routerStats.checkpoints;
    addCacheCounters(&counters->volumeCache, &routerStats.volumeCache);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int setGridCheckpointFrequency(Grid *grid, unsigned int frequency)
{
  for (unsigned int i = 0; i < grid->numRouters; i++) {
    IndexRouter *router = grid->routers[i];
    router->methods->setCheckpointFrequency(router, frequency);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int saveAndFreeGrid(Grid *grid, bool saveFlag)
{
  if (grid == NULL) {
    return UDS_SUCCESS;
  }

  int result = UDS_SUCCESS;
  if (grid->routers != NULL) {
    for (unsigned int i = 0; i < grid->numRouters; i++) {
      IndexRouter *router = grid->routers[i];
      if (router != NULL) {
        int routerResult = saveAndFreeIndexRouter(router, saveFlag);
        if (routerResult != UDS_SUCCESS) {
          logWarningWithStringError(routerResult,
                                    "index router save state problem");
          result = routerResult;
        }
      }
    }
  }

  freeIndexLayout(&grid->layout);
  FREE(grid->routers);
  FREE(grid);
  return result;
}
