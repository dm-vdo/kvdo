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
 * $Id: //eng/uds-releases/gloria/src/public/uds-error.h#1 $
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
enum udsStatusCodes {
  /** Successful return */
  UDS_SUCCESS      = 0,

  /** Used as a base value for reporting errors  */
  UDS_ERROR_CODE_BASE             = 1024,
  /** The UDS library is not initialized */
  UDS_UNINITIALIZED               = UDS_ERROR_CODE_BASE + 0,
  /** The UDS library is shutting down */
  UDS_SHUTTINGDOWN                = UDS_ERROR_CODE_BASE + 1,
  /** Could not load scanner modules */
  UDS_EMODULE_LOAD                = UDS_ERROR_CODE_BASE + 2,
  /** Could not create a new thread */
  UDS_ENOTHREADS                  = UDS_ERROR_CODE_BASE + 3,
  /** Could not find the specified library context */
  UDS_NOCONTEXT                   = UDS_ERROR_CODE_BASE + 4,
  /** The specified library context is disabled */
  UDS_DISABLED                    = UDS_ERROR_CODE_BASE + 5,
  /** Some saved index component is corrupt */
  UDS_CORRUPT_COMPONENT           = UDS_ERROR_CODE_BASE + 6,
  UDS_CORRUPT_FILE                = UDS_CORRUPT_COMPONENT,
  /** Unknown error */
  UDS_UNKNOWN_ERROR               = UDS_ERROR_CODE_BASE + 7,
  /** The grid configuration contains no servers */
  UDS_GRID_NO_SERVERS             = UDS_ERROR_CODE_BASE + 8,
  /** The grid configuration is inconsistent across servers */
  UDS_GRID_CONFIG_INCONSISTENT    = UDS_ERROR_CODE_BASE + 9,
  /** The index configuration or volume format is no longer supported */
  UDS_UNSUPPORTED_VERSION         = UDS_ERROR_CODE_BASE + 10,
  /** Index session not available */
  UDS_NO_INDEXSESSION             = UDS_ERROR_CODE_BASE + 11,
  /** Index data in memory is corrupt */
  UDS_CORRUPT_DATA                = UDS_ERROR_CODE_BASE + 12,
  /** Short read due to truncated file */
  UDS_SHORT_READ                  = UDS_ERROR_CODE_BASE + 13,
  /** Error determining address info */
  UDS_AI_ERROR                    = UDS_ERROR_CODE_BASE + 14,
  /** Internal resource limits exceeded */
  UDS_RESOURCE_LIMIT_EXCEEDED     = UDS_ERROR_CODE_BASE + 15,
  /** Memory overflow due to storage failure */
  UDS_VOLUME_OVERFLOW             = UDS_ERROR_CODE_BASE + 16,
  /** Block address required */
  UDS_BLOCK_ADDRESS_REQUIRED      = UDS_ERROR_CODE_BASE + 17,
  /** Block data required */
  UDS_CHUNK_DATA_REQUIRED         = UDS_ERROR_CODE_BASE + 18,
  /** Chunk name required */
  UDS_CHUNK_NAME_REQUIRED         = UDS_ERROR_CODE_BASE + 19,
  /** Configuration pointer required */
  UDS_CONF_PTR_REQUIRED           = UDS_ERROR_CODE_BASE + 20,
  /** Index stats pointer required */
  UDS_INDEX_STATS_PTR_REQUIRED    = UDS_ERROR_CODE_BASE + 21,
  /** Context stats pointer required */
  UDS_CONTEXT_STATS_PTR_REQUIRED  = UDS_ERROR_CODE_BASE + 22,
  /** Context pointer required */
  UDS_CONTEXT_PTR_REQUIRED        = UDS_ERROR_CODE_BASE + 23,
  /** File identifier required */
  UDS_FILEID_REQUIRED             = UDS_ERROR_CODE_BASE + 24,
  /** Stream required */
  UDS_STREAM_REQUIRED             = UDS_ERROR_CODE_BASE + 25,
  /** Stream identifier required */
  UDS_STREAMID_REQUIRED           = UDS_ERROR_CODE_BASE + 26,
  /** Stream pointer required */
  UDS_STREAM_PTR_REQUIRED         = UDS_ERROR_CODE_BASE + 27,
  /** Memory configuration not supported */
  UDS_INVALID_MEMORY_SIZE         = UDS_ERROR_CODE_BASE + 28,
  /** Metadata too big */
  UDS_INVALID_METADATA_SIZE       = UDS_ERROR_CODE_BASE + 29,
  /** Index name required */
  UDS_INDEX_NAME_REQUIRED         = UDS_ERROR_CODE_BASE + 30,
  /** Configuration required */
  UDS_CONF_REQUIRED               = UDS_ERROR_CODE_BASE + 31,
  /** Invalid file descriptor */
  UDS_BAD_FILE_DESCRIPTOR         = UDS_ERROR_CODE_BASE + 32,
  /** File already exists */
  UDS_INDEX_EXISTS                = UDS_ERROR_CODE_BASE + 33,
  /** Incorrect arguments to albmigrate */
  UDS_REQUESTS_OUT_OF_RANGE       = UDS_ERROR_CODE_BASE + 34,
  /** Incorrect arguments to albmigrate */
  UDS_BAD_NAMESPACE               = UDS_ERROR_CODE_BASE + 35,
  /** Incorrect arguments to albmigrate */
  UDS_MIGRATOR_MISMATCH           = UDS_ERROR_CODE_BASE + 36,
  /** Essential files for index not found */
  UDS_NO_INDEX                    = UDS_ERROR_CODE_BASE + 37,
  /** Checkpoint frequency out of range */
  UDS_BAD_CHECKPOINT_FREQUENCY    = UDS_ERROR_CODE_BASE + 38,
  /** Wrong type of index configuration */
  UDS_WRONG_INDEX_CONFIG          = UDS_ERROR_CODE_BASE + 39,
  /** Index path does not point to a directory */
  UDS_INDEX_PATH_NOT_DIR          = UDS_ERROR_CODE_BASE + 40,
  /** Open invoked on already opened connection */
  UDS_ALREADY_OPEN                = UDS_ERROR_CODE_BASE + 41,
  /** Callback already registered */
  UDS_CALLBACK_ALREADY_REGISTERED = UDS_ERROR_CODE_BASE + 42,
  /** Index path too long */
  UDS_INDEX_PATH_TOO_LONG         = UDS_ERROR_CODE_BASE + 43,
  /** Premature end of file in scanned file */
  UDS_END_OF_FILE                 = UDS_ERROR_CODE_BASE + 44,
  /** Attempt to access unsaved index */
  UDS_INDEX_NOT_SAVED_CLEANLY     = UDS_ERROR_CODE_BASE + 45,
  /** Attempt to use network when the version has no network */
  UDS_LOCAL_ONLY                  = UDS_ERROR_CODE_BASE + 46,
  /** There is not sufficient space to create the index */
  UDS_INSUFFICIENT_INDEX_SPACE    = UDS_ERROR_CODE_BASE + 47,
  /** The specified offset is not at appropriate alignment */
  UDS_BAD_INDEX_ALIGNMENT         = UDS_ERROR_CODE_BASE + 48,
  /** The code has entered an unsupported code path */
  UDS_UNSUPPORTED                 = UDS_ERROR_CODE_BASE + 49,
  /** The parameter is not defined */
  UDS_UNKNOWN_PARAMETER           = UDS_ERROR_CODE_BASE + 50,
  /** The parameter value type is invalid */
  UDS_BAD_PARAMETER_TYPE          = UDS_ERROR_CODE_BASE + 51,
  /** The parameter value is invalid */
  UDS_PARAMETER_INVALID           = UDS_ERROR_CODE_BASE + 52,
  /** Callback required */
  UDS_CALLBACK_REQUIRED           = UDS_ERROR_CODE_BASE + 53,
  /** Wrong operation type */
  UDS_INVALID_OPERATION_TYPE      = UDS_ERROR_CODE_BASE + 54,
  /** One more than the last UDS_ERROR_CODE */
  UDS_ERROR_CODE_LAST,
  /** One more than this block can use */
  UDS_ERROR_CODE_BLOCK_END = UDS_ERROR_CODE_BASE + 1024
};

#endif /* UDS_ERROR_H */
