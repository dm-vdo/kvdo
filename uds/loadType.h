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
 * $Id: //eng/uds-releases/krusty/src/uds/loadType.h#4 $
 */

#ifndef LOAD_TYPE_H
#define LOAD_TYPE_H

/**
 * Methods of starting the index.  (Keep get_load_type() in sync.)
 *
 * Usage number 1 is to note the interface method that initiates loading the
 * index.  As in this table:
 *
 *    name            type    opened by
 *    ===========     ======  ====================
 *    LOAD_CREATE     local   udsCreateLocalIndex
 *    LOAD_LOAD       local   udsLoadLocalIndex
 *    LOAD_REBUILD    local   udsRebuildLocalIndex
 *
 * Usage number 2 is to record how an index was really opened.  As in this
 * table:
 *
 *    LOAD_CREATE   new empty index
 *    LOAD_LOAD     loaded saved index
 *    LOAD_REPLAY   loaded checkpoint and replayed new chapters
 *    LOAD_EMPTY    empty volume index from empty volume data
 *    LOAD_REBUILD  rebuilt volume index from volume data
 **/
enum load_type {
	LOAD_UNDEFINED = 0,
	LOAD_CREATE,
	LOAD_LOAD,
	LOAD_REBUILD,
	LOAD_EMPTY,
	LOAD_REPLAY,
};

/**
 * get a string indicating how an index is to be loaded.
 *
 * @param load_type   The load type to log
 **/
const char *get_load_type(enum load_type load_type);

#endif /* LOAD_TYPE_H */
