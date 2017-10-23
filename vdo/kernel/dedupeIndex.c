/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/dedupeIndex.c#1 $
 */

#include "dedupeIndex.h"

#include "numeric.h"

#include "udsIndex.h"

// These times are in milliseconds
unsigned int albireoTimeoutInterval  = 5000;
unsigned int minAlbireoTimerInterval = 100;

// These times are in jiffies
Jiffies albireoTimeoutJiffies = 0;
static Jiffies minAlbireoTimerJiffies = 0;

/**********************************************************************/
Jiffies getAlbireoTimeout(Jiffies startJiffies)
{
  return maxULong(startJiffies + albireoTimeoutJiffies,
                  jiffies + minAlbireoTimerJiffies);
}

/**********************************************************************/
void setAlbireoTimeoutInterval(unsigned int value)
{
  // Arbitrary maximum value is two minutes
  if (value > 120000) {
    value = 120000;
  }
  // Arbitrary minimum value is 2 jiffies
  Jiffies albJiffies = msecs_to_jiffies(value);
  if (albJiffies < 2) {
    albJiffies = 2;
    value      = jiffies_to_msecs(albJiffies);
  }
  albireoTimeoutInterval = value;
  albireoTimeoutJiffies  = albJiffies;
}

/**********************************************************************/
void setMinAlbireoTimerInterval(unsigned int value)
{
  // Arbitrary maximum value is one second
  if (value > 1000) {
    value = 1000;
  }

  // Arbitrary minimum value is 2 jiffies
  Jiffies minJiffies = msecs_to_jiffies(value);
  if (minJiffies < 2) {
    minJiffies = 2;
    value = jiffies_to_msecs(minJiffies);
  }

  minAlbireoTimerInterval = value;
  minAlbireoTimerJiffies  = minJiffies;
}

/**********************************************************************/
int makeDedupeIndex(DedupeIndex **indexPtr, KernelLayer *layer)
{
  if (albireoTimeoutJiffies == 0) {
    setAlbireoTimeoutInterval(albireoTimeoutInterval);
  }

  if (minAlbireoTimerJiffies == 0) {
    setMinAlbireoTimerInterval(minAlbireoTimerInterval);
  }

  return makeUDSIndex(layer, indexPtr);
}
