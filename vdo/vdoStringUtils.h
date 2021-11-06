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

#ifndef VDO_STRING_UTILS_H
#define VDO_STRING_UTILS_H

#include <linux/types.h>

int __must_check
vdo_split_string(const char *string,
		 char separator,
		 char ***substring_array_ptr);

int __must_check vdo_join_strings(char **substring_array,
				  size_t array_length,
				  char separator,
				  char **string_ptr);

void vdo_free_string_array(char **string_array);

int __must_check
vdo_string_to_uint(const char *input, unsigned int *value_ptr);

#endif /* VDO_STRING_UTILS_H */
