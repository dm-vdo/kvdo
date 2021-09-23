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
 * $Id: //eng/uds-releases/lisa/src/uds/udsMain.c#6 $
 */

#include "uds.h"

/* Memory size constants */
const uds_memory_config_size_t UDS_MEMORY_CONFIG_MAX = 1024;
const uds_memory_config_size_t UDS_MEMORY_CONFIG_256MB = 0xffffff00; // -256
const uds_memory_config_size_t UDS_MEMORY_CONFIG_512MB = 0xfffffe00; // -512
const uds_memory_config_size_t UDS_MEMORY_CONFIG_768MB = 0xfffffd00; // -768

/* Memory size constants for volumes that have one less chapter */
const uds_memory_config_size_t UDS_MEMORY_CONFIG_REDUCED = 0x1000;
const uds_memory_config_size_t UDS_MEMORY_CONFIG_REDUCED_MAX = 1024 | 0x1000;
const uds_memory_config_size_t UDS_MEMORY_CONFIG_REDUCED_256MB =
	0xfffffb00; // -1280
const uds_memory_config_size_t UDS_MEMORY_CONFIG_REDUCED_512MB =
	0xfffffa00; // -1536
const uds_memory_config_size_t UDS_MEMORY_CONFIG_REDUCED_768MB =
	0xfffff900; // -1792

/**********************************************************************/
const char *uds_get_version(void)
{
#ifdef UDS_VERSION
	return UDS_VERSION;
#else
	return "internal version";
#endif
}
