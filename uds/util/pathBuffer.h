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
 * $Id: //eng/uds-releases/gloria/src/uds/util/pathBuffer.h#1 $
 */

#ifndef PATH_BUFFER_H
#define PATH_BUFFER_H 1

#include "typeDefs.h"

/**
 * A path buffer is designed to hold a path string and maybe additional space
 * to allow concatenation or modification without necessarily causing
 * reallocation.
 **/
typedef struct pathBuffer {
  char   *path;// begining of memory
  char   *end; // end of string contents: *end == 0; path + strlen(path) == end
  size_t  size;// size of memory allocated: end < path + size
} PathBuffer;

/**
 * Zero-initializes a path buffer without allocating.
 *
 * @param pb   the path buffer
 **/
void zeroPathBuffer(PathBuffer *pb);

/**
 * Initialize path buffer to be an empty string with reserved size.
 *
 * @param pb   the path buffer
 * @param size the amount of space to reserve
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int initializePathBuffer(PathBuffer *pb, size_t size)
  __attribute__((warn_unused_result));

/**
 * Initialize path buffer to be a copy of another.
 *
 * @param pb    the path buffer to initialize
 * @param other the path buffer to copy
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int initializePathBufferCopy(PathBuffer *pb, const PathBuffer *other)
  __attribute__((warn_unused_result));

/**
 * Initialize path buffer with contents of printf args.
 *
 * @param pb   the path buffer
 * @param fmt  an sprintf format parameter
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int initializePathBufferSprintf(PathBuffer *pb, const char *fmt, ...)
  __attribute__((format(printf, 2, 3), warn_unused_result));

/**
 * Initialize path buffer with contents of printf args, with minimum size.
 *
 * @param pb   the path buffer
 * @param size the minimum allocates size (may be more if sprintf requires)
 * @param fmt  an sprintf format parameter
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int initializePathBufferSizedSprintf(PathBuffer *pb, size_t size,
                                     const char *fmt, ...)
  __attribute__((format(printf, 3, 4), warn_unused_result));

/**
 * Initialize path buffer with exact contents of data.
 * Size will be len + 1.  Permits embedded-nulls.
 *
 * @param pb   the path buffer
 * @param data the beginning of some character data
 * @param len  the end of the character data
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int initializePathBufferExactFit(PathBuffer *pb, char *data, size_t len)
  __attribute__((warn_unused_result));

/**
 * Release any memory used by path buffer and resets it to zero.
 *
 * @param pb   the path buffer
 **/
void releasePathBuffer(PathBuffer *pb);

/**
 * Determine if the path buffer has a non-null path present.
 *
 * @param pb   the path buffer
 *
 * @return true if length not 0
 **/
bool pathBufferHasPath(const PathBuffer *pb)
  __attribute__((warn_unused_result));

/**
 * Return the length of the string currently in the path buffer.
 *
 * @param pb   the path buffer
 *
 * @return     end - start
 **/
size_t pathBufferLength(const PathBuffer *pb);

/**
 * Return the allocated size of the path buffer.
 *
 * @param pb   the path buffer
 *
 * @return     size
 **/
size_t pathBufferSize(const PathBuffer *pb);

/**
 * Return the path buffer's path.
 *
 * @param pb   the path buffer
 *
 * @return     path
 **/
const char *pathBufferPath(const PathBuffer *pb);

/**
 * Return the path buffer's buffer. Used for mkstemp/mkdtemp.
 *
 * @param pb    the path buffer
 *
 * @return the path buffer, or NULL if no path exists
 **/
char *pathBufferBuffer(const PathBuffer *pb);

/**
 * Replace the existing contents with new contents derived via sprintf.
 *
 * @param pb   the path buffer
 * @param fmt  the sprint format string
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int setPathBufferSprintf(PathBuffer *pb, char *fmt, ...)
  __attribute__((format(printf, 2, 3), warn_unused_result));

/**
 * @param dst  the destination path buffer
 * @param src  the source path buffer
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int copyPathBuffer(PathBuffer *dst, const PathBuffer *src)
  __attribute__((warn_unused_result));

/**
 * Truncate the current contents of the path buffer.
 *
 * @param pb   the path buffer
 * @param len  the desired length
 *
 * @return UDS_INVALID_ARGUMENT if len greather than the current length,
 *         UDS_SUCCESS, or an allocation failure
 **/
int truncatePathBuffer(PathBuffer *pb, size_t len)
  __attribute__((warn_unused_result));

/**
 * Append the contents of one path buffer to this one.
 *
 * @param dst  the destination path buffer
 * @param src  the source path buffer
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int appendPathBuffer(PathBuffer *dst, const PathBuffer *src)
  __attribute__((warn_unused_result));

/**
 * Append an sprintf-generated string to the path buffer.
 *
 * @param pb   the path buffer
 * @param fmt  the sprintf format string
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int appendPathBufferSprintf(PathBuffer *pb, const char *fmt, ...)
  __attribute__((format(printf, 2, 3), warn_unused_result));

/**
 * Resize the path buffer to exactly fit its current contents.
 *
 * @param pb   the path buffer
 *
 * @return UDS_SUCCESS or an allocation failure.
 **/
int setPathBufferSizeToFit(PathBuffer *pb)
  __attribute__((warn_unused_result));

/**
 * Resize the path buffer to fit the specified size.
 *
 * @param pb   the path buffer
 * @param size the requested size
 *
 * @return UDS_INVALID_ARGUMENT if the new size would truncate the string,
 *         UDS_SUCCESS, or an allocation failure.
 **/
int setPathBufferSize(PathBuffer *pb, size_t size)
  __attribute__((warn_unused_result));

#endif /* PATH_BUFFER_H */
