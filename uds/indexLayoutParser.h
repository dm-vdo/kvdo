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
 * $Id: //eng/uds-releases/gloria/src/uds/indexLayoutParser.h#1 $
 */

#ifndef INDEX_LAYOUT_PARSER_H
#define INDEX_LAYOUT_PARSER_H

#include "typeDefs.h"

typedef enum {
  LP_STRING    = 0x001,
  LP_UINT64    = 0x002,
  LP_TYPE_MASK = 0x0FF,
  LP_DEFAULT   = 0x100,
} LPType;

typedef struct layoutParameter {
  const char *name;
  LPType      type;
  union {
    char     **str;
    uint64_t  *num;
  } value;
} LayoutParameter;

/**
 * Function to parse an index layout specification.
 *
 * This parser treats the specification as a set of name=value parameters
 * or, in the absence of an '=' character, a single value for a default
 * parameter. The list of acceptable parameters is specified as an array
 * of LayoutParameter entries. Each such parameter contains the address
 * of the variable in which the value is to be stored.
 *
 * @param info          A copy of the index layout specification that
 *                        will be altered by the parser to insert null
 *                        characters after each value. Note that string
 *                        parameter values will point into the memory of
 *                        this string, so this specification cannot be
 *                        deallocated until all uses of the parameter
 *                        values are over.
 * @param params        The table of parameters the caller expects to
 *                        find in the ``info'' string. Currently this
 *                        parser can handle string and uint64_t values.
 * @param count         The size of the parameter table.
 *
 * @return UDS_SUCCESS or an error code, particularly
 *      UDS_INDEX_NAME_REQUIRED for all parsing errors.
 **/
int parseLayoutString(char                  *info,
                      const LayoutParameter *params,
                      size_t                 count)
  __attribute__((warn_unused_result));

#endif // INDEX_LAYOUT_PARSER_H
