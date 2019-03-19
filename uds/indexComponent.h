/*
 * Copyright (c) 2018 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/gloria/src/uds/indexComponent.h#1 $
 */

#ifndef INDEX_COMPONENT_H
#define INDEX_COMPONENT_H 1

#include "common.h"

#include "bufferedReader.h"
#include "bufferedWriter.h"
#include "compiler.h"
#include "regionIdentifiers.h"

typedef struct indexComponent IndexComponent;

typedef enum completionStatus {
  CS_NOT_COMPLETED,             // operation has not completed
  CS_JUST_COMPLETED,            // operation just completed
  CS_COMPLETED_PREVIOUSLY       // operation completed previously
} CompletionStatus;

typedef struct readPortal {
  IndexComponent  *component;
  IORegion       **regions;
  BufferedReader **readers;
  unsigned int     zones;
} ReadPortal;

/**
 * Prototype for functions which can load an index component from its
 * saved state.
 *
 * @param portal        A component portal which can be used to load the
 *                        specified component.
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*Loader)(ReadPortal *portal);

/**
 * Prototype for functions which can save an index component.
 *
 * @param component     The index component.
 * @param writer        A buffered writer.
 * @param zone          The zone number.
 *
 * @return UDS_SUCCESS or an error code
 **/
typedef int (*Saver)(IndexComponent *component,
                     BufferedWriter *writer,
                     unsigned int    zone);

/**
 * Command code used by IncrementalWriter function protocol.
 **/
typedef enum incrementalWriterCommand {
  IWC_START,    //< start an incremental save
  IWC_CONTINUE, //< continue an incremental save
  IWC_FINISH,   //< force finish of incremental save
  IWC_ABORT,    //< abort incremental save
  IWC_IDLE = -1,//< not a command, used internally to signify not in progress
  IWC_DONE = -2 //< not a command, used internally to signify async completion
} IncrementalWriterCommand;

/**
 * @param [in]  component       The index component.
 * @param [in]  writer          A buffered writer.
 * @param [in]  zone            The zone number (0 for non-multi-zone).
 * @param [in]  command         The incremental writer command.
 * @param [out] completed       If non-NULL, set to whether save is done.
 *
 * @return      UDS_SUCCESS or an error code
 **/
typedef int (*IncrementalWriter)(IndexComponent           *component,
                                 BufferedWriter           *writer,
                                 unsigned int              zone,
                                 IncrementalWriterCommand  command,
                                 bool                     *completed);

/**
 * The structure describing how to load or save an index component.
 * At least one of saver or incremental must be specified.
 **/
typedef struct {
  RegionKind          kind;         //< Region kind
  const char         *name;         //< The name of the component (for logging)
  const char         *fileName;     //< The name of the file for the component
  bool                saveOnly;     //< Used for saves but not checkpoints
  bool                chapterSync;  //< Saved by the chapter writer
  bool                multiZone;    //< Does this component have multiple zones?
  Loader              loader;       //< The function load this component
  Saver               saver;        //< The function to store this component
  IncrementalWriter   incremental;  //< The function for incremental writing
} IndexComponentInfo;

/**
 * Destroy and index component.
 *
 * @param componentPtr  A pointer to the component to be freed.
 **/
static inline void freeIndexComponent(IndexComponent **componentPtr);

/**
 * Return the index component name for this component.
 **/
static inline const char *indexComponentName(IndexComponent *component);

/**
 * Return the index component data for this component.
 **/
static inline void *indexComponentData(IndexComponent *component);

/**
 * Return the index component context for this component.
 **/
static inline void *indexComponentContext(IndexComponent *component);

/**
 * Return the index component name for this portal.
 **/
static inline const char *componentNameForPortal(ReadPortal *portal);

/**
 * Return the index component data for this portal.
 **/
static inline void *componentDataForPortal(ReadPortal *portal);

/**
 * Return the index component context for this portal.
 **/
static inline void *componentContextForPortal(ReadPortal *portal);

/**
 * Determine whether this component may be skipped for a checkpoint.
 *
 * @param component     the component,
 *
 * @return whether the component may be skipped
 **/
static inline bool skipIndexComponentOnCheckpoint(IndexComponent *component);

/**
 * Determine whether actual saving during a checkpoint should be
 * invoked by the chapter writer thread.
 **/
static inline bool
deferIndexComponentCheckpointToChapterWriter(IndexComponent *component);

/**
 * Determine whether a replay is required if component is missing.
 *
 * @param component     the component
 *
 * @return whether the component is final (that is, contains shutdown state)
 **/
static inline bool
missingIndexComponentRequiresReplay(IndexComponent *component);

/**
 * Read a component's state.
 *
 * @param component  The component to read.
 *
 * @return UDS_SUCCESS, an error code from reading, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int readIndexComponent(IndexComponent *component)
  __attribute__((warn_unused_result));

/**
 * Write a state file.
 *
 * @param component  The component to write
 *
 * @return UDS_SUCCESS, an error code from writing, or UDS_INVALID_ARGUMENT
 *         if the component is NULL.
 **/
int writeIndexComponent(IndexComponent *component)
  __attribute__((warn_unused_result));

/**
 * Start an incremental save for this component (all zones).
 *
 * @param [in] component        The index component.
 *
 * @return      UDS_SUCCESS or an error code.
 **/
int startIndexComponentIncrementalSave(IndexComponent *component)
  __attribute__((warn_unused_result));

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
 int performIndexComponentZoneSave(IndexComponent   *component,
                                   unsigned int      zone,
                                   CompletionStatus *completed)
  __attribute__((warn_unused_result));

/**
 * Perform an incremental save for a non-multizone component synchronized
 * with the chapter writer.
 *
 * @param component     The index component.
 **/
int performIndexComponentChapterWriterSave(IndexComponent *component)
  __attribute__((warn_unused_result));

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
int finishIndexComponentZoneSave(IndexComponent   *component,
                                 unsigned int      zone,
                                 CompletionStatus *completed)
  __attribute__((warn_unused_result));

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
int finishIndexComponentIncrementalSave(IndexComponent *component)
  __attribute__((warn_unused_result));

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
int abortIndexComponentZoneSave(IndexComponent   *component,
                                unsigned int      zone,
                                CompletionStatus *completed)
  __attribute__((warn_unused_result));

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
int abortIndexComponentIncrementalSave(IndexComponent *component)
  __attribute__((warn_unused_result));

/**
 * Remove or invalidate component state.
 *
 * @param component  The component whose file is to be removed.  If NULL
 *                   no action is taken.
 **/
static inline int discardIndexComponent(IndexComponent *component)
  __attribute__((warn_unused_result));

/**
 * Count the number of parts for this portal.
 *
 * @param [in]  portal          The component portal.
 *
 * @return UDS_SUCCESS or an error code.
 **/
__attribute__((warn_unused_result))
static INLINE unsigned int countPartsForPortal(ReadPortal *portal)
{
  return portal->zones;
}

/**
 * Get the size of the saved component part image.
 *
 * @param [in]  portal          The component portal.
 * @param [in]  part            The component ordinal number.
 * @param [out] size            The size of the component image.
 *
 * @return UDS_SUCCESS or an error code.
 *
 * @note This is only supported by some types of portals and only if the
 *       component was previously saved.
 **/
__attribute__((warn_unused_result))
int getComponentSizeForPortal(ReadPortal   *portal,
                              unsigned int  part,
                              off_t        *size);

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
__attribute__((warn_unused_result))
int getBufferedReaderForPortal(ReadPortal      *portal,
                               unsigned int     part,
                               BufferedReader **readerPtr);

#define INDEX_COMPONENT_INLINE
#include "indexComponentInline.h"
#undef INDEX_COMPONENT_INLINE

#endif /* INDEX_COMPONENT_H */
