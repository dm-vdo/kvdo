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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/stringLinuxKernel.c#8 $
 */

#include <linux/mm.h>

#include "errors.h"
#include "logger.h"
#include "stringUtils.h"

/**********************************************************************/
int uds_string_to_signed_long(const char *nptr, long *num)
{
	while (*nptr == ' ') {
		nptr++;
	}
	return kstrtol(nptr, 10, num) ? UDS_INVALID_ARGUMENT : UDS_SUCCESS;
}

/**********************************************************************/
int uds_string_to_unsigned_long(const char *nptr, unsigned long *num)
{
	while (*nptr == ' ') {
		nptr++;
	}
	if (*nptr == '+') {
		nptr++;
	}
	return kstrtoul(nptr, 10, num) ? UDS_INVALID_ARGUMENT : UDS_SUCCESS;
}

/**********************************************************************/
char *uds_next_token(char *str, const char *delims, char **state)
{
	char *ep, *sp = str ? str : *state;
	while (*sp && strchr(delims, *sp)) {
		++sp;
	}
	if (!*sp) {
		return NULL;
	}
	ep = sp;
	while (*ep && !strchr(delims, *ep)) {
		++ep;
	}
	if (*ep) {
		*ep++ = '\0';
	}
	*state = ep;
	return sp;
}

/**********************************************************************/
int uds_parse_uint64(const char *str, uint64_t *num)
{
	unsigned long value = *num;
	int result = uds_string_to_unsigned_long(str, &value);
	*num = value;
	return result;
}
