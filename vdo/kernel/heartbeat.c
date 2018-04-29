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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/heartbeat.c#1 $
 */

#include "heartbeat.h"

#include "kernelLayer.h"
#include "kernelVDOInternals.h"
#include "logger.h"
#include "vdo.h"

static Jiffies heartbeatInterval;

/**********************************************************************/
void initializeHeartbeatRate(void)
{
  /*
   * The kernel timer frequency is a configuration option.  As of the
   * 3.2 kernel, options offered include 100 Hz, 250 Hz, 300 Hz, and
   * 1000 Hz, with 250 Hz being the default and 100 Hz recommended for
   * servers for which interactive responsiveness is not critical.
   *
   * A HEARTBEAT_INTERVAL that divides evenly by 4ms can be exactly
   * expressed in jiffies on a system using a 250 Hz clock.  If it's
   * divisible by 20ms, it can be exactly expressed with any of the
   * four rates listed above.
   */
  heartbeatInterval = usecs_to_jiffies(HEARTBEAT_INTERVAL+1) - 1;
  unsigned long heartbeatUsec = jiffies_to_usecs(heartbeatInterval);
  if (heartbeatUsec != HEARTBEAT_INTERVAL) {
    logWarning("configured heartbeat interval (%u usec)"
               " not a multiple of jiffy (%u usec)",
               HEARTBEAT_INTERVAL, jiffies_to_usecs(1));
    logWarning("rounding down heartbeat interval to %lu usec", heartbeatUsec);
  }
}

/**
 * Handle heartbeat requests in request queue
 *
 * @param item the workitem
 */
static void kvdoHeartbeatWork(KvdoWorkItem *item)
{
  HeartbeatData *heartbeat = container_of(item, HeartbeatData, workItem);
  // XXX Needs to know about rest of KVDOThread.
  KVDOThread *thread = container_of(heartbeat, KVDOThread, heartbeat);
  if (!heartbeat->stop) {
    ThreadID id = thread - &thread->kvdo->threads[0];
    heartbeatCallback(heartbeat, id);
    enqueueKVDOThreadWorkDelayed(thread, &heartbeat->workItem,
                                 jiffies + heartbeatInterval);
  } else {
    complete(&thread->heartbeatWait);
  }
}

/**********************************************************************/
void setupHeartbeat(HeartbeatData *heartbeat)
{
  setupWorkItem(&heartbeat->workItem, kvdoHeartbeatWork, NULL,
                REQ_Q_ACTION_HEARTBEAT);
}

/**********************************************************************/
void startHeartbeat(HeartbeatData *heartbeat)
{
  heartbeat->stop = false;
  // XXX Needs to know about rest of KVDOThread.
  KVDOThread *thread = container_of(heartbeat, KVDOThread, heartbeat);
  enqueueKVDOThreadWorkDelayed(thread, &heartbeat->workItem,
                               jiffies + heartbeatInterval);
}

/**********************************************************************/
void stopHeartbeat(HeartbeatData *heartbeat)
{
  /*
   * There's currently no way to stop a work item once scheduled, so
   * set a flag for the work function to see, and wait for it to tell
   * us it's done.
   */
  heartbeat->stop = true;
}
