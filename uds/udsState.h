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
 * $Id: //eng/uds-releases/jasper/src/uds/udsState.h#1 $
 */

#ifndef UDS_STATE_H
#define UDS_STATE_H

#include "session.h"

/**
 * Initialize the library if has not already been done.
 **/
void udsInitialize(void);

/**
 * Lock the global state mutex
 **/
void lockGlobalStateMutex(void);

/**
 * Unlock the global state mutex
 **/
void unlockGlobalStateMutex(void);

/**
 * Ensure that the library is ready to create sessions.
 *
 * @return              UDS_SUCCESS or error code
 **/
int checkLibraryRunning(void);

/**
 * Return the SessionGroup that dispenses IndexSessions.
 *
 * @return udsState.indexSessions
 **/
SessionGroup *getIndexSessionGroup(void);

#endif /* UDS_State_H */
