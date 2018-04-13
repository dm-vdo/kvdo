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
 * $Id: //eng/uds-releases/gloria/src/uds/indexComponent.c#1 $
 */

#include "indexComponentInternal.h"

#include "compiler.h"
#include "errors.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "typeDefs.h"

/**********************************************************************/
int initIndexComponent(IndexComponent           *component,
                       const IndexComponentInfo *info,
                       unsigned int              zoneCount,
                       void                     *data,
                       void                     *context,
                       const IndexComponentOps  *ops)
{
  if ((info == NULL) || (info->name == NULL)) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "invalid component or directory specified");
  }
  if (info->loader == NULL) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "no .loader function specified "
                                   "for component %s",
                                   info->name);
  }
  if ((info->saver == NULL) && (info->incremental == NULL)) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "neither .saver function nor .incremental "
                                   "function specified for component %s",
                                   info->name);
  }

  component->info          = info;
  component->componentData = data;
  component->context       = context;
  component->numZones      = (component->info->multiZone ? zoneCount : 1);
  component->writeZones    = NULL;
  component->ops           = ops;

  return UDS_SUCCESS;
}

/*****************************************************************************/
void destroyIndexComponent(IndexComponent *component)
{
  if (component == NULL) {
    return;
  }
  if (component->writeZones) {
    component->ops->freeZones(component);
  }
}

/**
 * Destroy, deallocate, and expunge a read portal.
 *
 * @param readPortal     the readzone array
 **/
void destroyReadPortal(ReadPortal *readPortal)
{
  if (readPortal != NULL) {
    for (unsigned int z = 0; z < readPortal->zones; ++z) {
      if (readPortal->readers[z]) {
        freeBufferedReader(readPortal->readers[z]);
      }
      if (readPortal->regions[z]) {
        closeIORegion(&readPortal->regions[z]);
      }
    }
    FREE(readPortal->readers);
    FREE(readPortal->regions);
  }
}

/*****************************************************************************/
int initReadPortal(ReadPortal     *portal,
                   IndexComponent *component,
                   unsigned int    readZones)
{
  int result = ALLOCATE(readZones, IORegion *, "read zone IO regions",
                        &portal->regions);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ALLOCATE(readZones, BufferedReader *, "read zone buffered readers",
                    &portal->readers);
  if (result != UDS_SUCCESS) {
    FREE(portal->regions);
    return result;
  }
  portal->component = component;
  portal->zones = readZones;
  return UDS_SUCCESS;
}

/*****************************************************************************/
int getComponentSizeForPortal(ReadPortal   *portal,
                              unsigned int  part,
                              off_t        *size)
{
  if (part >= portal->zones) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "%s: cannot access zone %u of %u",
                                   __func__, part, portal->zones);
  }
  if (portal->regions[part] == NULL) {
    return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                   "%s: ioregion for zone %u not available",
                                   __func__, part);
  }
  return getRegionDataSize(portal->regions[part], size);
}

/*****************************************************************************/
int getBufferedReaderForPortal(ReadPortal      *portal,
                               unsigned int     part,
                               BufferedReader **readerPtr)
{
  if (part >= portal->zones) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "%s: cannot access zone %u of %u",
                                   __func__, part, portal->zones);
  }
  if (portal->readers[part] == NULL) {
    if (portal->regions[part] == NULL) {
      return logErrorWithStringError(UDS_UNEXPECTED_RESULT,
                                     "%s: ioregion for zone %u not available",
                                     __func__, part);
    }
    int result = makeBufferedReader(portal->regions[part],
                                    &portal->readers[part]);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "%s: cannot make buffered reader "
                                     "for zone %u", __func__, part);
    }
  }
  *readerPtr = portal->readers[part];
  return UDS_SUCCESS;
}

/*****************************************************************************/
int readIndexComponent(IndexComponent *component)
{
  ReadPortal *readPortal = NULL;
  int result = component->ops->createReadPortal(component, &readPortal);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = (*component->info->loader)(readPortal);
  component->ops->freeReadPortal(readPortal);
  return result;
}

/**
 * Determine the writeZone structure for the specified component and zone.
 *
 * @param [in]  component      the index component
 * @param [in]  zone           the zone number
 * @param [out] writeZonePtr   the resulting write zone instance
 *
 * @return UDS_SUCCESS or an error code
 **/
static int resolveWriteZone(const IndexComponent  *component,
                            unsigned int           zone,
                            WriteZone            **writeZonePtr)
{
  int result = ASSERT(writeZonePtr != NULL,
                      "output parameter is null");
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (component->writeZones == NULL) {
    return logErrorWithStringError(UDS_BAD_STATE,
                                   "cannot resolve index component write zone:"
                                   " not allocated");
  }

  if (zone >= component->numZones) {
    return logErrorWithStringError(UDS_INVALID_ARGUMENT,
                                   "cannot resolve index component write zone:"
                                   " zone out of range");
  }
  *writeZonePtr = component->writeZones[zone];
  return UDS_SUCCESS;
}

/**
 * Non-incremental save function used to emulate a regular save
 * using an incremental save function as a basis.
 *
 * @param component    the index component
 * @param writer       the buffered writer
 * @param zone         the zone number
 *
 * @return UDS_SUCCESS or an error code
 **/
static int indexComponentSaverIncrementalWrapper(IndexComponent *component,
                                                 BufferedWriter *writer,
                                                 unsigned int    zone)
{
  IncrementalWriter incrFunc  = component->info->incremental;
  bool              completed = false;

  int result = (*incrFunc)(component, writer, zone, IWC_START, &completed);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (!completed) {
    result = (*incrFunc)(component, writer, zone, IWC_FINISH, &completed);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  result = flushBufferedWriter(writer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/**
 * Specify that writing to a specific zone file has finished.
 *
 * If a syncer has been registered with the index component, the file
 * descriptor will be enqueued upon it for fsyncing and closing.
 * If not, or if the enqueue fails, the file will be fsynced and closed
 * immediately.
 *
 * @param writeZone    the index component write zone
 *
 * @return UDS_SUCCESS or an error code
 **/
static int doneWithZone(WriteZone *writeZone)
{
  const IndexComponent *component = writeZone->component;
  int                   result    = UDS_SUCCESS;

  if (writeZone->writer != NULL) {
    result = flushBufferedWriter(writeZone->writer);
    if (result != UDS_SUCCESS) {
      return logErrorWithStringError(result,
                                     "cannot flush buffered writer for "
                                     "%s component (zone %u)",
                                     component->info->name, writeZone->zone);
    }
  }
  if (writeZone->region == NULL) {
    return UDS_SUCCESS;
  }
  return syncAndCloseRegion(&writeZone->region,
                            "done with index component write zone");
}

/*****************************************************************************/
void destroyWriteZone(WriteZone *writeZone)
{
  if (writeZone == NULL) {
    return;
  }
  freeBufferedWriter(writeZone->writer);
  closeIORegion(&writeZone->region);
  writeZone->writer = NULL;
}

/*****************************************************************************/
void freeWriteZonesHelper(IndexComponent  *component,
                          void           (*zoneFreeFunc)(WriteZone *))
{
  if (component->writeZones != NULL) {
    for (unsigned int z = 0; z < component->numZones; ++z) {
      WriteZone *wz = component->writeZones[z];
      if (wz == NULL) {
        continue;
      }
      destroyWriteZone(wz);
      if (zoneFreeFunc != NULL) {
        zoneFreeFunc(component->writeZones[z]);
      } else {
        FREE(wz);
      }
    }
    FREE(component->writeZones);
    component->writeZones = NULL;
  }
}

/**
 * Construct the array of WriteZone instances for this component.
 *
 * @param component    the index component
 *
 * @return UDS_SUCCESS or an error code
 *
 * If this is a multizone component, each zone will be fully defined,
 * otherwise zone 0 stands in for the single state file.
 **/
static int makeWriteZones(IndexComponent *component)
{
  if (component->writeZones != NULL) {
    // just reinitialize states
    for (unsigned int z = 0; z < component->numZones; ++z) {
      WriteZone *wz = component->writeZones[z];
      wz->phase = IWC_IDLE;
    }
    return UDS_SUCCESS;
  }

  int result = ALLOCATE(component->numZones, WriteZone *,
                        "index component write zones",
                        &component->writeZones);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = component->ops->populateZones(component);
  if (result != UDS_SUCCESS) {
    component->ops->freeZones(component);
    return result;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
static int openBufferedWriters(IndexComponent *component)
{
  int result = UDS_SUCCESS;

  for (WriteZone **wzp = component->writeZones;
       wzp < component->writeZones + component->numZones;
       ++wzp)
  {
    WriteZone *wz = *wzp;
    wz->phase = IWC_START;

    result = ASSERT(wz->region == NULL, "write zone region already exists");
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = ASSERT(wz->writer == NULL, "write zone writer already exists");
    if (result != UDS_SUCCESS) {
      return result;
    }

    result = component->ops->openWriteRegion(wz);
    if (result != UDS_SUCCESS) {
      break;
    }

    result = makeBufferedWriter(wz->region, 0, &wz->writer);
    if (result != UDS_SUCCESS) {
      closeIORegion(&wz->region);
      return result;
    }
  }

  return result;
}

/*****************************************************************************/
static int startIndexComponentSave(IndexComponent *component)
{
  int result = makeWriteZones(component);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = component->ops->prepareZones(component, component->numZones);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = openBufferedWriters(component);
  if (result != UDS_SUCCESS) {
    return result;
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
int startIndexComponentIncrementalSave(IndexComponent *component)
{
  return startIndexComponentSave(component);
}

/*****************************************************************************/
int writeIndexComponent(IndexComponent *component)
{
  Saver saver = component->info->saver;
  if ((saver == NULL) && (component->info->incremental != NULL)) {
    saver = indexComponentSaverIncrementalWrapper;
  }

  int result = startIndexComponentSave(component);
  if (result != UDS_SUCCESS) {
    return result;
  }

  for (unsigned int z = 0; z < component->numZones; ++z) {
    WriteZone *writeZone = component->writeZones[z];

    result = (*saver)(component, writeZone->writer, z);
    if (result != UDS_SUCCESS) {
      break;
    }

    result = doneWithZone(writeZone);
    if (result != UDS_SUCCESS) {
      break;
    }

    freeBufferedWriter(writeZone->writer);
    writeZone->writer = NULL;
  }

  if (result != UDS_SUCCESS) {
    component->ops->freeZones(component);
    component->ops->cleanupWrite(component);
    return logErrorWithStringError(result, "index component write failed");
  }

  return UDS_SUCCESS;
}

/**
 * Close a specific buffered writer in a component write zone.
 *
 * @param writeZone    the write zone
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note closing a buffered writer causes its file descriptor to be
 *       passed to doneWithZone
 **/
static int closeBufferedWriter(WriteZone *writeZone)
{
  if (writeZone->writer == NULL) {
    return UDS_SUCCESS;
  }

  int result = doneWithZone(writeZone);
  freeBufferedWriter(writeZone->writer);
  writeZone->writer = NULL;

  return result;
}

/**
 * Faux incremental saver function for index components which only define
 * a simple saver.  Conforms to IncrementalWriter signature.
 *
 * @param [in]  component      the index component
 * @param [in]  writer         the buffered writer that does the output
 * @param [in]  zone           the zone number
 * @param [in]  command        the incremental writer command
 * @param [out] completed      if non-NULL, set to whether the save is complete
 *
 * @return UDS_SUCCESS or an error code
 *
 * @note This wrapper always calls the non-incremental saver when
 *       the IWC_START command is issued, and always reports that
 *       the save is complete unless the saver failed.
 **/
static int wrapSaverAsIncremental(IndexComponent           *component,
                                  BufferedWriter           *writer,
                                  unsigned int              zone,
                                  IncrementalWriterCommand  command,
                                  bool                     *completed)
{
  int result = UDS_SUCCESS;

  if ((command >= IWC_START) && (command <= IWC_FINISH)) {
    result = (*component->info->saver)(component, writer, zone);
    if (result == UDS_SUCCESS) {
      noteBufferedWriterUsed(writer);
    }
  }
  if ((result == UDS_SUCCESS) && (completed != NULL)) {
    *completed = true;
  }
  return result;
}

/**
 * Return the appropriate incremental writer function depending on
 * the component's type and whether this is the first zone.
 *
 * @param component    the index component
 *
 * @return the correct IncrementalWriter function to use, or
 *         NULL signifying no progress can be made at this time.
 **/
static IncrementalWriter getIncrementalWriter(IndexComponent *component)
{
  IncrementalWriter incrFunc = component->info->incremental;

  if (incrFunc == NULL) {
    incrFunc = &wrapSaverAsIncremental;
  }

  return incrFunc;
}

/*****************************************************************************/
int performIndexComponentZoneSave(IndexComponent   *component,
                                  unsigned int      zone,
                                  CompletionStatus *completed)
{
  CompletionStatus comp = CS_NOT_COMPLETED;

  WriteZone *wz = NULL;
  int result = resolveWriteZone(component, zone, &wz);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if (wz->phase == IWC_IDLE) {
    comp = CS_COMPLETED_PREVIOUSLY;
  } else if (wz->phase == IWC_DONE) {
    comp = CS_JUST_COMPLETED;
    wz->phase = IWC_IDLE;
  } else if (!component->info->chapterSync) {
    bool done = false;
    IncrementalWriter incrFunc = getIncrementalWriter(component);
    int result = (*incrFunc)(component, wz->writer, zone, wz->phase, &done);
    if (result != UDS_SUCCESS) {
      if (wz->phase == IWC_ABORT) {
        wz->phase = IWC_IDLE;
      } else {
        wz->phase = IWC_ABORT;
      }
      return result;
    }
    if (done) {
      comp = CS_JUST_COMPLETED;
      wz->phase = IWC_IDLE;
    } else if (wz->phase == IWC_START) {
      wz->phase = IWC_CONTINUE;
    }
  }

  if (completed != NULL) {
    *completed = comp;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int performIndexComponentChapterWriterSave(IndexComponent *component)
{
  WriteZone *wz = NULL;
  int result = resolveWriteZone(component, 0, &wz);
  if (result != UDS_SUCCESS) {
    return result;
  }

  if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
    bool done = false;
    IncrementalWriter incrFunc = getIncrementalWriter(component);
    int result = ASSERT(incrFunc != NULL, "no writer function");
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = (*incrFunc)(component, wz->writer, 0, wz->phase, &done);
    if (result != UDS_SUCCESS) {
      if (wz->phase == IWC_ABORT) {
        wz->phase = IWC_IDLE;
      } else {
        wz->phase = IWC_ABORT;
      }
      return result;
    }
    if (done) {
      wz->phase = IWC_DONE;
    } else if (wz->phase == IWC_START) {
      wz->phase = IWC_CONTINUE;
    }
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int finishIndexComponentZoneSave(IndexComponent   *component,
                                 unsigned int      zone,
                                 CompletionStatus *completed)
{
  WriteZone *wz = NULL;
  int result = resolveWriteZone(component, zone, &wz);
  if (result != UDS_SUCCESS) {
    return result;
  }

  CompletionStatus comp;
  switch (wz->phase) {
    case IWC_IDLE:
      comp = CS_COMPLETED_PREVIOUSLY;
      break;

    case IWC_DONE:
      comp = CS_JUST_COMPLETED;
      break;

    default:
      comp = CS_NOT_COMPLETED;
  }

  IncrementalWriter incrFunc = getIncrementalWriter(component);
  if ((wz->phase >= IWC_START) && (wz->phase < IWC_ABORT)) {
    bool done = false;
    int result = (*incrFunc)(component, wz->writer, zone, IWC_FINISH, &done);
    if (result != UDS_SUCCESS) {
      wz->phase = IWC_ABORT;
      return result;
    }
    if (!done) {
      logWarning("finish incremental save did not complete for %s zone %u",
                 component->info->name, zone);
      return UDS_CHECKPOINT_INCOMPLETE;
    }
    wz->phase = IWC_IDLE;
    comp = CS_JUST_COMPLETED;
  }

  if (completed != NULL) {
    *completed = comp;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int finishIndexComponentIncrementalSave(IndexComponent *component)
{
  for (unsigned int zone = 0; zone < component->numZones; ++zone) {
    WriteZone *wz = component->writeZones[zone];
    IncrementalWriter incrFunc = getIncrementalWriter(component);
    if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
      // Note: this is only safe if no other threads are currently processing
      // this particular index
      bool done = false;
      int result = (*incrFunc)(component, wz->writer, zone, IWC_FINISH, &done);
      if (result != UDS_SUCCESS) {
        return result;
      }
      if (!done) {
        logWarning("finishing incremental save did not complete for %s zone %u",
                   component->info->name, zone);
        return UDS_UNEXPECTED_RESULT;
      }
      wz->phase = IWC_IDLE;
    }

    if ((wz->writer != NULL) && !wasBufferedWriterUsed(wz->writer)) {
      return logErrorWithStringError(UDS_CHECKPOINT_INCOMPLETE,
                                     "component %s zone %u did not get written",
                                     component->info->name, zone);
    }

    int result = closeBufferedWriter(wz);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}

/*****************************************************************************/
int abortIndexComponentZoneSave(IndexComponent   *component,
                                unsigned int      zone,
                                CompletionStatus *status)
{
  WriteZone *wz = NULL;
  int result = resolveWriteZone(component, zone, &wz);
  if (result != UDS_SUCCESS) {
    return result;
  }

  CompletionStatus comp = CS_COMPLETED_PREVIOUSLY;

  IncrementalWriter incrFunc = getIncrementalWriter(component);
  if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
    result = (*incrFunc)(component, wz->writer, zone, IWC_ABORT, NULL);
    wz->phase = IWC_IDLE;
    if (result != UDS_SUCCESS) {
      return result;
    }
    comp = CS_JUST_COMPLETED;
  }

  if (status != NULL) {
    *status = comp;
  }
  return UDS_SUCCESS;
}

/*****************************************************************************/
int abortIndexComponentIncrementalSave(IndexComponent *component)
{
  int result = UDS_SUCCESS;

  for (unsigned int zone = 0; zone < component->numZones; ++zone) {
    WriteZone *wz = component->writeZones[zone];
    IncrementalWriter incrFunc = getIncrementalWriter(component);
    if ((wz->phase != IWC_IDLE) && (wz->phase != IWC_DONE)) {
      // Note: this is only safe if no other threads are currently processing
      // this particular index
      result = (*incrFunc)(component, wz->writer, zone, IWC_ABORT, NULL);
      wz->phase = IWC_IDLE;
      if (result != UDS_SUCCESS) {
        return result;
      }
    }

    int result = closeBufferedWriter(wz);
    if (result != UDS_SUCCESS) {
      return result;
    }
  }

  return UDS_SUCCESS;
}
