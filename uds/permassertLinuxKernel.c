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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/permassertLinuxKernel.c#3 $
 */

#include "logger.h"
#include "permassert.h"
#include "permassertInternals.h"

/**********************************************************************/
__attribute__((format(printf, 4, 0)))
void handleAssertionFailure(const char *expressionString,
                            const char *fileName,
                            int         lineNumber,
                            const char *format,
                            va_list     args)
{
  logEmbeddedMessage(LOG_ERR, "assertion \"", format, args,
                     "\" (%s) failed at %s:%d",
                     expressionString, fileName, lineNumber);
  logBacktrace(LOG_ERR);
}
