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
 * $Id: //eng/uds-releases/krusty/src/uds/uds-error.h#9 $
 */

/**
 * @file
 * @brief UDS error code definitions
 **/
#ifndef UDS_ERROR_H
#define UDS_ERROR_H


/**
 * Valid return status codes for API routines.
 **/
enum uds_status_codes {
	/** Successful return */
	UDS_SUCCESS = 0,

	/** Used as a base value for reporting errors  */
	UDS_ERROR_CODE_BASE = 1024,
	/** Unused */
	UDS_UNUSED_CODE_0 = UDS_ERROR_CODE_BASE + 0,
	/** Unused */
	UDS_UNUSED_CODE_1 = UDS_ERROR_CODE_BASE + 1,
	/** Could not load scanner modules */
	UDS_EMODULE_LOAD = UDS_ERROR_CODE_BASE + 2,
	/** Unused */
	UDS_UNUSED_CODE_3 = UDS_ERROR_CODE_BASE + 3,
	/** Unused */
	UDS_UNUSED_CODE_4 = UDS_ERROR_CODE_BASE + 4,
	/** The specified library context is disabled */
	UDS_DISABLED = UDS_ERROR_CODE_BASE + 5,
	/** Some saved index component is corrupt */
	UDS_CORRUPT_COMPONENT = UDS_ERROR_CODE_BASE + 6,
	UDS_CORRUPT_FILE = UDS_CORRUPT_COMPONENT,
	/** Unknown error */
	UDS_UNKNOWN_ERROR = UDS_ERROR_CODE_BASE + 7,
	/** Unused */
	UDS_UNUSED_CODE_8 = UDS_ERROR_CODE_BASE + 8,
	/** Unused */
	UDS_UNUSED_CODE_9 = UDS_ERROR_CODE_BASE + 9,
	/** The index configuration or volume format is no longer supported */
	UDS_UNSUPPORTED_VERSION = UDS_ERROR_CODE_BASE + 10,
	/** Unused */
	UDS_UNUSED_CODE_11 = UDS_ERROR_CODE_BASE + 11,
	/** Index data in memory is corrupt */
	UDS_CORRUPT_DATA = UDS_ERROR_CODE_BASE + 12,
	/** Short read due to truncated file */
	UDS_SHORT_READ = UDS_ERROR_CODE_BASE + 13,
	/** Unused */
	UDS_UNUSED_CODE_14 = UDS_ERROR_CODE_BASE + 14,
	/** Internal resource limits exceeded */
	UDS_RESOURCE_LIMIT_EXCEEDED = UDS_ERROR_CODE_BASE + 15,
	/** Memory overflow due to storage failure */
	UDS_VOLUME_OVERFLOW = UDS_ERROR_CODE_BASE + 16,
	/** Unused */
	UDS_UNUSED_CODE_17 = UDS_ERROR_CODE_BASE + 17,
	/** Unused */
	UDS_UNUSED_CODE_18 = UDS_ERROR_CODE_BASE + 18,
	/** Unused */
	UDS_UNUSED_CODE_19 = UDS_ERROR_CODE_BASE + 19,
	/** Unused */
	UDS_UNUSED_CODE_20 = UDS_ERROR_CODE_BASE + 20,
	/** Unused */
	UDS_UNUSED_CODE_21 = UDS_ERROR_CODE_BASE + 21,
	/** Unused */
	UDS_UNUSED_CODE_22 = UDS_ERROR_CODE_BASE + 22,
	/** Unused */
	UDS_UNUSED_CODE_23 = UDS_ERROR_CODE_BASE + 23,
	/** Unused */
	UDS_UNUSED_CODE_24 = UDS_ERROR_CODE_BASE + 24,
	/** Unused */
	UDS_UNUSED_CODE_25 = UDS_ERROR_CODE_BASE + 25,
	/** Unused */
	UDS_UNUSED_CODE_26 = UDS_ERROR_CODE_BASE + 26,
	/** Unused */
	UDS_UNUSED_CODE_27 = UDS_ERROR_CODE_BASE + 27,
	/** Unused */
	UDS_UNUSED_CODE_28 = UDS_ERROR_CODE_BASE + 28,
	/** Unused */
	UDS_UNUSED_CODE_29 = UDS_ERROR_CODE_BASE + 29,
	/** Unused */
	UDS_UNUSED_CODE_30 = UDS_ERROR_CODE_BASE + 30,
	/** Unused */
	UDS_UNUSED_CODE_31 = UDS_ERROR_CODE_BASE + 31,
	/** Unused */
	UDS_UNUSED_CODE_32 = UDS_ERROR_CODE_BASE + 32,
	/** Unused */
	UDS_UNUSED_CODE_33 = UDS_ERROR_CODE_BASE + 33,
	/** Unused */
	UDS_UNUSED_CODE_34 = UDS_ERROR_CODE_BASE + 34,
	/** Unused */
	UDS_UNUSED_CODE_35 = UDS_ERROR_CODE_BASE + 35,
	/** Unused */
	UDS_UNUSED_CODE_36 = UDS_ERROR_CODE_BASE + 36,
	/** Essential files for index not found */
	UDS_NO_INDEX = UDS_ERROR_CODE_BASE + 37,
	/** Unused */
	UDS_UNUSED_CODE_38 = UDS_ERROR_CODE_BASE + 38,
	/** Unused */
	UDS_UNUSED_CODE_39 = UDS_ERROR_CODE_BASE + 39,
	/** Unused */
	UDS_UNUSED_CODE_40 = UDS_ERROR_CODE_BASE + 40,
	/** Unused */
	UDS_UNUSED_CODE_41 = UDS_ERROR_CODE_BASE + 41,
	/** Unused */
	UDS_UNUSED_CODE_42 = UDS_ERROR_CODE_BASE + 42,
	/** Unused */
	UDS_UNUSED_CODE_43 = UDS_ERROR_CODE_BASE + 43,
	/** Premature end of file in scanned file */
	UDS_END_OF_FILE = UDS_ERROR_CODE_BASE + 44,
	/** Attempt to access unsaved index */
	UDS_INDEX_NOT_SAVED_CLEANLY = UDS_ERROR_CODE_BASE + 45,
	/** One more than the last UDS_ERROR_CODE */
	UDS_ERROR_CODE_LAST,
	/** One more than this block can use */
	UDS_ERROR_CODE_BLOCK_END = UDS_ERROR_CODE_BASE + 1024
};

#endif /* UDS_ERROR_H */
