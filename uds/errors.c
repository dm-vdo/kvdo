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
 * $Id: //eng/uds-releases/krusty/src/uds/errors.c#27 $
 */

#include "errors.h"

#include "common.h"
#include "permassert.h"
#include "stringUtils.h"

#include <linux/errno.h>

static const struct error_info successful = { "UDS_SUCCESS", "Success" };

static const char *const message_table[] = {
	[EPERM] = "Operation not permitted",
	[ENOENT] = "No such file or directory",
	[ESRCH] = "No such process",
	[EINTR] = "Interrupted system call",
	[EIO] = "Input/output error",
	[ENXIO] = "No such device or address",
	[E2BIG] = "Argument list too long",
	[ENOEXEC] = "Exec format error",
	[EBADF] = "Bad file descriptor",
	[ECHILD] = "No child processes",
	[EAGAIN] = "Resource temporarily unavailable",
	[ENOMEM] = "Cannot allocate memory",
	[EACCES] = "Permission denied",
	[EFAULT] = "Bad address",
	[ENOTBLK] = "Block device required",
	[EBUSY] = "Device or resource busy",
	[EEXIST] = "File exists",
	[EXDEV] = "Invalid cross-device link",
	[ENODEV] = "No such device",
	[ENOTDIR] = "Not a directory",
	[EISDIR] = "Is a directory",
	[EINVAL] = "Invalid argument",
	[ENFILE] = "Too many open files in system",
	[EMFILE] = "Too many open files",
	[ENOTTY] = "Inappropriate ioctl for device",
	[ETXTBSY] = "Text file busy",
	[EFBIG] = "File too large",
	[ENOSPC] = "No space left on device",
	[ESPIPE] = "Illegal seek",
	[EROFS] = "Read-only file system",
	[EMLINK] = "Too many links",
	[EPIPE] = "Broken pipe",
	[EDOM] = "Numerical argument out of domain",
	[ERANGE] = "Numerical result out of range"
};

static const struct error_info error_list[] = {
	{ "UDS_OVERFLOW", "Index overflow" },
	{ "UDS_INVALID_ARGUMENT",
	  "Invalid argument passed to internal routine" },
	{ "UDS_BAD_STATE", "UDS data structures are in an invalid state" },
	{ "UDS_DUPLICATE_NAME",
	  "Attempt to enter the same name into a delta index twice" },
	{ "UDS_UNEXPECTED_RESULT", "Unexpected result from internal routine" },
	{ "UDS_ASSERTION_FAILED", "Assertion failed" },
	{ "UDS_QUEUED", "Request queued" },
	{ "UDS_BUFFER_ERROR", "Buffer error" },
	{ "UDS_NO_DIRECTORY", "Expected directory is missing" },
	{ "UDS_CHECKPOINT_INCOMPLETE", "Checkpoint not completed" },
	{ "UDS_ALREADY_REGISTERED", "Error range already registered" },
	{ "UDS_BAD_IO_DIRECTION", "Bad I/O direction" },
	{ "UDS_INCORRECT_ALIGNMENT", "Offset not at block alignment" },
	{ "UDS_OUT_OF_RANGE", "Cannot access data outside specified limits" },
	{ "UDS_EMODULE_LOAD", "Could not load modules" },
	{ "UDS_DISABLED", "UDS library context is disabled" },
	{ "UDS_CORRUPT_COMPONENT", "Corrupt saved component" },
	{ "UDS_UNKNOWN_ERROR", "Unknown error" },
	{ "UDS_UNSUPPORTED_VERSION", "Unsupported version" },
	{ "UDS_CORRUPT_DATA", "Index data in memory is corrupt" },
	{ "UDS_SHORT_READ", "Could not read requested number of bytes" },
	{ "UDS_RESOURCE_LIMIT_EXCEEDED", "Internal resource limits exceeded" },
	{ "UDS_VOLUME_OVERFLOW", "Memory overflow due to storage failure" },
	{ "UDS_NO_INDEX", "No index found" },
	{ "UDS_END_OF_FILE", "Unexpected end of file" },
	{ "UDS_INDEX_NOT_SAVED_CLEANLY", "Index not saved cleanly" },
};

struct error_block {
	const char *name;
	int base;
	int last;
	int max;
	const struct error_info *infos;
};

enum {
	MAX_ERROR_BLOCKS = 6 // needed for testing
};

static struct error_information {
	int allocated;
	int count;
	struct error_block blocks[MAX_ERROR_BLOCKS];
} registered_errors = {
	.allocated = MAX_ERROR_BLOCKS,
	.count = 1,
	.blocks = { {
			    .name = "UDS Error",
			    .base = UDS_ERROR_CODE_BASE,
			    .last = UDS_ERROR_CODE_LAST,
			    .max = UDS_ERROR_CODE_BLOCK_END,
			    .infos = error_list,
		    } }
};

/**
 * Fetch the error info (if any) for the error number.
 *
 * @param errnum        the error number
 * @param info_ptr      the place to store the info for this error (if known),
 *                      otherwise set to NULL
 *
 * @return              the name of the error block (if known), NULL othersise
 **/
static const char *get_error_info(int errnum,
				  const struct error_info **info_ptr)
{
	struct error_block *block;

	if (errnum == UDS_SUCCESS) {
		if (info_ptr != NULL) {
			*info_ptr = &successful;
		}
		return NULL;
	}

	for (block = registered_errors.blocks;
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

/**
 * Return string describing a system error message
 *
 * @param errnum  System error number
 * @param buf     Buffer that can be used to contain the return value
 * @param buflen  Length of the buffer
 *
 * @return The error string, which may be a string constant or may be
 *         returned in the buf argument
 **/
static const char *system_string_error(int errnum, char *buf, size_t buflen)
{
	size_t len;
	const char *error_string = NULL;
	if ((errnum > 0) && (errnum < COUNT_OF(message_table))) {
		error_string = message_table[errnum];
	}

	len = ((error_string == NULL) ?
		 snprintf(buf, buflen, "Unknown error %d", errnum) :
		 snprintf(buf, buflen, "%s", error_string));
	if (len < buflen) {
		return buf;
	}

	buf[0] = '\0';
	return "System error";
}

/**********************************************************************/
const char *string_error(int errnum, char *buf, size_t buflen)
{
	char *buffer = buf;
	char *buf_end = buf + buflen;
	const struct error_info *info = NULL;
	const char *block_name;

	if (buf == NULL) {
		return NULL;
	}

	if (errnum < 0) {
		errnum = -errnum;
	}

	block_name = get_error_info(errnum, &info);

	if (block_name != NULL) {
		if (info != NULL) {
			buffer = uds_append_to_buffer(buffer,
						      buf_end,
						      "%s: %s",
						      block_name,
						      info->message);
		} else {
			buffer = uds_append_to_buffer(buffer,
						      buf_end,
						      "Unknown %s %d",
						      block_name,
						      errnum);
		}
	} else if (info != NULL) {
		buffer = uds_append_to_buffer(buffer, buf_end, "%s",
					      info->message);
	} else {
		const char *tmp =
			system_string_error(errnum, buffer, buf_end - buffer);
		if (tmp != buffer) {
			buffer = uds_append_to_buffer(buffer, buf_end, "%s",
						      tmp);
		} else {
			buffer += strlen(tmp);
		}
	}
	return buf;
}

/**********************************************************************/
const char *string_error_name(int errnum, char *buf, size_t buflen)
{

	char *buffer = buf;
	char *buf_end = buf + buflen;
	const struct error_info *info = NULL;
	const char *block_name;

	if (errnum < 0) {
		errnum = -errnum;
	}
	block_name = get_error_info(errnum, &info);
	if (block_name != NULL) {
		if (info != NULL) {
			buffer = uds_append_to_buffer(buffer, buf_end, "%s",
						      info->name);
		} else {
			buffer = uds_append_to_buffer(buffer, buf_end, "%s %d",
						      block_name, errnum);
		}
	} else if (info != NULL) {
		buffer = uds_append_to_buffer(buffer, buf_end, "%s",
					      info->name);
	} else {
		const char *tmp =
			system_string_error(errnum, buffer, buf_end - buffer);
		if (tmp != buffer) {
			buffer = uds_append_to_buffer(buffer, buf_end, "%s",
						      tmp);
		} else {
			buffer += strlen(tmp);
		}
	}
	return buf;
}

/**********************************************************************/
int uds_map_to_system_error(int error)
{
	char error_name[80], error_message[ERRBUF_SIZE];

	// 0 is success, negative a system error code
	if (likely(error <= 0)) {
		return error;
	}

	if (error < 1024) {
		// probably an errno from userspace, just negate it.
		return -error;
	}

	// UDS error
	switch (error) {
	case UDS_NO_INDEX:
	case UDS_CORRUPT_COMPONENT:
		// The index doesn't exist or can't be recovered.
		return -ENOENT;

	case UDS_INDEX_NOT_SAVED_CLEANLY:
	case UDS_UNSUPPORTED_VERSION:
		// The index exists, but can't be loaded. Tell the client it
		// exists so they don't destroy it inadvertently.
		return -EEXIST;

	case UDS_DISABLED:
		// The session is unusable; only returned by requests.
		return -EIO;

	default:
		// No other UDS error code is expected here, so log what we
		// got and convert to something reasonable.
		uds_log_info("%s: mapping status code %d (%s: %s) to -EIO",
			     __func__,
			     error,
			     string_error_name(error,
					       error_name,
					       sizeof(error_name)),
			     uds_string_error(error,
					      error_message,
					      sizeof(error_message)));
		return -EIO;
	}
}

/**********************************************************************/
int register_error_block(const char *block_name,
			 int first_error,
			 int last_reserved_error,
			 const struct error_info *infos,
			 size_t info_size)
{
	struct error_block *block;
	int result = ASSERT(first_error < last_reserved_error,
			    "bad error block range");
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (registered_errors.count == registered_errors.allocated) {
		// could reallocate and grow, but should never happen
		return UDS_OVERFLOW;
	}

	for (block = registered_errors.blocks;
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
					      (info_size /
					       sizeof(struct error_info)),
				      .max = last_reserved_error,
				      .infos = infos };

	return UDS_SUCCESS;
}
