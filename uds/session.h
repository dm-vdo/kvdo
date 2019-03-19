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
 * $Id: //eng/uds-releases/gloria/src/uds/session.h#2 $
 */

/**
 * @file
 * @brief Session abstraction
 *
 * The session abstraction provides a form of thread-safe weak pointer
 * abstraction, where a session can be looked up in a session group by its
 * session ID, for a reference-counted reference to the actual session
 * structure and its associated contents, which the caller later releases when
 * done using the session contents.  A session shutdown feature removes a
 * session from its group and waits for all remaining references to be
 * released.
 **/

#ifndef SESSION_H
#define SESSION_H

#include "queue.h"
#include "threads.h"
#include "typeDefs.h"

typedef unsigned int SessionID;

enum {
  SESSION_ID_NONE = 0,
  SESSION_ID_INIT = 1
};

typedef void * SessionContents;
typedef void (*SessionFree)(SessionContents contents);

typedef struct session {
  LIST_ENTRY(session) links;   // linked list, owned by the SessionGroup

  Mutex           mutex;       // protects refCount and idleWaiters
  CondVar         releaseCond; // signalled when refCount decremented
  unsigned int    refCount;
  unsigned short  idleWaiters; // number of references from waiting threads

  SessionContents contents;
  SessionID       id;
} Session;

typedef struct sessionGroup SessionGroup;

/**
 * Looks up a session ID, and if successful, returns a reference to the
 * associated session.  A lookup is successful if the session group has a
 * session listed with the right session ID.
 *
 * @param group       Session group in which to look up the session
 * @param id          Session ID to look up
 * @param sessionPtr  Return pointer for the session on success
 *
 * @return            If the session ID was not found, the session group's
 *                    'notFoundResult' value; otherwise, UDS_SUCCESS
 **/
int getSession(SessionGroup *group, SessionID id, Session **sessionPtr)
  __attribute__((warn_unused_result));

/**
 * Returns the contents associated with the session.  NOTE: the caller must
 * hold a reference to the session.
 *
 * @param session  Session
 * @return         Contents
 **/
SessionContents getSessionContents(Session *session)
  __attribute__((warn_unused_result));

/**
 * Releases a reference to the session.  NOTE: the caller must hold a reference
 * to the session, and must not use the session pointer after this call.
 *
 * @param session  Session to release.
 **/
void releaseSession(Session *session);

/**
 * Wait until the session is idle.
 *
 * This will return when the only references to the session are from threads
 * that are calling either waitForIdleSession() or finishSession().
 **/
void waitForIdleSession(Session *session);

/**
 * Initializes a session, assigning it an ID and adding it to a session group.
 * NOTE: the caller holds a reference to the session upon a successful return.
 *
 * @param group      Session group in which to create the session
 * @param session    Caller-allocated session structure to initialize
 * @param contents   Caller contents to associate with the session (may be a
 *                   structure that contains the session structure itself)
 * @param sessionID  Session ID for the session initialized is stored here
 *
 * @return UDS_SUCCESS or an error code
 **/
int initializeSession(SessionGroup    *group,
                      Session         *session,
                      SessionContents  contents,
                      SessionID       *sessionID)
  __attribute__((warn_unused_result));

/**
 * Acquire a reference to a session group.
 *
 * @param group  the session group to reference
 *
 * @return UDS_SUCCESS or an error code
 **/
int acquireSessionGroup(SessionGroup *group)
  __attribute__((warn_unused_result));

/**
 * Release a reference to a session group.
 *
 * @param group           the session group to release
 **/
void releaseSessionGroup(SessionGroup *group);

/**
 * Removes a session from a session group, preventing further lookups, and
 * waits until all other references to it are released.  NOTE: caller must hold
 * a reference to the session when calling this method (as implied by the fact
 * that the caller has a direct pointer to it).
 *
 * @param group    Session group from which to remove the session
 * @param session  Session to finish
 **/
void finishSession(SessionGroup *group, Session *session);

/**
 * Allocate and initialize a session group, and acquire a reference to it.
 *
 * @param notFoundResult  the return value for an unsuccessful lookup
 * @param free            function to call to free a session's resources on a
 *                        forced shutdown
 * @param groupPtr        a pointer to hold the new session group
 *
 * @return                UDS_SUCCESS or an error code
 **/
int makeSessionGroup(int notFoundResult, SessionFree free,
                     SessionGroup  **groupPtr)
  __attribute__((warn_unused_result));


/**
 * Shut down a session group.  Prevents further session lookups, then waits
 * until all other references to all sessions are released.
 *
 * @param group           session group to shut down
 **/
void shutdownSessionGroup(SessionGroup *group);

#endif /* SESSION_H */
