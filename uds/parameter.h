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
 * $Id: //eng/uds-releases/gloria/src/uds/parameter.h#1 $
 */

#ifndef PARAMETER_H
#define PARAMETER_H

#include "typeDefs.h"
#include "uds-param.h"

/**
 * A parameter definition stores the name, the current value (which includes
 * the type), and several function pointers:
 *
 *   The validate function (if defined) is used to check and convert the
 *   value when a set occurs, and the validationData field is optional extra
 *   information for that function. Often the validate function is also used
 *   to convert environment variable values (which are always strings) to
 *   the appropriate parameter type (numbers or booleans).
 *
 *   The reset function is used to reset the value to default and if not
 *   specified is automatically defined as specified below.
 *
 *   The update function (if defined) is used to notify module that the
 *   value has been updated.
 *
 * Note that currently all string-type parameter values must be statically
 * allocated.
 **/
typedef struct parameterDefinition ParameterDefinition;

typedef int ValidationFunc(const UdsParameterValue *input,
                           const void              *validationData,
                           UdsParameterValue       *output);

struct parameterDefinition {
  const char         *name;             // must be unique
  UdsParameterValue   currentValue;
  const void         *validationData;   // may be arbitrary or null
  ValidationFunc     *validate;
  int               (*reset)(ParameterDefinition *);
  void              (*update)(const UdsParameterValue *);
};

extern const char * const UDS_PARALLEL_FACTOR;
extern const char * const UDS_VOLUME_READ_THREADS;
extern const char * const UDS_PARAMETER_TEST_PARAM;

/**
 * A parameter definition function is responsible for populating most of
 * the entries in the ParameterDefinition structure. The name is pre-filled
 * to a constant value before the function is called. If not set, the
 * reset function will be set to the function itself.
 *
 * The function may return UDS_UNKNOWN_PARAMETER to cause the parameter
 * to be excluded from the active parameter table. Assuming it returns
 * successfully the parameter is installed and its current value is
 * available.
 *
 * These functions are called when the parameter system is initialized or
 * upon first use of the parameter system. In addition, they can be called
 * when udsResetParameter is invoked to recompute the default value, or
 * uninstall or install a parameter if the return value of the definition
 * function is different from the previous time it was called (this feature
 * is used in testing).
 **/

extern int defineParallelFactor(ParameterDefinition *pd);
extern int defineVolumeReadThreads(ParameterDefinition *pd);
extern int setTestParameterDefinitionFunc(int (*func)(ParameterDefinition *))
  __attribute__((warn_unused_result));

/**
 * Validate a string as a boolean value.
 *
 * @param input         A string or boolean value
 * @param validData     Unused for this function
 * @param output        A boolean value.
 *
 * @return UDS_SUCCESS, UDS_BAD_PARAMETER_TYPE, or UDS_PARAMETER_INVALID.
 **/
int validateBoolean(const UdsParameterValue *input,
                    const void              *validData, // not used
                    UdsParameterValue       *output)
  __attribute__((warn_unused_result));

/**
 * ValidationData structure for the validateNumericRange() function.
 **/
typedef struct numericValidationData {
  unsigned int minValue;                ///< the mininum allowed value
  unsigned int maxValue;                ///< the maximum allowed value
} NumericValidationData;

/**
 * Helpful validation function for unsigned integers within a range
 * specified by validationInfo being an instance of NumericValidationInfo.
 *
 * @param input         The input, either string or numeric
 * @param validData     The validation info cast to void
 * @param output        Where to put the output
 *
 * @return UDS_SUCCESS, UDS_BAD_PARAMETER_TYPE, or UDS_PARAMETER_INVALID
 **/
int validateNumericRange(const UdsParameterValue *input,
                         const void              *validData,
                         UdsParameterValue       *output)
  __attribute__((warn_unused_result));

#endif // PARAMETER_H
