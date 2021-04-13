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
 * $Id: //eng/uds-releases/krusty/src/uds/udsMain.c#20 $
 */

#include "uds.h"

#include "config.h"
#include "geometry.h"
#include "indexLayout.h"
#include "indexRouter.h"
#include "indexSession.h"
#include "loadType.h"
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
	if (user_config == NULL) {
		return log_error_strerror(UDS_CONF_PTR_REQUIRED,
					  "received a NULL config pointer");
	}

	/* Set the configuration parameters that change with memory size.  If
	 * you change these values, you should also:
	 *
	 * Change Configuration_x1, which tests these values and expects to see
	 * them
	 *
	 * Bump the index configuration version number.  This bump ensures that
	 * the test infrastructure will be forced to test the new
	 * configuration.
	 */

	unsigned int chapters_per_volume, record_pages_per_chapter;
	if (mem_gb == UDS_MEMORY_CONFIG_256MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME;
		record_pages_per_chapter = SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_512MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME;
		record_pages_per_chapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_768MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME;
		record_pages_per_chapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if ((mem_gb >= 1) && (mem_gb <= UDS_MEMORY_CONFIG_MAX)) {
		chapters_per_volume = mem_gb * DEFAULT_CHAPTERS_PER_VOLUME;
		record_pages_per_chapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_256MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME - 1;
		record_pages_per_chapter = SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_512MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME - 1;
		record_pages_per_chapter = 2 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if (mem_gb == UDS_MEMORY_CONFIG_REDUCED_768MB) {
		chapters_per_volume = DEFAULT_CHAPTERS_PER_VOLUME - 1;
		record_pages_per_chapter = 3 * SMALL_RECORD_PAGES_PER_CHAPTER;
	} else if ((mem_gb >= 1 + UDS_MEMORY_CONFIG_REDUCED) &&
		   (mem_gb <= UDS_MEMORY_CONFIG_REDUCED_MAX)) {
		chapters_per_volume = (mem_gb - UDS_MEMORY_CONFIG_REDUCED) *
			DEFAULT_CHAPTERS_PER_VOLUME - 1;
		record_pages_per_chapter = DEFAULT_RECORD_PAGES_PER_CHAPTER;
	} else {
		return UDS_INVALID_MEMORY_SIZE;
	}

	int result = ALLOCATE(1, struct uds_configuration, "uds_configuration",
			      user_config);
	if (result != UDS_SUCCESS) {
		return result;
	}

	(*user_config)->record_pages_per_chapter = record_pages_per_chapter;
	(*user_config)->chapters_per_volume = chapters_per_volume;
	(*user_config)->sparse_chapters_per_volume =
		DEFAULT_SPARSE_CHAPTERS_PER_VOLUME;
	(*user_config)->cache_chapters = DEFAULT_CACHE_CHAPTERS;
	(*user_config)->checkpoint_frequency = DEFAULT_CHECKPOINT_FREQUENCY;
	(*user_config)->volume_index_mean_delta =
		DEFAULT_VOLUME_INDEX_MEAN_DELTA;
	(*user_config)->bytes_per_page = DEFAULT_BYTES_PER_PAGE;
	(*user_config)->sparse_sample_rate = DEFAULT_SPARSE_SAMPLE_RATE;
	(*user_config)->nonce = 0;
	return UDS_SUCCESS;
}

/**********************************************************************/
void uds_configuration_set_sparse(struct uds_configuration *user_config,
				  bool sparse)
{
	bool prev_sparse = (user_config->sparse_chapters_per_volume != 0);
	if (sparse == prev_sparse) {
		// nothing to do
		return;
	}

	unsigned int prev_chapters_per_volume =
		user_config->chapters_per_volume;
	if (sparse) {
		// Index 10TB with 4K blocks, 95% sparse, fit in dense (1TB)
		// footprint
		user_config->chapters_per_volume =
			10 * prev_chapters_per_volume;
		user_config->sparse_chapters_per_volume =
			9 * prev_chapters_per_volume +
			prev_chapters_per_volume / 2;
		user_config->sparse_sample_rate = 32;
	} else {
		user_config->chapters_per_volume =
			prev_chapters_per_volume / 10;
		user_config->sparse_chapters_per_volume = 0;
		user_config->sparse_sample_rate = 0;
	}
}

/**********************************************************************/
bool uds_configuration_get_sparse(struct uds_configuration *user_config)
{
	return user_config->sparse_chapters_per_volume > 0;
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
	unsigned int memory = 0;
	unsigned int chapters = uds_configuration_get_sparse(user_config) ?
		user_config->chapters_per_volume / 10 :
		user_config->chapters_per_volume;

	if ((chapters % DEFAULT_CHAPTERS_PER_VOLUME) == 0) {
		switch (user_config->record_pages_per_chapter) {
		case SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_256MB;
			break;
		case 2 * SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_512MB;
			break;
		case 3 * SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_768MB;
			break;
		default:
			memory = chapters / DEFAULT_CHAPTERS_PER_VOLUME;
		}
	} else {
		switch (user_config->record_pages_per_chapter) {
		case SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_REDUCED_256MB;
			break;
		case 2 * SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_REDUCED_512MB;
			break;
		case 3 * SMALL_RECORD_PAGES_PER_CHAPTER:
			memory = UDS_MEMORY_CONFIG_REDUCED_768MB;
			break;
		default:
			memory = (chapters + 1) / DEFAULT_CHAPTERS_PER_VOLUME +
				UDS_MEMORY_CONFIG_REDUCED;
		}
	}
	return memory;
}

/**********************************************************************/
unsigned int
uds_configuration_get_chapters_per_volume(struct uds_configuration *user_config)
{
	return user_config->chapters_per_volume;
}

/**********************************************************************/
void uds_free_configuration(struct uds_configuration *user_config)
{
	FREE(user_config);
}

/**********************************************************************/
int uds_create_index_session(struct uds_index_session **session)
{
	if (session == NULL) {
		return UDS_NO_INDEXSESSION;
	}

	struct uds_index_session *index_session = NULL;
	int result = make_empty_index_session(&index_session);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*session = index_session;
	return UDS_SUCCESS;
}

/**********************************************************************/
static int
initialize_index_session_with_layout(struct uds_index_session *index_session,
				     struct index_layout *layout,
				     const struct uds_parameters *user_params,
				     enum load_type load_type)
{
	int result = ((load_type == LOAD_CREATE) ?
			write_index_config(layout,
					   &index_session->user_config) :
			verify_index_config(layout,
					    &index_session->user_config));
	if (result != UDS_SUCCESS) {
		return result;
	}

	struct configuration *index_config;
	result = make_configuration(&index_session->user_config,
				    &index_config);
	if (result != UDS_SUCCESS) {
		log_error_strerror(result, "Failed to allocate config");
		return result;
	}

	// Zero the stats for the new index.
	memset(&index_session->stats, 0, sizeof(index_session->stats));

	result = make_index_router(layout,
				   index_config,
				   user_params,
				   load_type,
				   &index_session->load_context,
				   enter_callback_stage,
				   &index_session->router);
	free_configuration(index_config);
	if (result != UDS_SUCCESS) {
		log_error_strerror(result, "Failed to make router");
		return result;
	}

	log_uds_configuration(&index_session->user_config);
	return UDS_SUCCESS;
}

/**********************************************************************/
static int initialize_index_session(struct uds_index_session *index_session,
				    const char *name,
				    const struct uds_parameters *user_params,
				    enum load_type load_type)
{
	struct index_layout *layout;
	int result = make_index_layout(name,
				       load_type == LOAD_CREATE,
				       &index_session->user_config,
				       &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = initialize_index_session_with_layout(index_session, layout,
						      user_params, load_type);
	put_index_layout(&layout);
	return result;
}

/**********************************************************************/
int uds_open_index(enum uds_open_index_type open_type,
		   const char *name,
		   const struct uds_parameters *user_params,
		   struct uds_configuration *user_config,
		   struct uds_index_session *session)
{
	if (name == NULL) {
		return UDS_INDEX_NAME_REQUIRED;
	}
	if (user_config == NULL) {
		return UDS_CONF_REQUIRED;
	}
	if (session == NULL) {
		return UDS_NO_INDEXSESSION;
	}

	int result = start_loading_index_session(session);
	if (result != UDS_SUCCESS) {
		return result;
	}

	session->user_config = *user_config;

	// Map the external open_type to the internal load_type
	enum load_type load_type =
		open_type == UDS_CREATE ?
			LOAD_CREATE :
			open_type == UDS_NO_REBUILD ? LOAD_LOAD : LOAD_REBUILD;
	log_notice("%s: %s", get_load_type(load_type), name);

	result = initialize_index_session(session, name, user_params,
					  load_type);
	if (result != UDS_SUCCESS) {
		log_error_strerror(result, "Failed %s",
				   get_load_type(load_type));
		save_and_free_index(session);
	}

	finish_loading_index_session(session, result);
	return sans_unrecoverable(result);
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

/**********************************************************************/
const char *uds_string_error(int errnum, char *buf, size_t buflen)
{
	if (buf == NULL) {
		return NULL;
	}

	return string_error(errnum, buf, buflen);
}
