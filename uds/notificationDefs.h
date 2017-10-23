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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/notificationDefs.h#2 $
 */

#ifndef LINUX_KERNEL_NOTIFICATION_DEFS_H
#define LINUX_KERNEL_NOTIFICATION_DEFS_H

#include "uds.h"
#include "uds-block.h"

/**
 * Called when a block context is closed.
 *
 * @param context  the block context
 **/
void notifyBlockContextClosed(UdsBlockContext context);

/**
 * Called when a block context is opened.
 *
 * @param session  the index session
 * @param context  the block context
 **/
void notifyBlockContextOpened(UdsIndexSession session,
                              UdsBlockContext context);

/**
 * Called when an index session is closed.
 *
 * @param session  the index session
 **/
void notifyIndexClosed(UdsIndexSession session);

/**
 * Called when an index session is opened.
 *
 * @param session  the index session
 * @param name     the index name
 **/
void notifyIndexOpened(UdsIndexSession session, const char *name);

#endif /*  LINUX_KERNEL_NOTIFICATION_DEFS_H */
