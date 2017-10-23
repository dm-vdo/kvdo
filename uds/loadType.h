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
 * $Id: //eng/uds-releases/flanders/src/uds/loadType.h#2 $
 */

#ifndef LOAD_TYPE_H
#define LOAD_TYPE_H

/**
 * Methods of starting the index.
 * Generally, higher valued flags take precedence over lower ones.
 * (Keep logLoadType() in sync.)
 **/
typedef enum {
  LOAD_UNDEFINED = 0,
  LOAD_CREATE,
  LOAD_LOAD,
  LOAD_REBUILD,
  LOAD_ATTACH
} LoadType;

/**
 * Log a message indicating how an index is to be loaded.
 *
 * @param loadType    The load type to log
 **/
void logLoadType(LoadType loadType);

#endif /* LOAD_TYPE_H */
