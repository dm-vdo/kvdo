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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/readOnlyModeContext.h#1 $
 */

#ifndef READ_ONLY_MODE_CONTEXT_H
#define READ_ONLY_MODE_CONTEXT_H

#include "types.h"

/**
 * Check whether a read-only mode context has been put in read-only mode.
 *
 * @param readOnlyContext  The context to check
 *
 * @return <code>true</code> if the context is in read-only mode
 **/
bool isReadOnly(ReadOnlyModeContext *readOnlyContext)
  __attribute__((warn_unused_result));

/**
 * Put a read-only mode context into read-only mode (has no effect if the
 * context is already in read-only mode).
 *
 * @param readOnlyContext  The context to put into read-only mode
 * @param errorCode        The error result triggering this call
 **/
void enterReadOnlyMode(ReadOnlyModeContext *readOnlyContext, int errorCode);

#endif /* READ_ONLY_MODE_CONTEXT_H */
