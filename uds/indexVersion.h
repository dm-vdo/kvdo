/*
 * Copyright Red Hat
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
 * $Id: //eng/uds-releases/jasper/src/uds/indexVersion.h#1 $
 */

#ifndef INDEX_VERSION_H
#define INDEX_VERSION_H

#include "typeDefs.h"

struct index_version {
  bool chapterIndexHeaderNativeEndian;
};

enum {
  SUPER_VERSION_MINIMUM = 1,
  SUPER_VERSION_MAXIMUM = 3,
  SUPER_VERSION_CURRENT = 3,
};

/**
 * Initialize the version parameters that we normally learn when loading the
 * index but need to use during index operation.
 *
 * @param version       The version parameters
 * @param superVersion  The SuperBlock version number
 **/
void initializeIndexVersion(struct index_version *version,
                            uint32_t              superVersion);

#endif // INDEX_VERSION_H
