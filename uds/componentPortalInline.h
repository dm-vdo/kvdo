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
 * $Id: //eng/uds-releases/flanders/src/uds/componentPortalInline.h#2 $
 */

#ifndef COMPONENT_PORTAL_INLINE_H
#define COMPONENT_PORTAL_INLINE_H

#ifndef COMPONENT_PORTAL_INLINE
# error "Must be included by componentPortal.h"
#endif

#include "compiler.h"

typedef struct componentPortalOps {
  int (*count)    (ComponentPortal *, unsigned int *);
  int (*getSize)  (ComponentPortal *, unsigned int, off_t *);
  int (*getLimit) (ComponentPortal *, unsigned int, off_t *);
  int (*getRegion)(ComponentPortal *, unsigned int, IORegion **);
  int (*getReader)(ComponentPortal *, unsigned int, BufferedReader **);
  int (*getWriter)(ComponentPortal *, unsigned int, BufferedWriter **);
} ComponentPortalOps;

struct componentPortal {
  IndexComponent           *component;
  const ComponentPortalOps *ops;
};

/*****************************************************************************/
static INLINE IndexComponent *indexComponentForPortal(ComponentPortal *portal)
{
  return portal->component;
}

/*****************************************************************************/
static INLINE int countComponents(ComponentPortal *portal, unsigned int *parts)
{
  return portal->ops->count(portal, parts);
}

/*****************************************************************************/
static INLINE int getComponentSize(ComponentPortal   *portal,
                                   unsigned int       part,
                                   off_t             *size)
{
  return portal->ops->getSize(portal, part, size);
}

/*****************************************************************************/
static INLINE int getComponentLimit(ComponentPortal   *portal,
                                    unsigned int       part,
                                    off_t             *size)
{
  return portal->ops->getLimit(portal, part, size);
}

/*****************************************************************************/
static INLINE int getIORegion(ComponentPortal  *portal,
                              unsigned int      part,
                              IORegion        **regionPtr)
{
  return portal->ops->getRegion(portal, part, regionPtr);
}

/*****************************************************************************/
static INLINE int getBufferedReader(ComponentPortal  *portal,
                                    unsigned int      part,
                                    BufferedReader  **readerPtr)
{
  return portal->ops->getReader(portal, part, readerPtr);
}

/*****************************************************************************/
static INLINE int getBufferedWriter(ComponentPortal  *portal,
                                    unsigned int      part,
                                    BufferedWriter  **writerPtr)
{
  return portal->ops->getWriter(portal, part, writerPtr);
}

#endif // COMPONENT_PORTAL_INLINE_H
