/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/messageStats.h#1 $
 *
 */

#ifndef MESSAGE_STATS_H
#define MESSAGE_STATS_H

#include "kernelLayer.h"

/**
 * Write kernel statistics to a buffer
 *
 * @param layer    the kernel layer
 * @param buffer   pointer to the buffer
 * @param maxlen   the maximum length of the buffer
 */
int write_kernel_statistics(struct kernel_layer *layer,
			    char *buf,
			    unsigned int maxlen);

/**
 * Write vdo statistics to a buffer
 *
 * @param layer    the kernel layer
 * @param buffer   pointer to the buffer
 * @param maxlen   the maximum length of the buffer
 */
int write_vdo_statistics(struct kernel_layer *layer,
			 char *buf,
			 unsigned int maxlen);

#endif  /* MESSAGE_STATS_H */
