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
 * $Id: //eng/uds-releases/gloria/src/uds/parameter.c#1 $
 */

#include "parameter.h"

#include "common.h"
#include "memoryAlloc.h"
#include "stringUtils.h"
#include "threadOnce.h"
#include "typeDefs.h"
#include "uds.h"

const UdsParameterValue UDS_PARAM_TRUE = {
  .type = UDS_PARAM_TYPE_BOOL,
  .value.u_bool = true,
};

const UdsParameterValue UDS_PARAM_FALSE = {
  .type = UDS_PARAM_TYPE_BOOL,
  .value.u_bool = false,
};

const char *const UDS_PARALLEL_FACTOR      = "UDS_PARALLEL_FACTOR";
const char *const UDS_VOLUME_READ_THREADS  = "UDS_VOLUME_READ_THREADS";
const char *const UDS_PARAMETER_TEST_PARAM = "UDS_PARAMETER_TEST_PARAM";

static int defineParameterTestParam(ParameterDefinition *);

static const struct paramDef {
  const char * const *name;
  int               (*func)(ParameterDefinition *pd);
} definitions[] = {
  { &UDS_PARALLEL_FACTOR,         defineParallelFactor        },
  { &UDS_VOLUME_READ_THREADS,     defineVolumeReadThreads     },
  { &UDS_PARAMETER_TEST_PARAM,    defineParameterTestParam    },
};

static OnceState paramOnce  = ONCE_STATE_INITIALIZER;

/**
 * The table of parameters.
 **/
static struct {
  size_t              count;
  ParameterDefinition params[COUNT_OF(definitions)];
} paramTable;

/*****************************************************************************/
static bool installParamDef(const struct paramDef *pdef)
{
  if (paramTable.count >= COUNT_OF(definitions)) {
    return false;
  }
  ParameterDefinition *pd = paramTable.params + paramTable.count;
  memset(pd, 0, sizeof(*pd));
  pd->name = *pdef->name;
  int result = pdef->func(pd);
  if ((result == UDS_SUCCESS) &&
      (pd->currentValue.type != UDS_PARAM_TYPE_UNSPECIFIED)) {
    if (pd->reset == NULL) {
      pd->reset = pdef->func;
    }
    ++paramTable.count;
    return true;
  }
  return false;
}

/*****************************************************************************/
static void initParameters(void)
{
  // note that this early in initialization, we cannot log errors reliably
  memset(&paramTable, 0, sizeof(paramTable));
  for (unsigned int i = 0; i < COUNT_OF(definitions); ++i) {
    installParamDef(&definitions[i]);
  }
}

/*****************************************************************************/
static int callInit(void)
{
  return performOnce(&paramOnce, initParameters);
}

/*****************************************************************************/
int udsSetParameter(const char *name, UdsParameterValue value)
{
  int result = callInit();
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = UDS_UNKNOWN_PARAMETER;
  for (ParameterDefinition *pd = paramTable.params;
       pd < paramTable.params + paramTable.count;
       ++pd)
  {
    if (name == pd->name || strcmp(name, pd->name) == 0) {
      if (pd->validate != NULL) {
        result = pd->validate(&value, pd->validationData, &pd->currentValue);
      } else if (pd->currentValue.type != value.type) {
        result = UDS_BAD_PARAMETER_TYPE;
      } else {
        pd->currentValue = value;
        result = UDS_SUCCESS;
      }
      if ((result == UDS_SUCCESS) && (pd->update != NULL)) {
        pd->update(&pd->currentValue);
      }
      break;
    }
  }
  return result;
}

/*****************************************************************************/
int udsGetParameter(const char *name, UdsParameterValue *value)
{
  int result = callInit();
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = UDS_UNKNOWN_PARAMETER;
  for (ParameterDefinition *pd = paramTable.params;
       pd < paramTable.params + paramTable.count;
       ++pd)
  {
    if (name == pd->name || strcmp(name, pd->name) == 0) {
      if (value != NULL) {
        *value = pd->currentValue;
      }
      result = UDS_SUCCESS;
      break;
    }
  }
  return result;
}

/*****************************************************************************/
int udsIterateParameter(unsigned int        index,
                        const char        **name,
                        UdsParameterValue  *value)
{
  int result = callInit();
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = UDS_UNKNOWN_PARAMETER;
  if (index < paramTable.count) {
    ParameterDefinition *pd = &paramTable.params[index];
    if (name != NULL) {
      *name = pd->name;
    }
    if (value != NULL) {
      *value = pd->currentValue;
    }
    result = UDS_SUCCESS;
  }
  return result;
}

/*****************************************************************************/
int udsResetParameter(const char *name)
{
  int result = callInit();
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = UDS_UNKNOWN_PARAMETER;
  for (ParameterDefinition *pd = paramTable.params;
       pd < paramTable.params + paramTable.count;
       ++pd)
  {
    if (name == pd->name || strcmp(name, pd->name) == 0) {
      ParameterDefinition copy = *pd;
      result = copy.reset(&copy);
      if (result == UDS_SUCCESS) {
        *pd = copy;
        if (pd->update != NULL) {
          pd->update(&pd->currentValue);
        }
      } else if (result == UDS_UNKNOWN_PARAMETER) {
        // delete the parameter from the active table
        while (&pd[1] < paramTable.params + paramTable.count) {
          *pd = pd[1];
          ++pd;
        }
        --paramTable.count;
        result = UDS_SUCCESS;
      }
      break;
    }
  }
  if (result == UDS_UNKNOWN_PARAMETER) {
    for (unsigned int i = 0; i < COUNT_OF(definitions); ++i) {
      if (strcmp(name, *definitions[i].name) == 0) {
        if (installParamDef(&definitions[i])) {
          result = UDS_SUCCESS;
        }
        break;
      }
    }
  }
  return result;
}

/*****************************************************************************/
UdsParameterValue udsUnsignedValue(unsigned int number)
{
  UdsParameterValue value = {
    .type = UDS_PARAM_TYPE_UNSIGNED_INT,
    .value.u_uint = number,
  };
  return value;
}

/*****************************************************************************/
UdsParameterValue udsStringValue(const char *string)
{
  UdsParameterValue value = {
    .type = UDS_PARAM_TYPE_STRING,
    .value.u_string = string,
  };
  return value;
}

/*****************************************************************************/
UdsParameterValue udsBooleanValue(bool boolean)
{
  return boolean ? UDS_PARAM_TRUE : UDS_PARAM_FALSE;
}

/*****************************************************************************/
int validateBoolean(const UdsParameterValue *input,
                    const void              *validData __attribute__((unused)),
                    UdsParameterValue       *output)
{
  if (input->type == UDS_PARAM_TYPE_BOOL) {
    output->type = UDS_PARAM_TYPE_BOOL;
    output->value.u_bool = input->value.u_bool;
    return UDS_SUCCESS;
  } else if (input->type == UDS_PARAM_TYPE_STRING) {
    if (strcasecmp(input->value.u_string, "true") == 0 ||
        strcasecmp(input->value.u_string, "yes") == 0) {
      output->type = UDS_PARAM_TYPE_UNSIGNED_INT;
      output->value.u_bool = true;
      return UDS_SUCCESS;
    } else if (strcasecmp(input->value.u_string, "false") == 0 ||
               strcasecmp(input->value.u_string, "no") == 0) {
      output->type = UDS_PARAM_TYPE_UNSIGNED_INT;
      output->value.u_bool = false;
      return UDS_SUCCESS;
    }
  } else {
    return UDS_BAD_PARAMETER_TYPE;
  }
  return UDS_PARAMETER_INVALID;
}

/*****************************************************************************/
int validateNumericRange(const UdsParameterValue *input,
                         const void              *validData,
                         UdsParameterValue       *output)
{
  const NumericValidationData *range = validData;

  if (input->type == UDS_PARAM_TYPE_UNSIGNED_INT) {
    if (range->minValue <= input->value.u_uint &&
        input->value.u_uint <= range->maxValue) {
      *output = *input;
      return UDS_SUCCESS;
    }
  } else if (input->type == UDS_PARAM_TYPE_STRING) {
    unsigned long n = 0;
    int result = stringToUnsignedLong(input->value.u_string, &n);
    if ((result == UDS_SUCCESS)
        && (range->minValue <= n) && (n <= range->maxValue)) {
      output->type = UDS_PARAM_TYPE_UNSIGNED_INT;
      output->value.u_uint = n;
      return UDS_SUCCESS;
    }
  } else {
    return UDS_BAD_PARAMETER_TYPE;
  }
  return UDS_PARAMETER_INVALID;
}

/*
 * Test Support
 */

static int (*testParamDefFunc)(ParameterDefinition *) = NULL;

/*****************************************************************************/
static int defineParameterTestParam(ParameterDefinition *pd)
{
  if (testParamDefFunc) {
    return testParamDefFunc(pd);
  }
  return UDS_UNKNOWN_PARAMETER;
}

/*****************************************************************************/
int setTestParameterDefinitionFunc(int (*func)(ParameterDefinition *))
{
  testParamDefFunc = func;
  return udsResetParameter(UDS_PARAMETER_TEST_PARAM);
}
