/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/errors.c#4 $
 */

#include "errors.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "permassert.h"
#include "statusCodes.h"

static const struct errorInfo error_list[] = {
	{ "UDS_UNINITIALIZED", "UDS library is not initialized" },
	{ "UDS_SHUTTINGDOWN", "UDS library is shutting down" },
	{ "UDS_EMODULE_LOAD", "Could not load modules" },
	{ "UDS_ENOTHREADS", "Could not create a new thread" },
	{ "UDS_NOCONTEXT", "Could not find the requested library context" },
	{ "UDS_DISABLED", "UDS library context is disabled" },
	{ "UDS_CORRUPT_FILE", "Corrupt file" },
	{ "UDS_UNKNOWN_ERROR", "Unknown error" },
	{ "UDS_GRID_NO_SERVERS", "No servers in grid configuration" },
	{ "UDS_GRID_CONFIG_INCONSISTENT", "Grid configuration inconsistent" },
	{ "UDS_UNSUPPORTED_VERSION", "Unsupported version" },
	{ "UDS_NO_INDEXSESSION", "Index session not known" },
	{ "UDS_CORRUPT_DATA", "Index data in memory is corrupt" },
	{ "UDS_SHORT_READ", "Could not read requested number of bytes" },
	{ "UDS_AI_ERROR", "Network address and service translation error" },
	{ "UDS_RESOURCE_LIMIT_EXCEEDED", "Internal resource limits exceeded" },
	{ "UDS_WRONG_CONTEXT_TYPE", "Context type mismatch" },
	{ "UDS_BLOCK_ADDRESS_REQUIRED", "A block address is required" },
	{ "UDS_CHUNK_DATA_REQUIRED", "Block data is required" },
	{ "UDS_CHUNK_NAME_REQUIRED", "A chunk name is required" },
	{ "UDS_CONF_PTR_REQUIRED", "A configuration pointer is required" },
	{ "UDS_INDEX_STATS_PTR_REQUIRED",
	  "An index stats pointer is required" },
	{ "UDS_CONTEXT_STATS_PTR_REQUIRED",
	  "A context stats pointer is required" },
	{ "UDS_CONTEXT_PTR_REQUIRED", "A context pointer is required" },
	{ "UDS_FILEID_REQUIRED", "A file ID is required" },
	{ "UDS_STREAM_REQUIRED", "A stream is required" },
	{ "UDS_STREAMID_REQUIRED", "A stream ID is required" },
	{ "UDS_STREAM_PTR_REQUIRED", "A stream pointer is required" },
	{ "UDS_INVALID_MEMORY_SIZE",
	  "Configured memory too small or unsupported size" },
	{ "UDS_INVALID_METADATA_SIZE", "Invalid metadata size" },
	{ "UDS_INDEX_NAME_REQUIRED", "An index name is required" },
	{ "UDS_CONF_REQUIRED", "A configuration is required" },
	{ "UDS_BAD_FILE_DESCRIPTOR", "Bad file descriptor" },
	{ "UDS_INDEX_EXISTS", "Index already exists" },
	{ "UDS_REQUESTS_OUT_OF_RANGE", "Maximum request value out of range" },
	{ "UDS_BAD_NAMESPACE", "Bad namespace" },
	{ "UDS_MIGRATOR_MISMATCH",
	  "Migrator arguments do not match reader arguments" },
	{ "UDS_NO_INDEX", "No index found" },
	{ "UDS_BAD_CHECKPOINT_FREQUENCY", "Checkpoint frequency out of range" },
	{ "UDS_WRONG_INDEX_CONFIG", "Wrong type of index configuration" },
	{ "UDS_INDEX_PATH_NOT_DIR",
	  "Index path does not point to a directory" },
	{ "UDS_ALREADY_OPEN", "Open invoked on already opened connection" },
	{ "UDS_CALLBACK_ALREADY_REGISTERED", "Callback already registered" },
	{ "UDS_INDEX_PATH_TOO_LONG", "Index path too long" },
	{ "UDS_END_OF_FILE", "Unexpected end of file" },
	{ "UDS_INDEX_NOT_SAVED_CLEANLY", "Index not saved cleanly" },
};

static const struct errorInfo internal_error_list[] = {
	{ "UDS_PROTOCOL_ERROR", "Client/server protocol error" },
	{ "UDS_OVERFLOW", "Index overflow" },
	{ "UDS_FILLDONE", "Fill phase done" },
	{ "UDS_INVALID_ARGUMENT",
	  "Invalid argument passed to internal routine" },
	{ "UDS_BAD_STATE", "UDS data structures are in an invalid state" },
	{ "UDS_DUPLICATE_NAME",
	  "Attempt to enter the same name into a delta index twice" },
	{ "UDS_UNEXPECTED_RESULT", "Unexpected result from internal routine" },
	{ "UDS_INJECTED_ERROR", "Injected error" },
	{ "UDS_ASSERTION_FAILED", "Assertion failed" },
	{ "UDS_UNSCANNABLE", "Unscannable" },
	{ "UDS_QUEUED", "Request queued" },
	{ "UDS_QUEUE_ALREADY_CONNECTED", "Queue already connected" },
	{ "UDS_BAD_FILL_PHASE", "Fill phase not supported" },
	{ "UDS_BUFFER_ERROR", "Buffer error" },
	{ "UDS_CONNECTION_LOST", "Lost connection to peer" },
	{ "UDS_TIMEOUT", "A time out has occurred" },
	{ "UDS_NO_DIRECTORY", "Expected directory is missing" },
	{ "UDS_CHECKPOINT_INCOMPLETE", "Checkpoint not completed" },
	{ "UDS_INVALID_RUN_ID", "Invalid albGenTest server run ID" },
	{ "UDS_RUN_CANCELED", "albGenTest server run canceled" },
	{ "UDS_ALREADY_REGISTERED", "error range already registered" },
};

/** Error attributes - or into top half of error code */
enum { UDS_UNRECOVERABLE = (1 << 17) };

struct error_block {
	const char *name;
	int base;
	int last;
	int max;
	const ErrorInfo *infos;
};

enum {
	MAX_ERROR_BLOCKS = 6 // needed for testing
};

static struct error_information {
	int allocated;
	int count;
	struct error_block blocks[MAX_ERROR_BLOCKS];
} registered_errors;

/**********************************************************************/
void initialize_standard_error_blocks(void)
{
	registered_errors.allocated = MAX_ERROR_BLOCKS;
	registered_errors.count = 0;


	registered_errors.blocks[registered_errors.count++] =
		(struct error_block){
			.name = "UDS Error",
			.base = UDS_ERROR_CODE_BASE,
			.last = UDS_ERROR_CODE_LAST,
			.max = UDS_ERROR_CODE_BLOCK_END,
			.infos = error_list,
		};

	registered_errors.blocks[registered_errors.count++] =
		(struct error_block){
			.name = "UDS Internal Error",
			.base = UDS_INTERNAL_ERROR_CODE_BASE,
			.last = UDS_INTERNAL_ERROR_CODE_LAST,
			.max = UDS_INTERNAL_ERROR_CODE_BLOCK_END,
			.infos = internal_error_list,
		};

	registered_errors.blocks[registered_errors.count++] =
		(struct error_block){
			.name = THIS_MODULE->name,
			.base = VDO_BLOCK_START,
			.last = VDO_STATUS_CODE_LAST,
			.max = VDO_BLOCK_END,
			.infos = vdoStatusList,
		};
}

/**
 * Fetch the error info (if any) for the error number.
 *
 * @param errnum       the error number
 * @param info_ptr     the place to store the info for this error (if known),
 *                     otherwise set to NULL
 *
 * @return             the name of the error block (if known), NULL otherwise
 **/
static const char *get_error_info(int errnum, const ErrorInfo **info_ptr)
{
	for (struct error_block *block = registered_errors.blocks;
	     block < registered_errors.blocks + registered_errors.count;
	     ++block) {
		if ((errnum >= block->base) && (errnum < block->last)) {
			if (info_ptr != NULL) {
				*info_ptr =
					block->infos + (errnum - block->base);
			}
			return block->name;
		} else if ((errnum >= block->last) && (errnum < block->max)) {
			if (info_ptr != NULL) {
				*info_ptr = NULL;
			}
			return block->name;
		}
	}
	if (info_ptr != NULL) {
		*info_ptr = NULL;
	}
	return NULL;
}

/*****************************************************************************/
const char *string_error(int errnum, char *buf, size_t buflen)
{
	if (buf == NULL) {
		return NULL;
	}

	const ErrorInfo *info = NULL;
	const char *block_name = get_error_info(errnum, &info);

	if (block_name != NULL) {
		if (info != NULL) {
			snprintf(buf, buflen, "%s: %s", block_name,
				 info->message);
		} else {
			snprintf(buf, buflen, "Unknown %s %d", block_name,
				 errnum);
		}
	} else {
		snprintf(buf, buflen, "System error %d", errnum);
	}
	return buf;
}

/*****************************************************************************/
const char *string_error_name(int errnum, char *buf, size_t buflen)
{
	const ErrorInfo *info = NULL;
	const char *block_name = get_error_info(errnum, &info);

	if (block_name != NULL) {
		if (info != NULL) {
			snprintf(buf, buflen, "%s: %s", block_name, info->name);
		} else {
			snprintf(buf, buflen, "Unknown %s %d", block_name,
				 errnum);
		}
	} else {
		snprintf(buf, buflen, "System error %d", errnum);
	}
	return buf;
}

/*****************************************************************************/
int make_unrecoverable(int result_code)
{
	return ((result_code == UDS_SUCCESS) ?
			result_code :
			(result_code | UDS_UNRECOVERABLE));
}

/*****************************************************************************/
int sans_unrecoverable(int result_code)
{
	return result_code & ~UDS_UNRECOVERABLE;
}

/*****************************************************************************/
bool is_unrecoverable(int result_code)
{
	return (bool)(result_code & UDS_UNRECOVERABLE);
}

/*****************************************************************************/
int register_error_block(const char *block_name,
			 int first_error,
			 int last_reserved_error,
			 const ErrorInfo *infos,
			 size_t info_size)
{
	int result = ASSERT(first_error < last_reserved_error,
			    "bad error block range");
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (registered_errors.count == registered_errors.allocated) {
		// could reallocate and grow, but should never happen
		return UDS_OVERFLOW;
	}

	for (struct error_block *block = registered_errors.blocks;
	     block < registered_errors.blocks + registered_errors.count;
	     ++block) {
		if (strcmp(block_name, block->name) == 0) {
			return UDS_DUPLICATE_NAME;
		}
		// check for overlap in error ranges
		if ((first_error < block->max) &&
		    (last_reserved_error > block->base)) {
			return UDS_ALREADY_REGISTERED;
		}
	}

	registered_errors.blocks[registered_errors.count++] =
		(struct error_block){ .name = block_name,
				      .base = first_error,
				      .last = first_error +
					      (info_size / sizeof(ErrorInfo)),
				      .max = last_reserved_error,
				      .infos = infos };

	return UDS_SUCCESS;
}
