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
 * $Id: //eng/vdo-releases/magnesium-rhel7.5/src/c++/vdo/kernel/heartbeat.h#1 $
 */

#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "workQueue.h"

typedef struct heartbeatData {
  bool         stop;
  KvdoWorkItem workItem;
} HeartbeatData;

/**
 * Figures out the heartbeat rate we're able to use based on how this
 * system is configured.  If our timing is going to be off because of
 * jiffy granularity, issues one warning at load time.
 **/
void initializeHeartbeatRate(void);

/**
 * Set up data structures for the heartbeat.
 *
 * @param heartbeat   The heartbeat data
 **/
void setupHeartbeat(HeartbeatData *heartbeat);

/**
 * Actually start the heart beating.
 *
 * @param heartbeat   The heartbeat data
 **/
void startHeartbeat(HeartbeatData *heartbeat);

/**
 * Cancel heartbeat timer.
 *
 * Must be called from the request-queue thread.
 *
 * @param heartbeat   The heartbeat data
 **/
void stopHeartbeat(HeartbeatData *heartbeat);

/**
 * Heartbeat callback function.
 *
 * The heartbeat code CALLS OUT to this function (does not provide it)
 * when the heartbeat fires.
 *
 * @param heartbeat   The heartbeat data
 * @param thread      The id number of the current thread
 **/
void heartbeatCallback(HeartbeatData *heartbeat, ThreadID thread);

#endif // HEARTBEAT_H
