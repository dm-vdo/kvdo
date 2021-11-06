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
 */

#include "vdoStringUtils.h"

#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "stringUtils.h"

#include "status-codes.h"

/**
 * Free a list of non-NULL string pointers, and then the list itself.
 *
 * @param string_array  The string list
 **/
void vdo_free_string_array(char **string_array)
{
	unsigned int offset;

	for (offset = 0; string_array[offset] != NULL; offset++) {
		UDS_FREE(string_array[offset]);
	}
	UDS_FREE(string_array);
}

/**
 * Split the input string into substrings, separated at occurrences of
 * the indicated character, returning a null-terminated list of string
 * pointers.
 *
 * The string pointers and the pointer array itself should both be
 * freed with UDS_FREE() when no longer needed. This can be done with
 * vdo_free_string_array (below) if the pointers in the array are not
 * changed. Since the array and copied strings are allocated by this
 * function, it may only be used in contexts where allocation is
 * permitted.
 *
 * Empty substrings are not ignored; that is, returned substrings may
 * be empty strings if the separator occurs twice in a row.
 *
 * @param [in]  string               The input string to be broken apart
 * @param [in]  separator            The separator character
 * @param [out] substring_array_ptr  The NULL-terminated substring array
 *
 * @return UDS_SUCCESS or -ENOMEM
 **/
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
			/*
			 * Trailing NUL is already in place after allocation; 
			 * deal with the zero or more non-NUL bytes in the 
			 * string.
			 */
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
	/* Process final string, with no trailing separator. */
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
	/* substrings[current_substring] is NULL already */
	*substring_array_ptr = substrings;
	return UDS_SUCCESS;
}

/**
 * Join the input substrings into one string, joined with the indicated
 * character, returning a string.
 *
 * @param [in]  substring_array  The NULL-terminated substring array
 * @param [in]  array_length     A bound on the number of valid elements
 *                               in substring_array, in case it is not
 *                               NULL-terminated.
 * @param [in]  separator        The separator character
 * @param [out] string_ptr       A pointer to hold the joined string
 *
 * @return VDO_SUCCESS or an error
 **/
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
		current_position = uds_append_to_buffer(current_position,
							output + string_length,
							"%s",
							substring_array[i]);
		*current_position = separator;
		current_position++;
	}

	/* We output one too many separators; replace the last with a zero byte. */
	if (current_position != output) {
		*(current_position - 1) = '\0';
	}

	*string_ptr = output;
	return UDS_SUCCESS;
}

/**
 * Parse a string as an "unsigned int" value, yielding the value.
 * On overflow, -ERANGE is returned. On invalid number, -EINVAL is
 * returned.
 *
 * @param [in]  input      The string to be processed
 * @param [out] value_ptr  The value of the number read
 *
 * @return UDS_SUCCESS or -EINVAL or -ERANGE.
 **/
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
