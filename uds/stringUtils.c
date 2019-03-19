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
 * $Id: //eng/uds-releases/gloria/src/uds/stringUtils.c#2 $
 */

#include "stringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "uds.h"

/*****************************************************************************/
int allocSprintf(const char *what, char **strp, const char *fmt, ...)
{
  if (strp == NULL) {
    return UDS_INVALID_ARGUMENT;
  }
  va_list args;
  va_start(args, fmt);
  int result = doPlatformVasprintf(what, strp, fmt, args);
  va_end(args);
  if ((result != UDS_SUCCESS) && (what != NULL)) {
    logError("cannot allocate %s", what);
  }
  return result;
}

/*****************************************************************************/
int wrapVsnprintf(const char *what, char *buf, size_t bufSize,
                  int error, const char *fmt, va_list ap, size_t *needed)
{
  if (buf == NULL) {
    static char nobuf[1];
    buf = nobuf;
    bufSize = 0;
  }
  int n = vsnprintf(buf, bufSize, fmt, ap);
  if (n < 0) {
    return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                   "%s: vsnprintf failed", what);
  }
  if (needed) {
    *needed = n;
  }
  if (((size_t) n >= bufSize) && (buf != NULL) && (error != UDS_SUCCESS)) {
    return logErrorWithStringError(error, "%s: string too long", what);
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int fixedSprintf(const char *what,
                 char       *buf,
                 size_t      bufSize,
                 int         error,
                 const char *fmt,
                 ...)
{
  if (buf == NULL) {
    return UDS_INVALID_ARGUMENT;
  }
  va_list args;
  va_start(args, fmt);
  int result = wrapVsnprintf(what, buf, bufSize, error, fmt, args, NULL);
  va_end(args);
  return result;
}

/*****************************************************************************/
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

/*****************************************************************************/
char *appendToBuffer(char *buffer, char *bufEnd, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  char *pos = vAppendToBuffer(buffer, bufEnd, fmt, ap);
  va_end(ap);
  return pos;
}

/*****************************************************************************/
int stringToSignedInt(const char *nptr, int *num)
{
  long value;
  int result = stringToSignedLong(nptr, &value);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if ((value < INT_MIN) || (value > INT_MAX)) {
    return ERANGE;
  }
  *num = (int) value;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int stringToUnsignedInt(const char *nptr, unsigned int *num)
{
  unsigned long value;
  int result = stringToUnsignedLong(nptr, &value);
  if (result != UDS_SUCCESS) {
    return result;
  }
  if (value > UINT_MAX) {
    return ERANGE;
  }
  *num = (unsigned int) value;
  return UDS_SUCCESS;
}
