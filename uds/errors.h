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
 * $Id: //eng/uds-releases/krusty/src/uds/errors.h#16 $
 */

#ifndef ERRORS_H
#define ERRORS_H

#include "compiler.h"
#include "typeDefs.h"

/**
 * Valid return status codes for internal UDS functions.
 **/
enum uds_status_codes {
	/** Successful return */
	UDS_SUCCESS = 0,

	/** Used as a base value for reporting internal errors */
	UDS_ERROR_CODE_BASE = 1024,
	/** Index overflow */
	UDS_OVERFLOW = UDS_ERROR_CODE_BASE + 0,
	/** Invalid argument passed to internal routine */
	UDS_INVALID_ARGUMENT = UDS_ERROR_CODE_BASE + 1,
	/** UDS data structures are in an invalid state */
	UDS_BAD_STATE = UDS_ERROR_CODE_BASE + 2,
	/** Attempt to enter the same name into an internal structure twice */
	UDS_DUPLICATE_NAME = UDS_ERROR_CODE_BASE + 3,
	/** An internal protocol violation between system components */
	UDS_UNEXPECTED_RESULT = UDS_ERROR_CODE_BASE + 4,
	/** An assertion failed */
	UDS_ASSERTION_FAILED = UDS_ERROR_CODE_BASE + 5,
	/** Not an actual error, but reporting that the result will be
	 *  delayed */
	UDS_QUEUED = UDS_ERROR_CODE_BASE + 6,
	/** A problem has occured with a buffer */
	UDS_BUFFER_ERROR = UDS_ERROR_CODE_BASE + 7,
	/** No directory was found where one was expected */
	UDS_NO_DIRECTORY = UDS_ERROR_CODE_BASE + 8,
	/** Checkpoint not completed */
	UDS_CHECKPOINT_INCOMPLETE = UDS_ERROR_CODE_BASE + 9,
	/** This error range has already been registered */
	UDS_ALREADY_REGISTERED = UDS_ERROR_CODE_BASE + 10,
	/** Either read-only or write-only */
	UDS_BAD_IO_DIRECTION = UDS_ERROR_CODE_BASE + 11,
	/** Cannot do I/O at this offset */
	UDS_INCORRECT_ALIGNMENT = UDS_ERROR_CODE_BASE + 12,
	/** Attempt to read or write data outside the bounds established for
	 *  it */
	UDS_OUT_OF_RANGE = UDS_ERROR_CODE_BASE + 13,
	/** Could not load scanner modules */
	UDS_EMODULE_LOAD = UDS_ERROR_CODE_BASE + 14,
	/** The specified library context is disabled */
	UDS_DISABLED = UDS_ERROR_CODE_BASE + 15,
	/** Some saved index component is corrupt */
	UDS_CORRUPT_COMPONENT = UDS_ERROR_CODE_BASE + 16,
	UDS_CORRUPT_FILE = UDS_CORRUPT_COMPONENT,
	/** Unknown error */
	UDS_UNKNOWN_ERROR = UDS_ERROR_CODE_BASE + 17,
	/** The index configuration or volume format is no longer supported */
	UDS_UNSUPPORTED_VERSION = UDS_ERROR_CODE_BASE + 18,
	/** Index data in memory is corrupt */
	UDS_CORRUPT_DATA = UDS_ERROR_CODE_BASE + 19,
	/** Short read due to truncated file */
	UDS_SHORT_READ = UDS_ERROR_CODE_BASE + 20,
	/** Internal resource limits exceeded */
	UDS_RESOURCE_LIMIT_EXCEEDED = UDS_ERROR_CODE_BASE + 21,
	/** Memory overflow due to storage failure */
	UDS_VOLUME_OVERFLOW = UDS_ERROR_CODE_BASE + 22,
	/** Essential files for index not found */
	UDS_NO_INDEX = UDS_ERROR_CODE_BASE + 23,
	/** Premature end of file in scanned file */
	UDS_END_OF_FILE = UDS_ERROR_CODE_BASE + 24,
	/** Attempt to access unsaved index */
	UDS_INDEX_NOT_SAVED_CLEANLY = UDS_ERROR_CODE_BASE + 25,
	/** One more than the last UDS_INTERNAL error code */
	UDS_ERROR_CODE_LAST,
	/** One more than the last error this block will ever use */
	UDS_ERROR_CODE_BLOCK_END = UDS_ERROR_CODE_BASE + 440
};

enum {
	ERRBUF_SIZE = 128 // default size for buffer passed to string_error
};

const char *string_error(int errnum, char *buf, size_t buflen);
const char *string_error_name(int errnum, char *buf, size_t buflen);

/*
 * Identify that an result code is a successful result.
 *
 * @param result  A result code
 *
 * @return true if the result represents a success.
 */
static INLINE bool __must_check is_successful(int result)
{
	return (result == UDS_SUCCESS) || (result == UDS_QUEUED);
}

struct error_info {
	const char *name;
	const char *message;
};

/**
 * Given an error code, return a value acceptable to the kernel. The input
 * error code may be a system-generated value (such as -EIO), or an internal
 * UDS status code; the result will be a negative errno value.
 *
 * @param error  The error code to convert
 *
 * @return a system error code value
 **/
int uds_map_to_system_error(int error);

/**
 * Register an error code block for string_error and string_error_name.
 *
 * @param block_name           the name of the block of error codes
 * @param first_error          the first error code in the block
 * @param last_reserved_error  one past the highest possible error in the block
 * @param infos                a pointer to the error info array for the block
 * @param info_size            the size of the error info array, which
 *                             determines the last actual error for which
 *                             information is available
 *
 * @return a success or error code, particularly UDS_DUPLICATE_NAME if the
 *         block name is already present, or UDS_ALREADY_REGISTERED if a
 *         block with the specified error code is present
 **/
int register_error_block(const char *block_name,
			 int first_error,
			 int last_reserved_error,
			 const struct error_info *infos,
			 size_t info_size);

/**
 * Return the first error between result1 and result2.
 *
 * @param result1       A success or error code.
 * @param result2       A success or error code.
 *
 * @return result1 if that is an error, else result2
 **/
static INLINE int first_error(int result1, int result2)
{
	return result1 == UDS_SUCCESS ? result2 : result1;
}

#endif /* ERRORS_H */
