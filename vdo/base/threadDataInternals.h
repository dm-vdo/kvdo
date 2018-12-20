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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/threadDataInternals.h#2 $
 */

#ifndef THREAD_DATA_INTERNALS_H
#define THREAD_DATA_INTERNALS_H

#include "completion.h"

/**
 * Data associated with each base code thread.
 **/
struct threadData {
  /** The completion for entering read-only mode */
  VDOCompletion          completion;
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
};

#endif /* THREAD_DATA_INTERNALS_H */
