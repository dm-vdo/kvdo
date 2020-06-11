/*
 * Copyright (c) 2020 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/memoryUsage.c#10 $
 */

#include "memoryUsage.h"

#include "memoryAlloc.h"

#include "kernelStatistics.h"

/**********************************************************************/
struct memory_usage get_memory_usage(void)
{
	struct memory_usage memory_usage;

	get_memory_stats(&memory_usage.bytes_used,
			 &memory_usage.peak_bytes_used);
	return memory_usage;
}
