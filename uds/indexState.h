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
 * $Id: //eng/uds-releases/lisa/src/uds/indexState.h#2 $
 */

#ifndef INDEX_STATE_H
#define INDEX_STATE_H 1

#include "buffer.h"
#include "indexComponent.h"


/**
 * Used here and in SingleFileLayout.
 **/
enum index_save_type {
	IS_SAVE,
	NO_SAVE = 9999,
};

/*
 * Used in get_state_index_state_buffer to identify whether the index state
 * buffer is for the index being loaded or the index being saved.
 */
enum io_access_mode {
	IO_READ = 0x1,
	IO_WRITE = 0x2,
};

/**
 * The index state structure controls the loading and saving of the index
 * state.
 **/
struct index_state {
	struct index_layout *layout;
	unsigned int zone_count;           // number of index zones to use
	unsigned int load_zones;
	unsigned int load_slot;
	unsigned int save_slot;
	unsigned int count;                // count of registered entries
					   // (<= length)
	unsigned int length;               // total span of array allocation
	struct index_component *entries[]; // array of index component entries
};

/**
 * Make an index state object,
 *
 * @param [in]  layout          The index layout.
 * @param [in]  num_zones       The number of zones to use.
 * @param [in]  max_components  The maximum number of components to be handled.
 * @param [out] state_ptr       Where to store the index state object.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check make_index_state(struct index_layout *layout,
				  unsigned int num_zones,
				  unsigned int max_components,
				  struct index_state **state_ptr);

/**
 * Free an index state (generically).
 *
 * @param state  The index state to be freed
 **/
void free_index_state(struct index_state *state);

/**
 * Add an index component to an index state.
 *
 * @param state     The index directory in which to add this component.
 * @param info      The index component file specification.
 * @param data      The per-component data structure.
 * @param context   The load/save context of the component.
 *
 * @return          UDS_SUCCESS or an error code.
 **/
int __must_check
add_index_state_component(struct index_state *state,
			  const struct index_component_info *info,
			  void *data,
			  void *context);

/**
 * Load index state
 *
 * @param state       The index state.
 *
 * @return            UDS_SUCCESS or error
 **/
int __must_check load_index_state(struct index_state *state);

/**
 * Save the current index state, including the open chapter.
 *
 * @param state         The index state.
 *
 * @return              UDS_SUCCESS or error
 **/
int __must_check save_index_state(struct index_state *state);

/**
 *  Prepare to save the index state.
 *
 *  @param state  the index state
 *
 *  @return UDS_SUCCESS or an error code
 **/
int __must_check prepare_to_save_index_state(struct index_state *state);

/**
 * Remove or disable the index state data, for testing.
 *
 * @param state         The index state
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note the return value of this function is frequently ignored
 **/
int discard_index_state_data(struct index_state *state);

/**
 * Find index component, for testing.
 *
 * @param state The index state
 * @param info  The index component file specification
 *
 * @return      The index component, or NULL if not found
 **/
struct index_component *__must_check
find_index_component(const struct index_state *state,
		     const struct index_component_info *info);

/**
 * Get the index state buffer for a specified mode.
 *
 * @param state      The index state.
 * @param mode       One of IO_READ or IO_WRITE.
 *
 * @return the index state buffer
 **/
struct buffer *__must_check
get_state_index_state_buffer(struct index_state *state,
			     enum io_access_mode mode);

/**
 * Open a buffered reader for a specified state, kind, and zone.
 * This helper function is used by index_component.
 *
 * @param state       The index state.
 * @param kind        The kind of index save region to open.
 * @param zone        The zone number for the region.
 * @param reader_ptr  Where to store the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_state_buffered_reader(struct index_state *state,
			   enum region_kind kind,
			   unsigned int zone,
			   struct buffered_reader **reader_ptr);

/**
 * Open a buffered writer for a specified state, kind, and zone.
 * This helper function is used by index_component.
 *
 * @param state       The index state.
 * @param kind        The kind of index save region to open.
 * @param zone        The zone number for the region.
 * @param writer_ptr  Where to store the buffered writer.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
open_state_buffered_writer(struct index_state *state,
			   enum region_kind kind,
			   unsigned int zone,
			   struct buffered_writer **writer_ptr);

#endif // INDEX_STATE_H
