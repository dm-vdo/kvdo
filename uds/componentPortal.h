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
 * $Id: //eng/uds-releases/flanders-rhel7.5/src/uds/componentPortal.h#1 $
 */

#ifndef COMPONENT_PORTAL_H
#define COMPONENT_PORTAL_H

#include "bufferedReader.h"
#include "bufferedWriter.h"

typedef struct componentPortal ComponentPortal;
typedef struct indexComponent  IndexComponent;

/**
 * Return the index component associated with this component portal.
 *
 * @param portal        The component portal.
 *
 * @return              The index component.
 **/
static IndexComponent *indexComponentForPortal(ComponentPortal *portal);

/**
 * Count the number of parts for this portal.
 *
 * @param [in]  portal          The component portal.
 * @param [out] parts           The number of parts.
 *
 * @return UDS_SUCCESS or an error code.
 **/
static int countComponents(ComponentPortal *portal, unsigned int *parts)
  __attribute__((warn_unused_result));

/**
 * Get the size of the saved component part image.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] size            The size of the component image.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note This is only supported by some types of portals and only if the
 *       component was previously saved.
 **/
static int getComponentSize(ComponentPortal *portal,
                            unsigned int     part,
                            off_t           *size)
  __attribute__((warn_unused_result));

/**
 * Get the limit of the component's storage area. May be unlimited.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] limit           The space available to store the component.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note File-based portals have no limit.
 **/
static int getComponentLimit(ComponentPortal *portal,
                             unsigned int     part,
                             off_t           *limit)
  __attribute__((warn_unused_result));

/**
 * Return the IORegion for the specified component part.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] regionPtr       An IORegion for reading or writing
 *                                the specified component instance.
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note the region is managed by the component portal
 **/
static int getIORegion(ComponentPortal  *portal,
                       unsigned int      part,
                       IORegion        **regionPtr)
  __attribute__((warn_unused_result));

/**
 * Get a buffered reader for the specified component part.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] readerPtr       Where to put the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note the reader is managed by the component portal
 **/
static int getBufferedReader(ComponentPortal  *portal,
                             unsigned int      part,
                             BufferedReader  **readerPtr)
  __attribute__((warn_unused_result));

/**
 * Get a buffered writer for the specified component part.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] writerPtr       Where to put the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note the writer is managed by the component portal
 **/
static int getBufferedWriter(ComponentPortal  *portal,
                             unsigned int      part,
                             BufferedWriter  **writerPtr)
  __attribute__((warn_unused_result));

// Dull, boring implementations details below...

#define COMPONENT_PORTAL_INLINE
#include "componentPortalInline.h"
#undef COMPONENT_PORTAL_INLINE

#endif // COMPONENT_PORTAL_H
