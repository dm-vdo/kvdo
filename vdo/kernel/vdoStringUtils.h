/*
 * Copyright Red Hat
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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/kernel/vdoStringUtils.h#1 $
 */

#ifndef VDO_STRING_UTILS_H
#define VDO_STRING_UTILS_H

#include <stdarg.h>
#include <linux/types.h>

/**
 * Helper to append a string to a buffer.
 *
 * @param buffer  the place at which to append the string
 * @param bufEnd  pointer to the end of the buffer
 * @param fmt     a printf format string
 *
 * @return  the updated buffer position after the append
 *
 * if insufficient space is available, the contents are silently truncated
 **/
char *appendToBuffer(char *buffer, char *bufEnd, const char *fmt, ...);

/**
 * Variable-arglist helper to append a string to a buffer.
 * If insufficient space is available, the contents are silently truncated.
 *
 * @param buffer  the place at which to append the string
 * @param bufEnd  pointer to the end of the buffer
 * @param fmt     a printf format string
 * @param args    printf arguments
 *
 * @return  the updated buffer position after the append
 **/
char *vAppendToBuffer(char       *buffer,
                      char       *bufEnd,
                      const char *fmt,
                      va_list     args);

/**
 * Split the input string into substrings, separated at occurrences of
 * the indicated character, returning a null-terminated list of string
 * pointers.
 *
 * The string pointers and the pointer array itself should both be
 * freed with FREE() when no longer needed. This can be done with
 * freeStringArray (below) if the pointers in the array are not
 * changed. Since the array and copied strings are allocated by this
 * function, it may only be used in contexts where allocation is
 * permitted.
 *
 * Empty substrings are not ignored; that is, returned substrings may
 * be empty strings if the separator occurs twice in a row.
 *
 * @param [in]  string             The input string to be broken apart
 * @param [in]  separator          The separator character
 * @param [out] substringArrayPtr  The NULL-terminated substring array
 *
 * @return  UDS_SUCCESS or -ENOMEM
 **/
int splitString(const char *string, char separator, char ***substringArrayPtr)
  __attribute__((warn_unused_result));

/**
 * Join the input substrings into one string, joined with the indicated
 * character, returning a string.
 *
 * @param [in]  substringArray  The NULL-terminated substring array
 * @param [in]  arrayLength     A bound on the number of valid elements
 *                              in substringArray, in case it is not
 *                              NULL-terminated.
 * @param [in]  separator       The separator character
 * @param [out] stringPtr       A pointer to hold the joined string
 *
 * @return  VDO_SUCCESS or an error
 **/
int joinStrings(char   **substringArray,
                size_t   arrayLength,
                char     separator,
                char   **stringPtr)
  __attribute__((warn_unused_result));

/**
 * Free a list of non-NULL string pointers, and then the list itself.
 *
 * @param stringArray  The string list
 **/
void freeStringArray(char **stringArray);

/**
 * Parse a string as an "unsigned int" value, yielding the value.
 * On overflow, -ERANGE is returned. On invalid number, -EINVAL is
 * returned.
 *
 * @param [in]  input     The string to be processed
 * @param [out] valuePtr  The value of the number read
 *
 * @return  UDS_SUCCESS or -EINVAL or -ERANGE.
 **/
int stringToUInt(const char *input, unsigned int *valuePtr)
  __attribute__((warn_unused_result));

#endif /* VDO_STRING_UTILS_H */
