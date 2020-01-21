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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/stringLinuxKernel.c#1 $
 */

#include <linux/mm.h>

#include "errors.h"
#include "logger.h"
#include "stringUtils.h"

/**********************************************************************/
int stringToSignedLong(const char *nptr, long *num)
{
  while (*nptr == ' ') {
    nptr++;
  }
  return kstrtol(nptr, 10, num) ? UDS_INVALID_ARGUMENT : UDS_SUCCESS;
}

/**********************************************************************/
int stringToUnsignedLong(const char *nptr, unsigned long *num)
{
  while (*nptr == ' ') {
    nptr++;
  }
  if (*nptr == '+') {
    nptr++;
  }
  return kstrtoul(nptr, 10, num) ? UDS_INVALID_ARGUMENT : UDS_SUCCESS;
}

/*****************************************************************************/
char *nextToken(char *str, const char *delims, char **state)
{
  char *sp = str ? str : *state;
  while (*sp && strchr(delims, *sp)) {
    ++sp;
  }
  if (!*sp) {
    return NULL;
  }
  char *ep = sp;
  while (*ep && !strchr(delims, *ep)) {
    ++ep;
  }
  if (*ep) {
    *ep++ = '\0';
  }
  *state = ep;
  return sp;
}

/*****************************************************************************/
int parseUint64(const char *str, uint64_t *num)
{
  unsigned long value = *num;
  int result = stringToUnsignedLong(str, &value);
  *num = value;
  return result;
}
