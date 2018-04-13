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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/readOnlyModeContextInternals.h#1 $
 */

#ifndef READ_ONLY_MODE_CONTEXT_INTERNALS_H
#define READ_ONLY_MODE_CONTEXT_INTERNALS_H

#include "readOnlyModeContext.h"

/**
 * A function to query whether a context is in read-only mode.
 *
 * @param context  The context to query
 *
 * @return <code>true</code> if the context is in read-only mode
 **/
typedef bool ReadOnlyModeQuery(void *context);

/**
 * A function for entering read-only mode.
 *
 * @param context    The context to put in read-only mode
 * @param errorCode  The error result triggering this call
 **/
typedef void ReadOnlyModeEnterer(void *context, int errorCode);

struct readOnlyModeContext {
  void                *context;
  ReadOnlyModeQuery   *isReadOnly;
  ReadOnlyModeEnterer *enterReadOnlyMode;
};

#endif /* READ_ONLY_MODE_CONTEXT_INTERNALS_H */
