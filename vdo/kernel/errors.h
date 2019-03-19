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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/errors.h#1 $
 */

#ifndef ERRORS_H
#define ERRORS_H

#include <linux/types.h>
#include "uds-error.h"

enum udsInternalErrorCodes {
  /** Used as a base value for reporting internal errors */
  UDS_INTERNAL_ERROR_CODE_BASE = 66560,
  /** Client/server protocol framing error */
  UDS_PROTOCOL_ERROR           = UDS_INTERNAL_ERROR_CODE_BASE + 0,
  /** Index overflow */
  UDS_OVERFLOW                 = UDS_INTERNAL_ERROR_CODE_BASE + 1,
  /** Fill phase done (intended for albfill only) */
  UDS_FILLDONE                 = UDS_INTERNAL_ERROR_CODE_BASE + 2,
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
  /** A file or stream is not scannable with the current scanner */
  UDS_UNSCANNABLE              = UDS_INTERNAL_ERROR_CODE_BASE + 9,
  /** Not an actual error, but reporting that the result will be delayed */
  UDS_QUEUED                   = UDS_INTERNAL_ERROR_CODE_BASE + 10,
  /** Queue already connected */
  UDS_QUEUE_ALREADY_CONNECTED  = UDS_INTERNAL_ERROR_CODE_BASE + 11,
  /** Fill phase not supported */
  UDS_BAD_FILL_PHASE           = UDS_INTERNAL_ERROR_CODE_BASE + 12,
  /** A problem has occurred with a Buffer */
  UDS_BUFFER_ERROR             = UDS_INTERNAL_ERROR_CODE_BASE + 13,
  /** A network connection was lost */
  UDS_CONNECTION_LOST          = UDS_INTERNAL_ERROR_CODE_BASE + 14,
  /** A time out has occurred */
  UDS_TIMEOUT                  = UDS_INTERNAL_ERROR_CODE_BASE + 15,
  /** No directory was found where one was expected */
  UDS_NO_DIRECTORY             = UDS_INTERNAL_ERROR_CODE_BASE + 16,
  /** Checkpoint not completed */
  UDS_CHECKPOINT_INCOMPLETE    = UDS_INTERNAL_ERROR_CODE_BASE + 17,
  /** Invalid albGenTest server run ID */
  UDS_INVALID_RUN_ID           = UDS_INTERNAL_ERROR_CODE_BASE + 18,
  /** albGenTest server run canceled */
  UDS_RUN_CANCELED             = UDS_INTERNAL_ERROR_CODE_BASE + 19,
  /** this error range has already been registered */
  UDS_ALREADY_REGISTERED       = UDS_INTERNAL_ERROR_CODE_BASE + 20,
  /** One more than the last UDS_INTERNAL error code */
  UDS_INTERNAL_ERROR_CODE_LAST,
  /** One more than the last error this block will ever use */
  UDS_INTERNAL_ERROR_CODE_BLOCK_END = UDS_INTERNAL_ERROR_CODE_BASE + 440
};

enum {
  ERRBUF_SIZE = 128 // default size for buffer passed to stringError
};

const char *stringError(int errnum, char *buf, size_t buflen);
const char *stringErrorName(int errnum, char *buf, size_t buflen);

int makeUnrecoverable(int resultCode) __attribute__((warn_unused_result));
bool isUnrecoverable(int resultCode) __attribute__((warn_unused_result));
int sansUnrecoverable(int resultCode) __attribute__((warn_unused_result));

typedef struct errorInfo {
  const char *name;
  const char *message;
} ErrorInfo;

/**
 * Initialize UDS error code blocks.
 *
 * @note Must be called once, before any of the other routines in this
 * file.
 **/
void initializeStandardErrorBlocks(void);

/**
 * Register an error code block for stringError and stringErrorName.
 *
 * @param blockName         the name of the block of error codes
 * @param firstError        the first error code in the block
 * @param lastReservedError one past the highest possible error in the block
 * @param infos             a pointer to the error info array for the block
 * @param infoSize          the size of the error info array, which determines
 *                          the last actual error for which information is
 *                          available
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

#endif /* ERRORS_H */
