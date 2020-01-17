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
 * $Id: //eng/uds-releases/jasper/src/uds/errors.h#4 $
 */

#ifndef ERRORS_H
#define ERRORS_H

#include "compiler.h"
#include "typeDefs.h"
#include "uds-error.h"

enum udsInternalErrorCodes {
  /** Used as a base value for reporting internal errors */
  UDS_INTERNAL_ERROR_CODE_BASE = 66560,
  /** Unused */
  UDS_INTERNAL_UNUSED_0        = UDS_INTERNAL_ERROR_CODE_BASE + 0,
  /** Index overflow */
  UDS_OVERFLOW                 = UDS_INTERNAL_ERROR_CODE_BASE + 1,
  /** Unused */
  UDS_INTERNAL_UNUSED_2        = UDS_INTERNAL_ERROR_CODE_BASE + 2,
  /** Invalid argument passed to internal routine */
  UDS_INVALID_ARGUMENT         = UDS_INTERNAL_ERROR_CODE_BASE + 3,
  /** UDS data structures are in an invalid state */
  UDS_BAD_STATE                = UDS_INTERNAL_ERROR_CODE_BASE + 4,
  /** Attempt to enter the same name into an internal structure twice */
  UDS_DUPLICATE_NAME           = UDS_INTERNAL_ERROR_CODE_BASE + 5,
  /** An internal protocol violation between system components */
  UDS_UNEXPECTED_RESULT        = UDS_INTERNAL_ERROR_CODE_BASE + 6,
  /** An error created by test case processing */
  UDS_INJECTED_ERROR           = UDS_INTERNAL_ERROR_CODE_BASE + 7,
  /** An assertion failed */
  UDS_ASSERTION_FAILED         = UDS_INTERNAL_ERROR_CODE_BASE + 8,
  /** Unused */
  UDS_INTERNAL_UNUSED_9        = UDS_INTERNAL_ERROR_CODE_BASE + 9,
  /** Not an actual error, but reporting that the result will be delayed */
  UDS_QUEUED                   = UDS_INTERNAL_ERROR_CODE_BASE + 10,
  /** Unused */
  UDS_INTERNAL_UNUSED_11       = UDS_INTERNAL_ERROR_CODE_BASE + 11,
  /** Unused */
  UDS_INTERNAL_UNUSED_12       = UDS_INTERNAL_ERROR_CODE_BASE + 12,
  /** A problem has occured with a Buffer */
  UDS_BUFFER_ERROR             = UDS_INTERNAL_ERROR_CODE_BASE + 13,
  /** Unused */
  UDS_INTERNAL_UNUSED_14       = UDS_INTERNAL_ERROR_CODE_BASE + 14,
  /** Unused */
  UDS_INTERNAL_UNUSED_15       = UDS_INTERNAL_ERROR_CODE_BASE + 15,
  /** No directory was found where one was expected */
  UDS_NO_DIRECTORY             = UDS_INTERNAL_ERROR_CODE_BASE + 16,
  /** Checkpoint not completed */
  UDS_CHECKPOINT_INCOMPLETE    = UDS_INTERNAL_ERROR_CODE_BASE + 17,
  /** Unused */
  UDS_INTERNAL_UNUSED_18       = UDS_INTERNAL_ERROR_CODE_BASE + 18,
  /** Unused */
  UDS_INTERNAL_UNUSED_19       = UDS_INTERNAL_ERROR_CODE_BASE + 19,
  /** This error range has already been registered */
  UDS_ALREADY_REGISTERED       = UDS_INTERNAL_ERROR_CODE_BASE + 20,
  /** Either read-only or write-only */
  UDS_BAD_IO_DIRECTION         = UDS_INTERNAL_ERROR_CODE_BASE + 21,
  /** Cannot do I/O at this offset */
  UDS_INCORRECT_ALIGNMENT      = UDS_INTERNAL_ERROR_CODE_BASE + 22,
  /** Attempt to read or write data outside the bounds established for it */
  UDS_OUT_OF_RANGE             = UDS_INTERNAL_ERROR_CODE_BASE + 23,
  /** One more than the last UDS_INTERNAL error code */
  UDS_INTERNAL_ERROR_CODE_LAST,
  /** One more than the last error this block will ever use */
  UDS_INTERNAL_ERROR_CODE_BLOCK_END = UDS_INTERNAL_ERROR_CODE_BASE + 440
};

enum {
  ERRBUF_SIZE = 128 // default size for buffer passed to stringError
};

// Error attributes - or into top half of error code
enum { UDS_UNRECOVERABLE = (1 << 17) };

const char *stringError(int errnum, char *buf, size_t buflen);
const char *stringErrorName(int errnum, char *buf, size_t buflen);

/*
 * Identify that an result code is a successful result.
 *
 * @param result  A result code
 *
 * @return true if the result represents a success.
 */
__attribute__((warn_unused_result))
static INLINE bool isSuccessful(int result)
{
  return (result == UDS_SUCCESS) || (result == UDS_QUEUED);
}

/*
 * Identify that an result code has been marked unrecoverable.
 *
 * @param result  A result code
 *
 * @return true if the result has been marked unrecoverable.
 */
__attribute__((warn_unused_result))
static INLINE bool isUnrecoverable(int result)
{
  return (result & UDS_UNRECOVERABLE) != 0;
}

/*
 * Mark a result code as unrecoverable.
 *
 * @param result  A result code
 *
 * @return the result code with the unrecoverable marker added
 */
__attribute__((warn_unused_result))
static INLINE int makeUnrecoverable(int result)
{
  return isSuccessful(result) ? result : (result | UDS_UNRECOVERABLE);
}

/*
 * Remove the unrecoverable marker from a result code.
 *
 * @param result  A result code
 *
 * @return the result code with the unrecoverable marker removed
 */
__attribute__((warn_unused_result))
static INLINE int sansUnrecoverable(int result)
{
  return result & ~UDS_UNRECOVERABLE;
}

typedef struct errorInfo {
  const char *name;
  const char *message;
} ErrorInfo;

/**
 * Register an error code block for stringError and stringErrorName.
 *
 * @param blockName             the name of the block of error codes
 * @param firstError            the first error code in the block
 * @param lastReservedError     one past the highest possible error in the bloc
 * @param infos                 a pointer to the error info array for the block
 * @param infoSize              the size of the error info array, which
 *                              determines the last actual error for which
 *                              information is available
 *
 * @return a success or error code, particularly UDS_DUPLICATE_NAME if the
 *         block name is already present, or UDS_ALREADY_REGISTERED if a
 *         block with the specified error code is present
 **/
int registerErrorBlock(const char      *blockName,
                       int              firstError,
                       int              lastReservedError,
                       const ErrorInfo *infos,
                       size_t           infoSize);

/**
 * Return the first error between result1 and result2.
 *
 * @param result1       A success or error code.
 * @param result2       A success or error code.
 *
 * @return result1 if that is an error, else result2
 **/
static INLINE int firstError(int result1, int result2)
{
  return result1 == UDS_SUCCESS ? result2 : result1;
}

#endif /* ERRORS_H */
