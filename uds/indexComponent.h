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
 * $Id: //eng/uds-releases/lisa/src/uds/indexComponent.h#2 $
 */

#ifndef INDEX_COMPONENT_H
#define INDEX_COMPONENT_H 1

#include "common.h"

#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "regionIdentifiers.h"

enum completion_status {
	CS_NOT_COMPLETED,        // operation has not completed
	CS_JUST_COMPLETED,       // operation just completed
	CS_COMPLETED_PREVIOUSLY  // operation completed previously
};

struct read_portal {
	struct index_component *component;
	struct buffered_reader **readers;
	unsigned int zones;
};

/**
 * Prototype for functions which can load an index component from its
 * saved state.
 *
 * @param portal        A component portal which can be used to load the
 *                        specified component.
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*loader_t)(struct read_portal *portal);

/**
 * Prototype for functions which can save an index component.
 *
 * @param component     The index component.
 * @param writer        A buffered writer.
 * @param zone          The zone number.
 *
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*saver_t)(struct index_component *component,
		       struct buffered_writer *writer,
		       unsigned int zone);

struct write_zone {
	struct index_component *component;
	struct buffered_writer *writer;
	unsigned int zone;
};

/**
 * The structure describing how to load or save an index component.
 **/
struct index_component_info {
	enum region_kind kind; // Region kind
	const char *name;      // The name of the component (for logging)
	bool multi_zone;       // Does this component have multiple zones?
	bool io_storage;       // Do we do I/O directly to storage?
	loader_t loader;       // The function load this component
	saver_t saver;         // The function to store this component
};

/**
 * The structure representing a savable (and loadable) part of an index.
 **/
struct index_component {
	const struct index_component_info *info; // index_component_info
						 // specification
	void *component_data;                    // The object to load or
						 // save
	void *context;                           // The context used to
						 // load or save
	struct index_state *state;               // The index state
	unsigned int num_zones;                  // Number of zones in
						 // write portal
	struct write_zone **write_zones;         // State for writing
						 // component
};

/**
 * Make an index component
 *
 * @param state          The index state in which this component instance
 *                         shall reside.
 * @param info           The component info specification for this component.
 * @param zone_count     How many active zones are in use.
 * @param data           Component-specific data.
 * @param context        Component-specific context.
 * @param component_ptr  Where to store the resulting component.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_index_component(struct index_state *state,
				      const struct index_component_info *info,
				      unsigned int zone_count,
				      void *data,
				      void *context,
				      struct index_component **component_ptr);

/**
 * Destroy an index component.
 *
 * @param component  The component to be freed
 **/
void free_index_component(struct index_component *component);

/**
 * Return the index component name for this component.
 **/
static INLINE const char *
index_component_name(struct index_component *component)
{
	return component->info->name;
}

/**
 * Return the index component data for this component.
 **/
static INLINE void *index_component_data(struct index_component *component)
{
	return component->component_data;
}

/**
 * Return the index component context for this component.
 **/
static INLINE void *index_component_context(struct index_component *component)
{
	return component->context;
}

/**
 * Read a component's state.
 *
 * @param component  The component to read.
 *
 * @return UDS_SUCCESS, an error code from reading, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int __must_check read_index_component(struct index_component *component);

/**
 * Write a state file.
 *
 * @param component  The component to write
 *
 * @return UDS_SUCCESS, an error code from writing, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int __must_check write_index_component(struct index_component *component);

/**
 * Remove or invalidate component state.
 *
 * @param component  The component whose file is to be removed.  If NULL
 *                   no action is taken.
 **/
int __must_check discard_index_component(struct index_component *component);

/**
 * Get a buffered reader for the specified component part.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] reader_ptr      Where to put the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note the reader is managed by the component portal
 **/
int __must_check
get_buffered_reader_for_portal(struct read_portal *portal,
			       unsigned int part,
			       struct buffered_reader **reader_ptr);

#endif /* INDEX_COMPONENT_H */
