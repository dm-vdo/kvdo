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
 * $Id: //eng/uds-releases/homer/kernelLinux/uds/assertDefs.h#1 $
 */

#ifndef LINUX_KERNEL_ASSERT_DEFS_H
#define LINUX_KERNEL_ASSERT_DEFS_H

#include "compiler.h"
#include "logger.h"
#include "threads.h"

#define CU_COMPLAIN(PRED) \
  logError("\n%s:%d: %s: %s: ", __FILE__, __LINE__, __func__, PRED)

#define CU_MESSAGE(...) \
  logError(__VA_ARGS__)

/**
 * Print the error message for a system error
 *
 * @param string  The stringized value passed to the CU assertion macro
 * @param value   The integer error code resulting from the stringized code
 **/
void cuErrorMessage(const char *string, int value);

/**
 * An assertion has triggered, so try to die cleanly
 **/
__attribute__((noreturn))
static INLINE void cuDie(void)
{
  exitThread();
}

#endif /* LINUX_KERNEL_ASSERT_DEFS_H */
