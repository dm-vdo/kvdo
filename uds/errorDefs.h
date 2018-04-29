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
 * $Id: //eng/uds-releases/flanders-rhel7.5/kernelLinux/uds/errorDefs.h#1 $
 */

#ifndef LINUX_KERNEL_ERROR_DEFS_H
#define LINUX_KERNEL_ERROR_DEFS_H

#include "typeDefs.h"

/**
 * Return string describing a system error number
 *
 * @param errnum  System error number
 * @param buf     Buffer that can be used to contain the return value
 * @param buflen  Length of the buffer
 *
 * @return The error string, which may be a string constant or may be
 *         returned in the buf argument
 **/
const char *systemStringError(int errnum, char *buf, size_t buflen);

#endif /* LINUX_KERNEL_ERROR_DEFS_H */
