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
 * $Id: //eng/uds-releases/krusty/src/uds/indexComponent.h#6 $
 */

#ifndef INDEX_COMPONENT_H
#define INDEX_COMPONENT_H 1

#include "common.h"

#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "regionIdentifiers.h"

enum completion_status {
  CS_NOT_COMPLETED,             // operation has not completed
  CS_JUST_COMPLETED,            // operation just completed
  CS_COMPLETED_PREVIOUSLY       // operation completed previously
};

struct read_portal {
  struct index_component  *component;
  struct buffered_reader **readers;
  unsigned int             zones;
};

/**
 * Prototype for functions which can load an index component from its
 * saved state.
 *
 * @param portal        A component portal which can be used to load the
 *                        specified component.
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*Loader)(struct read_portal *portal);

/**
 * Prototype for functions which can save an index component.
 *
 * @param component     The index component.
 * @param writer        A buffered writer.
 * @param zone          The zone number.
 *
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*Saver)(struct index_component *component,
                     struct buffered_writer *writer,
                     unsigned int            zone);

/**
 * Command code used by IncrementalWriter function protocol.
 **/
enum incremental_writer_command {
  IWC_START,    //< start an incremental save
  IWC_CONTINUE, //< continue an incremental save
  IWC_FINISH,   //< force finish of incremental save
  IWC_ABORT,    //< abort incremental save
  IWC_IDLE = -1,//< not a command, used internally to signify not in progress
  IWC_DONE = -2 //< not a command, used internally to signify async completion
};

struct write_zone {
  struct index_component          *component;
  enum incremental_writer_command  phase;
  struct buffered_writer          *writer;
  unsigned int                     zone;
};

/**
 * @param [in]  component       The index component.
 * @param [in]  writer          A buffered writer.
 * @param [in]  zone            The zone number (0 for non-multi-zone).
 * @param [in]  command         The incremental writer command.
 * @param [out] completed       If non-NULL, set to whether save is done.
 *
 * @return      UDS_SUCCESS or an error code
 **/
typedef int (*IncrementalWriter)(struct index_component          *component,
                                 struct buffered_writer          *writer,
                                 unsigned int                     zone,
                                 enum incremental_writer_command  command,
                                 bool                            *completed);

/**
 * The structure describing how to load or save an index component.
 * At least one of saver or incremental must be specified.
 **/
struct index_component_info {
  RegionKind         kind;        // Region kind
  const char        *name;        // The name of the component (for logging)
  bool               saveOnly;    // Used for saves but not checkpoints
  bool               chapterSync; // Saved by the chapter writer
  bool               multiZone;   // Does this component have multiple zones?
  bool               ioStorage;   // Do we do I/O directly to storage?
  Loader             loader;      // The function load this component
  Saver              saver;       // The function to store this component
  IncrementalWriter  incremental; // The function for incremental writing
};

/**
 * The structure representing a savable (and loadable) part of an index.
 **/
struct index_component {
  const struct index_component_info  *info;           // index_component_info
                                                      // specification
  void                               *componentData;  // The object to load or
                                                      // save
  void                               *context;        // The context used to
                                                      // load or save
  struct index_state                 *state;          // The index state
  unsigned int                        numZones;       // Number of zones in
                                                      // write portal
  struct write_zone                 **writeZones;     // State for writing
                                                      // component
};

/**
 * Make an index component
 *
 * @param state         The index state in which this component instance
 *                        shall reside.
 * @param info          The component info specification for this component.
 * @param zoneCount     How many active zones are in use.
 * @param data          Component-specific data.
 * @param context       Component-specific context.
 * @param componentPtr  Where to store the resulting component.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check makeIndexComponent(struct index_state *state,
				    const struct index_component_info *info,
				    unsigned int zoneCount,
				    void *data,
				    void *context,
				    struct index_component **componentPtr);

/**
 * Destroy and index component.
 *
 * @param componentPtr  A pointer to the component to be freed.
 **/
void freeIndexComponent(struct index_component **componentPtr);

/**
 * Return the index component name for this component.
 **/
static INLINE const char *indexComponentName(struct index_component *component)
{
  return component->info->name;
}

/**
 * Return the index component data for this component.
 **/
static INLINE void *indexComponentData(struct index_component *component)
{
  return component->componentData;
}

/**
 * Return the index component context for this component.
 **/
static INLINE void *indexComponentContext(struct index_component *component)
{
  return component->context;
}

/**
 * Determine whether this component may be skipped for a checkpoint.
 *
 * @param component     the component,
 *
 * @return whether the component may be skipped
 **/
static INLINE bool
skipIndexComponentOnCheckpoint(struct index_component *component)
{
  return component->info->saveOnly;
}

/**
 * Determine whether actual saving during a checkpoint should be
 * invoked by the chapter writer thread.
 **/
static INLINE bool
deferIndexComponentCheckpointToChapterWriter(struct index_component *component)
{
  return component->info->chapterSync;
}

/**
 * Determine whether a replay is required if component is missing.
 *
 * @param component     the component
 *
 * @return whether the component is final (that is, contains shutdown state)
 **/
static INLINE bool
missingIndexComponentRequiresReplay(struct index_component *component)
{
  return component->info->saveOnly;
}

/**
 * Read a component's state.
 *
 * @param component  The component to read.
 *
 * @return UDS_SUCCESS, an error code from reading, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int __must_check readIndexComponent(struct index_component *component);

/**
 * Write a state file.
 *
 * @param component  The component to write
 *
 * @return UDS_SUCCESS, an error code from writing, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int __must_check writeIndexComponent(struct index_component *component);

/**
 * Start an incremental save for this component (all zones).
 *
 * @param [in] component        The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
int __must_check
startIndexComponentIncrementalSave(struct index_component *component);

/**
 * Perform an incremental save for a component in a particular zone.
 *
 * @param [in]  component       The index component.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Pointer to hold completion status result.
 *
 * @return      UDS_SUCCESS or an error code.
 *
 * @note        If an incremental save is not supported, a regular
 *              save will be performed if this is the first call in zone 0.
 **/
int __must_check
performIndexComponentZoneSave(struct index_component *component,
			      unsigned int zone,
			      enum completion_status *completed);

/**
 * Perform an incremental save for a non-multizone component synchronized
 * with the chapter writer.
 *
 * @param component     The index component.
 **/
int __must_check
performIndexComponentChapterWriterSave(struct index_component *component);

/**
 * Force the completion of an incremental save currently in progress in
 * a particular zone.
 *
 * @param [in]  component       The index component.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Pointer to hold completion status result.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
int __must_check
finishIndexComponentZoneSave(struct index_component *component,
			     unsigned int zone,
			     enum completion_status *completed);

/**
 * Force the completion of an incremental save in all zones and complete
 * the overal save.
 *
 * @param [in]  component       The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 *
 * @note        If all zones call finishIndexComponentZoneSave first, only
 *              the common non-index-related completion code is required,
 *              which protects access to the index data structures from the
 *              invoking thread.
 **/
int __must_check
finishIndexComponentIncrementalSave(struct index_component *component);

/**
 * Abort the incremental save currently in progress in a particular zone.
 *
 * @param [in]  component       The index component.
 * @param [in]  zone            The zone number.
 * @param [out] completed       Pointer to hold completion status result.
 *
 * @return      UDS_SUCCESS or an error code.
 *
 * @note        "Completed" in this case means completed or aborted.
 *              Once any zone calls this function the entire save is
 *              useless unless every zone indicates CS_COMPLETED_PREVIOUSLY.
 **/
int __must_check
abortIndexComponentZoneSave(struct index_component *component,
			    unsigned int zone,
			    enum completion_status *completed);

/**
 * Abort an incremental save currently in progress
 *
 * @param [in] component        The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 *
 * @note        If all zones call abortIndexComponentZoneSave first, only
 *              the common non-index-related completion code is required,
 *              which protects access to the index data structures from the
 *              invoking thread.
 **/
int __must_check
abortIndexComponentIncrementalSave(struct index_component *component);

/**
 * Remove or invalidate component state.
 *
 * @param component  The component whose file is to be removed.  If NULL
 *                   no action is taken.
 **/
int __must_check discardIndexComponent(struct index_component *component);

/**
 * Get a buffered reader for the specified component part.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] readerPtr       Where to put the buffered reader.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note the reader is managed by the component portal
 **/
int __must_check getBufferedReaderForPortal(struct read_portal *portal,
					    unsigned int part,
					    struct buffered_reader **readerPtr);

#endif /* INDEX_COMPONENT_H */
