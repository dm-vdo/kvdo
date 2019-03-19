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
 * $Id: //eng/uds-releases/gloria/src/public/uds-param.h#1 $
 */

/**
 * @file
 * @brief Run-time parameter API
 **/

#ifndef UDS_PARAM_H
#define UDS_PARAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "uds-platform.h"

/**
 * The parameter mechanism can be used to specify run-time parameters at
 * the start of processing. Typically these parameters are set prior to
 * the first call to udsCreateLocalIndex, udsLoadLocalIndex,
 * udsRebuildLocalIndex, or udsAttachGridIndex. Once an index session is
 * created several of the parameters are fixed with respect to that index
 * session.
 *
 * Only the main thread creating the index sessions should call the
 * mutative operations to set or reset parameter values, as the effect when
 * called from multiple threads is undefined.
 *
 * A call to udsShutdown has the effect of resetting all parameters to
 * default values.
 **/

/**
 * Enumeration which specifies the different types of parameter values.
 **/
typedef enum udsParameterType {
  UDS_PARAM_TYPE_UNSPECIFIED,
  UDS_PARAM_TYPE_BOOL,
  UDS_PARAM_TYPE_UNSIGNED_INT,
  UDS_PARAM_TYPE_STRING,
} UdsParameterType;

/**
 * Union used to store the various values.
 **/
typedef union {
  bool          u_bool;
  unsigned int  u_uint;
  const char   *u_string;
} UdsValueUnion;

/**
 * A parameter value consists of a type and the value union.
 **/
typedef struct udsParameterValue {
  UdsParameterType type;
  UdsValueUnion    value;
} UdsParameterValue;

/**
 * Predefined true and false constant parameter values.
 **/
extern const UdsParameterValue UDS_PARAM_TRUE;
extern const UdsParameterValue UDS_PARAM_FALSE;

/**
 * Parameters we currently support:
 *
 * UDS_LOG_LEVEL
 *      UINT                                                    [LOG_INFO]
 *      STRING          "EMERG", "ALERT", "CRIT", "ERR",
 *                      "WARNING", "NOTICE", "INFO", "DEBUG"
 *                      (not case sensitive)
 *      The maximum log level we will use. Although stored as an unsigned int,
 *      the validation function will accept strings as well. This parameter
 *      may be changed at any time.
 *
 * UDS_PARALLEL_FACTOR
 *      UNSIGNED INT    1-16                                    [see below]
 *      STRING          "[number]"
 *      The degree of parallelism (number of index zones) that will be used
 *      to process requests. The default value is one less than the maximum
 *      number of CPUs on those platforms where that can be determined, or
 *      one if it cannot be. Although stored as an unsigned int, the
 *      validation function will accept strings as well. This parameter affect
 *      how local index sessions operate.
 *
 * UDS_VOLUME_READ_THREADS
 *      UNSIGNED INT    1-16                                    [2]
 *      STRING          "[number]"
 *      The number of threads used to read chapters.  Although stored as an
 *      unsigned int, the validation function will accept strings as well.
 *      This parameter affect how local index sessions operate.
 **/

/**
 * Set a run-time parameter prior to accessing an index.
 *
 * A change to a parameter value will not have an effect on contexts or
 * index sessions which have already been created, therefore best practice
 * is to set them as early as possible before the index sessions are
 * started. These parameters are not persistent across program or system
 * restarts.
 *
 * @param name          The parameter name.
 * @param value         The parameter value.
 *
 * @return UDS_SUCCESS or an error code, particularly
 *         UDS_UNKNOWN_PARAMETER if the name is not known,
 *         UDS_BAD_PARAMETER_TYPE if the type is incorrect
 *         UDS_PARAMETER_INVALID if the value is not valid for the parameter
 *
 * @note The bytes of a string-valued parameter value will be copied into
 *       private memory.
 **/
int udsSetParameter(const char *name, UdsParameterValue  value)
  __attribute__((warn_unused_result));

/**
 * Get the value of a specified run-time parameter.
 *
 * @param [in]  name    The parameter name.
 * @param [out] value   The parameter's current value. If this value is
 *                        of type UDS_PARAM_TYPE_STRING it is only valid
 *                        until the next call to udsSetParameter().
 *
 * @return UDS_SUCCESS or an error code, particularly
 *         UDS_UNKNOWN_PARAMETER if the name is not known.
 **/
int udsGetParameter(const char *name, UdsParameterValue *value)
  __attribute__((warn_unused_result));

/**
 * Reset the value of the specified parameter to the recomputed default value.
 *
 * As with setting the parameter value, this may not have an effect on
 * contexts or index sessions which have already been created.
 *
 * @param name          The parameter name.
 *
 * @return UDS_SUCCESS or an error code, particularly
 *         UDS_UNKNOWN_PARAMETER if the name is not known.
 **/
int udsResetParameter(const char *name)
  __attribute__((warn_unused_result));

/**
 * Iterate over the available parameters.
 *
 * @param [in]  index   An iteration index. The currently defined parameters
 *                        are mapped to the integer range 0 .. N-1.
 * @param [out] name    The name of the parameter. The pointer is set to
 *                        data which is valid until the next call to
 *                        udsSetParameter().
 * @param [out] value   The current value of the parameter. If the value
 *                        is a string, it is only valid until the next call
 *                        to udsSetParameter().
 *
 * @return UDS_SUCCESS or an error code, particularly
 *         UDS_UNKNOWN_PARAMETER if the index is not valid.
 *
 * @note the typical usage is <code>
 *      for (unsigned int i = 0; ; ++i) {
 *          const char *name;
 *          UdsParameterValue value;
 *          int result = udsIterateParameter(i, &name, &value);
 *          if (result == UDS_UNKNOWN_PARAMETER) {
 *              break;
 *          } else if (result != UDS_SUCCESS) {
 *              // handle the error
 *          }
 *          ...
 *      }
 * </code>
 **/
int udsIterateParameter(unsigned int        index,
                        const char        **name,
                        UdsParameterValue  *value)
  __attribute__((warn_unused_result));

/**
 * Convenience function to make a numeric parameter value.
 *
 * @param number        An unsigned int value
 *
 * @return the value wrapped as a UdsParameterValue
 **/
UdsParameterValue udsUnsignedValue(unsigned int number);

/**
 * Convenience function to make a string parameter value.
 *
 * @param string        A string value
 *
 * @return the value wrapped as a UdsParameterValue
 **/
UdsParameterValue udsStringValue(const char *string);

/**
 * Convenience function to make a boolean parameter value.
 *
 * @param boolean       An boolean value
 *
 * @return the value wrapped as a UdsParameterValue
 **/
UdsParameterValue udsBooleanValue(bool boolean);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UDS_PARAM_H */
