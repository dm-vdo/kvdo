/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/src/uds/loadType.c#2 $
 */

#include "loadType.h"

#include "logger.h"

/**********************************************************************/
void logLoadType(LoadType loadType)
{
  switch (loadType) {
  case LOAD_CREATE:
    logNotice("Creating new index.");
    break;

  case LOAD_LOAD:
    logNotice("Loading index from saved state.");
    break;

  case LOAD_REBUILD:
    logNotice("Loading index from saved state and rebuilding if necessary.");
    break;

  case LOAD_ATTACH:
    logNotice("Attaching to a remote index.");
    break;

  default:
    logNotice("No load method specified.");
  }
}
