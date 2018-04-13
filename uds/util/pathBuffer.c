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
 * $Id: //eng/uds-releases/gloria/src/uds/util/pathBuffer.c#1 $
 */

#include "pathBuffer.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "stringUtils.h"
#include "uds.h"

static const size_t DEFAULT_PATH_BUFFER_SIZE = 128;
static const size_t PATH_BUFFER_QUANTUM      =  64;

/******************************************************************************/
void zeroPathBuffer(PathBuffer *pb)
{
  pb->path = NULL;
  pb->end  = NULL;
  pb->size = 0;
}

/******************************************************************************/
int initializePathBuffer(PathBuffer *pb, size_t size)
{
  int result = ASSERT_WITH_ERROR_CODE(size > 0, UDS_INVALID_ARGUMENT,
                                      "size must be greater than zero");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ALLOCATE(size, char, "path buffer", &pb->path);
  if (result != UDS_SUCCESS) {
    return result;
  }
  pb->size = size;
  pb->end = pb->path;
  *pb->end = '\0';

  return UDS_SUCCESS;
}

/******************************************************************************/
static size_t roundToNext(size_t size, size_t quantum)
{
  size_t temp = size + quantum - 1;
  return temp - temp % quantum;
}

/******************************************************************************/
int initializePathBufferCopy(PathBuffer *pb, const PathBuffer *other)
{
  size_t len = pathBufferLength(other);
  size_t size = roundToNext(len + 1, PATH_BUFFER_QUANTUM);

  int result = initializePathBuffer(pb, size);
  if (result != UDS_SUCCESS) {
    return result;
  }

  memcpy(pb->path, other->path, len);
  pb->end = pb->path + len;
  *pb->end = '\0';

  return UDS_SUCCESS;
}

/******************************************************************************/
int initializePathBufferSprintf(PathBuffer *pb, const char *fmt, ...)
{
  va_list args;

  char buf[DEFAULT_PATH_BUFFER_SIZE];

  size_t needed = 0;
  va_start(args, fmt);
  int result = wrapVsnprintf("path buffer", buf, sizeof(buf), UDS_SUCCESS,
                             fmt, args, &needed);
  va_end(args);
  if (result != UDS_SUCCESS) {
    return result;
  }

  size_t size = roundToNext(needed + 1, PATH_BUFFER_QUANTUM);

  result = initializePathBuffer(pb, size);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (needed < sizeof(buf)) {
    // fit in original buffer, just copy it
    memcpy(pb->path, buf, needed);
    pb->end = pb->path + needed;
    *pb->end = '\0';
  } else {
    // didn't fit, have to do the vsnprintf over again
    va_start(args, fmt);
    result = wrapVsnprintf("path buffer", pb->path, pb->size, UDS_BAD_STATE,
                           fmt, args, NULL);
    va_end(args);
    if (result != UDS_SUCCESS) {
      pb->end = pb->path;
      return result;
    }
    pb->end = pb->path + needed;
  }
  return UDS_SUCCESS;
}

/******************************************************************************/
static int growPathBufferIfNeeded(PathBuffer *pb, size_t size, bool exactly)
{
  if (size < pb->size) {
    return UDS_SUCCESS;
  }
  if (!exactly) {
    size = roundToNext(size, PATH_BUFFER_QUANTUM);
  }

  char *buf = NULL;
  int result = reallocateMemory(pb->path, pb->size, size, "path buffer", &buf);
  if (result != UDS_SUCCESS) {
    return result;
  }

  pb->end = buf + (pb->end - pb->path);
  pb->path = buf;
  pb->size = size;

  return UDS_SUCCESS;
}

/******************************************************************************/
__attribute__((format(printf, 3, 0) ))
static int pathBufferSprintHelper(PathBuffer *pb, size_t offset,
                                  const char *fmt, va_list ap1, va_list ap2,
                                  bool exact)
{
  size_t needed = 0;

  int result = wrapVsnprintf("path buffer",
                             pb->path + offset,
                             pb->size - offset,
                             UDS_SUCCESS,
                             fmt, ap1, &needed);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (needed + offset >= pb->size) {
    result = growPathBufferIfNeeded(pb, offset + needed + 1, exact);
    if (result != UDS_SUCCESS) {
      return result;
    }

    // didn't fit, have to do the vsnprintf over again
    result = wrapVsnprintf("path buffer",
                           pb->path + offset,
                           pb->size - offset,
                           UDS_BAD_STATE,
                           fmt, ap2, NULL);
    if (result != UDS_SUCCESS) {
      pb->end = pb->path;
      return result;
    }
  }
  pb->end = pb->path + offset + needed;
  return UDS_SUCCESS;
}

/******************************************************************************/
int initializePathBufferSizedSprintf(PathBuffer *pb, size_t size,
                                     const char *fmt, ...)
{
  int result = initializePathBuffer(pb, size);
  if (result != UDS_SUCCESS) {
    return result;
  }

  va_list args;
  va_list args2;
  va_start(args, fmt);
  va_copy(args2, args);
  result = pathBufferSprintHelper(pb, 0, fmt, args, args2, true);
  va_end(args);
  va_end(args2);
  return result;
}

/******************************************************************************/
int initializePathBufferExactFit(PathBuffer *pb, char *data, size_t len)
{
  int result = initializePathBuffer(pb, len + 1);
  if (result != UDS_SUCCESS) {
    return result;
  }
  memcpy(pb->path, data, len);
  pb->end = pb->path + len;
  *pb->end = '\0';
  return UDS_SUCCESS;
}

/******************************************************************************/
void releasePathBuffer(PathBuffer *pb)
{
  if (pb != NULL) {
    if (pb->path != NULL) {
      FREE(pb->path);
    }
    zeroPathBuffer(pb);
  }
}

/******************************************************************************/
bool pathBufferHasPath(const PathBuffer *pb)
{
  return (pb->path != NULL) && (pb->end > pb->path);
}

/******************************************************************************/
size_t pathBufferLength(const PathBuffer *pb)
{
  return pb->path == NULL ? 0 : pb->end - pb->path;
}

/******************************************************************************/
size_t pathBufferSize(const PathBuffer *pb)
{
  return pb->size;
}

/******************************************************************************/
const char *pathBufferPath(const PathBuffer *pb)
{
  return pb->path;
}

/******************************************************************************/
char *pathBufferBuffer(const PathBuffer *pb)
{
  return pb->path;
}

/******************************************************************************/
int setPathBufferSprintf(PathBuffer *pb, char *fmt, ...)
{
  va_list args;
  va_list args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int result = pathBufferSprintHelper(pb, 0, fmt, args, args2, false);
  va_end(args);
  va_end(args2);
  return result;
}

/******************************************************************************/
int copyPathBuffer(PathBuffer *dst, const PathBuffer *src)
{
  size_t slen = pathBufferLength(src);
  if (dst->size < slen + 1) {
    int result = growPathBufferIfNeeded(dst, slen + 1, false);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  memcpy(dst->path, src->path, slen);
  dst->end = dst->path + slen;
  *dst->end = '\0';
  return UDS_SUCCESS;
}

/******************************************************************************/
int truncatePathBuffer(PathBuffer *pb, size_t len)
{
  if (len <= pathBufferLength(pb)) {
    pb->end = pb->path + len;
    *pb->end = '\0';
    return UDS_SUCCESS;
  }
  return UDS_INVALID_ARGUMENT;
}

/******************************************************************************/
int appendPathBuffer(PathBuffer *dst, const PathBuffer *src)
{
  size_t dlen = pathBufferLength(dst);
  size_t slen = pathBufferLength(src);
  if (dst->size < dlen + slen + 1) {
    int result = growPathBufferIfNeeded(dst, dlen + slen + 1, false);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  memcpy(dst->path + dlen, src->path, slen);
  dst->end = dst->path + dlen + slen;
  *dst->end = '\0';
  return UDS_SUCCESS;
}

/******************************************************************************/
int appendPathBufferSprintf(PathBuffer *pb, const char *fmt, ...)
{
  va_list args;
  va_list args2;
  va_start(args, fmt);
  va_copy(args2, args);
  int result = pathBufferSprintHelper(pb, pathBufferLength(pb),
                                      fmt, args, args2, false);
  va_end(args);
  va_end(args2);
  return result;
}

/******************************************************************************/
int setPathBufferSize(PathBuffer *pb, size_t size)
{
  if (size < pathBufferLength(pb) + 1) {
    return UDS_INVALID_ARGUMENT;
  }

  char *buf = NULL;
  int result = reallocateMemory(pb->path, pb->size, size, "path buffer", &buf);
  if (result != UDS_SUCCESS) {
    return result;
  }

  pb->end = buf + (pb->end - pb->path);
  pb->path = buf;
  pb->size = size;
  return UDS_SUCCESS;
}

/******************************************************************************/
int setPathBufferSizeToFit(PathBuffer *pb)
{
  return setPathBufferSize(pb, pathBufferLength(pb) + 1);
}
