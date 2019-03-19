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
 * $Id: //eng/uds-releases/gloria/src/uds/zone.c#1 $
 */

#include "zone.h"

#include "featureDefs.h"
#include "logger.h"
#include "parameter.h"
#include "permassert.h"
#include "stringUtils.h"
#include "threads.h"

static const NumericValidationData validRange = {
  .minValue = 1,
  .maxValue = MAX_ZONES,
};

/**********************************************************************/
static unsigned int getDefaultZoneCount(void)
{
  unsigned int zoneCount = getNumCores();
  if (zoneCount < validRange.minValue) {
    zoneCount = validRange.minValue;
  }
  if (zoneCount > validRange.maxValue) {
    zoneCount = validRange.maxValue;
  }
  return zoneCount;
}

/**********************************************************************/
static UdsParameterValue getZoneCountInitialValue(void)
{
  UdsParameterValue value;

#if ENVIRONMENT
  char *env = getenv(UDS_PARALLEL_FACTOR);
  if (env != NULL) {
    UdsParameterValue tmp = {
      .type = UDS_PARAM_TYPE_STRING,
      .value.u_string = *env ? env : "true",
    };
    if (validateNumericRange(&tmp, &validRange, &value) == UDS_SUCCESS) {
      return value;
    }
  }
#endif // ENVIRONMENT

  value.type = UDS_PARAM_TYPE_UNSIGNED_INT;
  value.value.u_uint = getDefaultZoneCount();
  return value;
}

/**********************************************************************/
int defineParallelFactor(ParameterDefinition *pd)
{
  pd->validate       = validateNumericRange;
  pd->validationData = &validRange;
  pd->currentValue   = getZoneCountInitialValue();
  return UDS_SUCCESS;
}

/**********************************************************************/
unsigned int getZoneCount(void)
{
  unsigned int zoneCount = 0;

  UdsParameterValue value;
  if ((udsGetParameter(UDS_PARALLEL_FACTOR, &value) == UDS_SUCCESS) &&
      value.type == UDS_PARAM_TYPE_UNSIGNED_INT)
  {
    zoneCount = value.value.u_uint;
  }

  if (zoneCount == 0) {
    zoneCount = getDefaultZoneCount();
  }

  logInfo("Using %u indexing zone%s for concurrency.", zoneCount,
          zoneCount == 1 ? "" : "s");
  return zoneCount;
}

/**********************************************************************/
int setZoneCount(unsigned int count)
{
  int result = udsSetParameter(UDS_PARALLEL_FACTOR, udsUnsignedValue(count));
  if (result != UDS_SUCCESS) {
    return logErrorWithStringError(result,
                                   "cannot set %s to %u",
                                   UDS_PARALLEL_FACTOR, count);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
void resetZoneCount(void)
{
  int result = udsResetParameter(UDS_PARALLEL_FACTOR);
  if (result != UDS_SUCCESS) {
    logErrorWithStringError(result, "cannot reset %s", UDS_PARALLEL_FACTOR);
  }
}
