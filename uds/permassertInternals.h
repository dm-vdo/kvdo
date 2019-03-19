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
 * $Id: //eng/uds-releases/gloria/src/uds/permassertInternals.h#1 $
 */

#ifndef PERMASSERT_INTERNALS_H
#define PERMASSERT_INTERNALS_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

void handleAssertionFailure(const char *expressionString,
                            const char *fileName,
                            int         lineNumber,
                            const char *format,
                            va_list     args)
  __attribute__((format(printf, 4, 0)));

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PERMASSERT_INTERNALS_H */
