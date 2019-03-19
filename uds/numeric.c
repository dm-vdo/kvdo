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
 * $Id: //eng/uds-releases/gloria/src/uds/numeric.c#1 $
 */

#include "numeric.h"
#include "permassert.h"

#define STATIC_ASSERT_ALIGNOF(type, expectedAlignment) \
  STATIC_ASSERT(__alignof__(type) == (expectedAlignment))

/**********************************************************************/
uint64_t greatestCommonDivisor(uint64_t a, uint64_t b)
{
  const uint64_t threshold = (1 << 13);
  if ((a < threshold) && (b < threshold)) {
    // use Euclid's subtractive algorithm
    while (a != b) {
      if (a > b) {
        a = a - b;
      } else {
        b = b - a;
      }
    }
  } else {
    // use Euclid's remainder algorithm
    while (b != 0) {
      uint64_t t = b;
      b = a % t;
      a = t;
    }
  }
  return a;
}

/**********************************************************************/
uint64_t leastCommonMultiple(uint64_t a, uint64_t b)
{
  return a / greatestCommonDivisor(a, b) * b;
}

/**********************************************************************/
bool multiplyWouldOverflow(uint64_t a, uint64_t b)
{
  return b != 0 && a > UINT64_MAX / b;
}

/**********************************************************************/
void numericCompileTimeAssertions(void)
{
  STATIC_ASSERT_SIZEOF(uint64_t, 8);
  STATIC_ASSERT_SIZEOF(uint32_t, 4);
  STATIC_ASSERT_SIZEOF(uint16_t, 2);

  STATIC_ASSERT_SIZEOF(UNALIGNED_WRAPPER(uint64_t), 8);
  STATIC_ASSERT_SIZEOF(UNALIGNED_WRAPPER(uint32_t), 4);
  STATIC_ASSERT_SIZEOF(UNALIGNED_WRAPPER(uint16_t), 2);

  STATIC_ASSERT_ALIGNOF(UNALIGNED_WRAPPER(uint64_t), 1);
  STATIC_ASSERT_ALIGNOF(UNALIGNED_WRAPPER(uint32_t), 1);
  STATIC_ASSERT_ALIGNOF(UNALIGNED_WRAPPER(uint16_t), 1);
}
