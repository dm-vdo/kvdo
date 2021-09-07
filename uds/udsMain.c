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
 * $Id: //eng/uds-releases/lisa/src/uds/udsMain.c#5 $
 */

#include "uds.h"

#include "config.h"
#include "logger.h"
#include "memoryAlloc.h"

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

/*
 * ===========================================================================
 * UDS system management
 * ===========================================================================
 */

/**********************************************************************/
int uds_initialize_configuration(struct uds_configuration **user_config,
				 uds_memory_config_size_t mem_gb)
{
	int result;

	if (user_config == NULL) {
		uds_log_error("missing configuration pointer");
		return -EINVAL;
	}

	result = UDS_ALLOCATE(1, struct uds_configuration, "uds_configuration",
			      user_config);
	if (result != UDS_SUCCESS) {
		return uds_map_to_system_error(result);
	}

	(*user_config)->memory_size = mem_gb;
	return UDS_SUCCESS;
}

/**********************************************************************/
void uds_configuration_set_sparse(struct uds_configuration *user_config,
				  bool sparse)
{
	user_config->sparse = sparse;
}

/**********************************************************************/
bool uds_configuration_get_sparse(struct uds_configuration *user_config)
{
	return user_config->sparse;
}

/**********************************************************************/
void uds_configuration_set_nonce(struct uds_configuration *user_config,
				 uds_nonce_t nonce)
{
	user_config->nonce = nonce;
}

/**********************************************************************/
uds_nonce_t uds_configuration_get_nonce(struct uds_configuration *user_config)
{
	return user_config->nonce;
}

/**********************************************************************/
unsigned int
uds_configuration_get_memory(struct uds_configuration *user_config)
{
	return user_config->memory_size;
}

/**********************************************************************/
void uds_free_configuration(struct uds_configuration *user_config)
{
	UDS_FREE(user_config);
}

/**********************************************************************/
const char *uds_get_version(void)
{
#ifdef UDS_VERSION
	return UDS_VERSION;
#else
	return "internal version";
#endif
}
