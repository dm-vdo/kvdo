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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/readOnlyNotifierInternals.h#1 $
 */

#ifndef READ_ONLY_NOTIFIER_INTERNALS_H
#define READ_ONLY_NOTIFIER_INTERNALS_H

#include "completion.h"

/**
 * An object to be notified when the VDO enters read-only mode
 **/
typedef struct readOnlyListener ReadOnlyListener;

struct readOnlyListener {
  /** The listener */
  void                 *listener;
  /** The method to call to notifiy the listener */
  ReadOnlyNotification *notify;
  /** A pointer to the next listener */
  ReadOnlyListener     *next;
};

/**
 * Data associated with each base code thread.
 **/
typedef struct threadData {
  /** The completion for entering read-only mode */
  VDOCompletion          completion;
  /** The ReadOnlyNotifier to which this structure belongs */
  ReadOnlyNotifier      *notifier;
  /** The thread this represents */
  ThreadID               threadID;
  /** Whether this thread is in read-only mode */
  bool                   isReadOnly;
  /** Whether this thread is entering read-only mode */
  bool                   isEnteringReadOnlyMode;
  /** Whether this thread may enter read-only mode */
  bool                   mayEnterReadOnlyMode;
  /** The error code for entering read-only mode */
  int                    readOnlyError;
  /** A completion to notify when this thread is not entering read-only mode */
  VDOCompletion         *superBlockIdleWaiter;
  /** A completion which is waiting to enter read-only mode */
  VDOCompletion         *readOnlyModeWaiter;
  /**
   * A list of objects waiting to be notified on this thread that the VDO has
   * entered read-only mode.
   **/
  ReadOnlyListener      *listeners;
} ThreadData;

struct readOnlyNotifier {
  /** The completion for waiting until not entering read-only mode */
  VDOCompletion       completion;
  /** Whether the listener has already started a notification */
  bool                isReadOnly;
  /** The thread config of the VDO */
  const ThreadConfig *threadConfig;
  /** The ID of the admin thread */
  ThreadID            adminThreadID;
  /** The array of per-thread data */
  ThreadData          threadData[];
};

#endif /* READ_ONLY_NOTIFIER_INTERNALS_H */
