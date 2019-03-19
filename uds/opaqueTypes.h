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
 * $Id: //eng/uds-releases/gloria/src/uds/opaqueTypes.h#1 $
 */

#ifndef OPAQUE_TYPES_H
#define OPAQUE_TYPES_H

/*
 * This file contains typedefs of structures internal to the UDS library
 * for which many users of those structures do need to know the details
 * of the structures themselves.
 */
typedef struct aipContext              AIPContext;
typedef struct grid                    Grid;
typedef struct indexRouter             IndexRouter;
typedef struct indexRouterStatCounters IndexRouterStatCounters;
typedef struct indexSession            IndexSession;
typedef struct serverConnection        ServerConnection;
typedef struct request                 Request;
typedef struct requestQueue            RequestQueue;

#endif /* OPAQUE_TYPES_H */
