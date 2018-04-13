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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/compressionStateInternals.h#1 $
 */

#ifndef COMPRESSION_STATE_INTERNALS_H
#define COMPRESSION_STATE_INTERNALS_H

#include "compressionState.h"

/**
 * Set the compression state of a DataVIO (exposed for testing).
 *
 * @param dataVIO   The DataVIO whose compression state is to be set
 * @param state     The expected current state of the DataVIO
 * @param newState  The state to set
 *
 * @return <code>true</code> if the new state was set, false if the DataVIO's
 *         compression state did not match the expected state, and so was
 *         left unchanged
 **/
bool setCompressionState(DataVIO             *dataVIO,
                         VIOCompressionState  state,
                         VIOCompressionState  newState);

#endif /* COMPRESSION_STATE_H */
