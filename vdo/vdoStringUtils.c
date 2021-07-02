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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/vdoStringUtils.c#9 $
 */

#include "vdoStringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "statusCodes.h"

/**********************************************************************/
void vdo_free_string_array(char **string_array)
{
	unsigned int offset;

	for (offset = 0; string_array[offset] != NULL; offset++) {
		UDS_FREE(string_array[offset]);
	}
	UDS_FREE(string_array);
}

/**********************************************************************/
int vdo_split_string(const char *string,
		     char separator,
		     char ***substring_array_ptr)
{
	unsigned int current_substring = 0, substring_count = 1;
	const char *s;
	char **substrings;
	int result;
	ptrdiff_t length;

	for (s = string; *s != 0; s++) {
		if (*s == separator) {
			substring_count++;
		}
	}

	result = UDS_ALLOCATE(substring_count + 1,
			      char *,
			      "string-splitting array",
			      &substrings);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (s = string; *s != 0; s++) {
		if (*s == separator) {
			ptrdiff_t length = s - string;

			result = UDS_ALLOCATE(length + 1,
					      char,
					      "split string",
					      &substrings[current_substring]);
			if (result != UDS_SUCCESS) {
				vdo_free_string_array(substrings);
				return result;
			}
			// Trailing NUL is already in place after allocation;
			// deal with the zero or more non-NUL bytes in the
			// string.
			if (length > 0) {
				memcpy(substrings[current_substring],
				       string,
				       length);
			}
			string = s + 1;
			current_substring++;
			BUG_ON(current_substring >= substring_count);
		}
	}
	// Process final string, with no trailing separator.
	BUG_ON(current_substring != (substring_count - 1));
	length = strlen(string);

	result = UDS_ALLOCATE(length + 1,
			      char,
			      "split string",
			      &substrings[current_substring]);
	if (result != UDS_SUCCESS) {
		vdo_free_string_array(substrings);
		return result;
	}
	memcpy(substrings[current_substring], string, length);
	current_substring++;
	// substrings[current_substring] is NULL already
	*substring_array_ptr = substrings;
	return UDS_SUCCESS;
}

/**********************************************************************/
int vdo_join_strings(char **substring_array, size_t array_length,
		     char separator, char **string_ptr)
{
	size_t string_length = 0;
	size_t i;
	int result;
	char *output, *current_position;

	for (i = 0; (i < array_length) && (substring_array[i] != NULL); i++) {
		string_length += strlen(substring_array[i]) + 1;
	}

	result = UDS_ALLOCATE(string_length, char, __func__, &output);

	if (result != VDO_SUCCESS) {
		return result;
	}

	current_position = &output[0];

	for (i = 0; (i < array_length) && (substring_array[i] != NULL); i++) {
		current_position = append_to_buffer(current_position,
						    output + string_length,
						    "%s",
						    substring_array[i]);
		*current_position = separator;
		current_position++;
	}

	// We output one too many separators; replace the last with a zero byte.
	if (current_position != output) {
		*(current_position - 1) = '\0';
	}

	*string_ptr = output;
	return UDS_SUCCESS;
}

/**********************************************************************/
int vdo_string_to_uint(const char *input, unsigned int *value_ptr)
{
	unsigned long long_value;
	int result = kstrtoul(input, 10, &long_value);

	if (result != 0) {
		return result;
	}

	if (long_value > UINT_MAX) {
		return -ERANGE;
	}

	*value_ptr = long_value;
	return UDS_SUCCESS;
}
