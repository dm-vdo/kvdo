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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/statusCodes.c#9 $
 */

#include "statusCodes.h"

#include "errors.h"
#include "permassert.h"
#include "threadOnce.h"

const struct error_info vdo_status_list[] = {
	{ "VDO_NOT_IMPLEMENTED", "Not implemented" },
	{ "VDO_OUT_OF_RANGE", "Out of range" },
	{ "VDO_REF_COUNT_INVALID", "Reference count would become invalid" },
	{ "VDO_NO_SPACE", "Out of space" },
	{ "VDO_UNEXPECTED_EOF", "Unexpected EOF on block read" },
	{ "VDO_BAD_CONFIGURATION", "Bad configuration option" },
	{ "VDO_SOCKET_ERROR", "Socket error" },
	{ "VDO_BAD_ALIGNMENT", "Mis-aligned block reference" },
	{ "VDO_COMPONENT_BUSY", "Prior operation still in progress" },
	{ "VDO_BAD_PAGE", "Corrupt or incorrect page" },
	{ "VDO_UNSUPPORTED_VERSION", "Unsupported component version" },
	{ "VDO_INCORRECT_COMPONENT", "Component id mismatch in decoder" },
	{ "VDO_PARAMETER_MISMATCH", "Parameters have conflicting values" },
	{ "VDO_BLOCK_SIZE_TOO_SMALL", "The block size is too small" },
	{ "VDO_UNKNOWN_PARTITION", "No partition exists with a given id" },
	{ "VDO_PARTITION_EXISTS",
	  "A partition already exists with a given id" },
	{ "VDO_NOT_READ_ONLY", "The device is not in read-only mode" },
	{ "VDO_INCREMENT_TOO_SMALL",
	  "Physical block growth of too few blocks" },
	{ "VDO_CHECKSUM_MISMATCH", "Incorrect checksum" },
	{ "VDO_RECOVERY_JOURNAL_FULL", "The recovery journal is full" },
	{ "VDO_LOCK_ERROR", "A lock is held incorrectly" },
	{ "VDO_READ_ONLY", "The device is in read-only mode" },
	{ "VDO_SHUTTING_DOWN", "The device is shutting down" },
	{ "VDO_CORRUPT_JOURNAL", "Recovery journal entries corrupted" },
	{ "VDO_TOO_MANY_SLABS", "Exceeds maximum number of slabs supported" },
	{ "VDO_INVALID_FRAGMENT", "Compressed block fragment is invalid" },
	{ "VDO_RETRY_AFTER_REBUILD",
	  "Retry operation after rebuilding finishes" },
	{ "VDO_UNKNOWN_COMMAND", "The extended command is not known" },
	{ "VDO_COMMAND_ERROR", "Bad extended command parameters" },
	{ "VDO_CANNOT_DETERMINE_SIZE", "Cannot determine config sizes to fit" },
	{ "VDO_BAD_MAPPING", "Invalid page mapping" },
	{ "VDO_READ_CACHE_BUSY", "Read cache has no free slots" },
	{ "VDO_BIO_CREATION_FAILED", "Bio creation failed" },
	{ "VDO_BAD_MAGIC", "Bad magic number" },
	{ "VDO_BAD_NONCE", "Bad nonce" },
	{ "VDO_JOURNAL_OVERFLOW", "Journal sequence number overflow" },
	{ "VDO_INVALID_ADMIN_STATE", "Invalid operation for current state" },
};

static once_state_t vdo_status_codes_registered = ONCE_STATE_INITIALIZER;
static int status_code_registration_result;

/**********************************************************************/
static void do_status_code_registration(void)
{
	STATIC_ASSERT((VDO_STATUS_CODE_LAST - VDO_STATUS_CODE_BASE) ==
		      COUNT_OF(vdo_status_list));

	int result = register_error_block("VDO Status",
					  VDO_STATUS_CODE_BASE,
					  VDO_STATUS_CODE_BLOCK_END,
					  vdo_status_list,
					  sizeof(vdo_status_list));
	/*
	 *  The following test handles cases where libvdo is statically linked
	 *  against both the test modules and the test driver (because multiple
	 *  instances of this module call their own copy of this function
	 *  once each, resulting in multiple calls to register_error_block
	 *  which is shared in libuds).
	 */
	if (result == UDS_DUPLICATE_NAME) {
		result = UDS_SUCCESS;
	}

	status_code_registration_result =
		(result == UDS_SUCCESS) ? VDO_SUCCESS : result;
}

/**********************************************************************/
int register_status_codes(void)
{
	perform_once(&vdo_status_codes_registered, do_status_code_registration);
	return status_code_registration_result;
}
