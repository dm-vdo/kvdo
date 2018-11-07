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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/vdoStringUtils.c#1 $
 */

#include "vdoStringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "statusCodes.h"

/**********************************************************************/
char *vAppendToBuffer(char       *buffer,
                      char       *bufEnd,
                      const char *fmt,
                      va_list     args)
{
  size_t n = vsnprintf(buffer, bufEnd - buffer, fmt, args);
  if (n >= (size_t) (bufEnd - buffer)) {
    buffer = bufEnd;
  } else {
    buffer += n;
  }
  return buffer;
}

/**********************************************************************/
char *appendToBuffer(char *buffer, char *bufEnd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  char *pos = vAppendToBuffer(buffer, bufEnd, fmt, ap);
  va_end(ap);
  return pos;
}

/**********************************************************************/
void freeStringArray(char **stringArray)
{
  for (unsigned int offset = 0; stringArray[offset] != NULL; offset++) {
    FREE(stringArray[offset]);
  }
  FREE(stringArray);
}

/**********************************************************************/
int splitString(const char *string, char separator, char ***substringArrayPtr)
{
  unsigned int substringCount = 1;
  for (const char *s = string; *s != 0; s++) {
    if (*s == separator) {
      substringCount++;
    }
  }

  char **substrings;
  int result = ALLOCATE(substringCount + 1, char *, "string-splitting array",
                        &substrings);
  if (result != UDS_SUCCESS) {
    return result;
  }
  unsigned int currentSubstring = 0;
  for (const char *s = string; *s != 0; s++) {
    if (*s == separator) {
      ptrdiff_t length = s - string;
      result = ALLOCATE(length + 1, char, "split string",
                        &substrings[currentSubstring]);
      if (result != UDS_SUCCESS) {
        freeStringArray(substrings);
        return result;
      }
      // Trailing NUL is already in place after allocation; deal with
      // the zero or more non-NUL bytes in the string.
      if (length > 0) {
        memcpy(substrings[currentSubstring], string, length);
      }
      string = s + 1;
      currentSubstring++;
      BUG_ON(currentSubstring >= substringCount);
    }
  }
  // Process final string, with no trailing separator.
  BUG_ON(currentSubstring != (substringCount - 1));
  ptrdiff_t length = strlen(string);
  result = ALLOCATE(length + 1, char, "split string",
                    &substrings[currentSubstring]);
  if (result != UDS_SUCCESS) {
    freeStringArray(substrings);
    return result;
  }
  memcpy(substrings[currentSubstring], string, length);
  currentSubstring++;
  // substrings[currentSubstring] is NULL already
  *substringArrayPtr = substrings;
  return UDS_SUCCESS;
}

/**********************************************************************/
int joinStrings(char   **substringArray,
                size_t   arrayLength,
                char     separator,
                char   **stringPtr)
{
  size_t stringLength = 0;
  for (size_t i = 0; (i < arrayLength) && (substringArray[i] != NULL); i++) {
    stringLength += strlen(substringArray[i]) + 1;
  }

  char *output;
  int result = ALLOCATE(stringLength, char, __func__, &output);
  if (result != VDO_SUCCESS) {
    return result;
  }

  char *currentPosition = &output[0];
  for (size_t i = 0; (i < arrayLength) && (substringArray[i] != NULL); i++) {
    currentPosition = appendToBuffer(currentPosition, output + stringLength,
                                     "%s", substringArray[i]);
    *currentPosition = separator;
    currentPosition++;
  }

  // We output one too many separators; replace the last with a zero byte.
  if (currentPosition != output) {
    *(currentPosition - 1) = '\0';
  }

  *stringPtr = output;
  return UDS_SUCCESS;
}

/**********************************************************************/
int stringToUInt(const char *input, unsigned int *valuePtr)
{
  unsigned long longValue;
  int result = kstrtoul(input, 10, &longValue);
  if (result != 0) {
    return result;
  }

  if (longValue > UINT_MAX) {
    return -ERANGE;
  }

  *valuePtr = longValue;
  return UDS_SUCCESS;
}
