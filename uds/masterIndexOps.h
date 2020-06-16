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
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndexOps.h#12 $
 */

#ifndef MASTERINDEXOPS_H
#define MASTERINDEXOPS_H 1

#include "compiler.h"
#include "deltaIndex.h"
#include "indexComponent.h"
#include "indexConfig.h"
#include "threads.h"
#include "uds.h"

extern const struct index_component_info *const MASTER_INDEX_INFO;
extern unsigned int min_master_index_delta_lists;

struct master_index_stats {
  size_t memoryAllocated;  // Number of bytes allocated
  RelTime rebalanceTime;   // The number of seconds spent rebalancing
  int  rebalanceCount;     // Number of memory rebalances
  long recordCount;        // The number of records in the index
  long collisionCount;     // The number of collision records
  long discardCount;       // The number of records removed
  long overflowCount;      // The number of UDS_OVERFLOWs detected
  unsigned int numLists;   // The number of delta lists
  long earlyFlushes;       // Number of early flushes
};

/*
 * The master_index_triage structure is used by lookupMasterIndexName(),
 * which is a read-only operation that looks at the chunk name and returns
 * some information used by the index to select the thread/queue/code_path
 * that will process the chunk.
 */
struct master_index_triage {
  uint64_t virtualChapter;  // If inSampledChapter is true, then this is the
                            // chapter containing the entry for the chunk name
  unsigned int zone;        // The zone containing the chunk name
  bool isSample;            // If true, this chunk name belongs to the
                            // sampled index
  bool inSampledChapter;    // If true, this chunk already has an entry in the
                            // sampled index and virtualChapter is valid
};

/*
 * The master_index_record structure is used for normal index read-write
 * processing of a chunk name.  The first call must be to
 * getMasterIndexRecord() to find the master index record for a chunk name.
 * This call can be followed by put_master_index_record() to add a master
 * index record, or by set_master_index_record_chapter() to associate the chunk
 * name with a different chapter, or by remove_master_index_record() to delete
 * a master index record.
 */
struct master_index_record {
  // Public fields
  uint64_t virtualChapter;  // Chapter where the block info is found
  bool     isCollision;     // This record is a collision
  bool     isFound;         // This record is the block searched for

  // Private fields
  unsigned char        magic;           // The magic number for valid records
  unsigned int         zoneNumber;      // Zone that contains this block
  struct master_index *masterIndex;     // The master index
  Mutex               *mutex;           // Mutex that must be held while accessing
                                        // this delta index entry; used only for
                                        // a sampled index; otherwise is NULL
  const struct uds_chunk_name *name;    // The blockname to which this record refers
  struct delta_index_entry deltaEntry;  // The delta index entry for this record
};

struct master_index {
  void (*abortRestoringMasterIndex)(struct master_index *masterIndex);
  int (*abortSavingMasterIndex)(const struct master_index *masterIndex,
                                unsigned int zoneNumber);
  int (*finishSavingMasterIndex)(const struct master_index *masterIndex,
                                 unsigned int zoneNumber);
  void (*freeMasterIndex)(struct master_index *masterIndex);
  size_t (*getMasterIndexMemoryUsed)(const struct master_index *masterIndex);
  int (*getMasterIndexRecord)(struct master_index *masterIndex,
                              const struct uds_chunk_name *name,
                              struct master_index_record *record);
  void (*getMasterIndexStats)(const struct master_index *masterIndex,
                              struct master_index_stats *dense,
                              struct master_index_stats *sparse);
  unsigned int (*getMasterIndexZone)(const struct master_index *masterIndex,
                                     const struct uds_chunk_name *name);
  bool (*isMasterIndexSample)(const struct master_index *masterIndex,
                              const struct uds_chunk_name *name);
  bool (*isRestoringMasterIndexDone)(const struct master_index *masterIndex);
  bool (*isSavingMasterIndexDone)(const struct master_index *masterIndex,
                                  unsigned int zoneNumber);
  int (*lookupMasterIndexName)(const struct master_index *masterIndex,
                               const struct uds_chunk_name *name,
                               struct master_index_triage *triage);
  int (*lookupMasterIndexSampledName)(const struct master_index *masterIndex,
                                      const struct uds_chunk_name *name,
                                      struct master_index_triage *triage);
  int (*restoreDeltaListToMasterIndex)(struct master_index *masterIndex,
                                       const struct delta_list_save_info *dlsi,
                                       const byte data[DELTA_LIST_MAX_BYTE_COUNT]);
  void (*setMasterIndexOpenChapter)(struct master_index *masterIndex,
                                    uint64_t virtualChapter);
  void (*setMasterIndexTag)(struct master_index *masterIndex, byte tag);
  void (*setMasterIndexZoneOpenChapter)(struct master_index *masterIndex,
                                        unsigned int zoneNumber,
                                        uint64_t virtualChapter);
  int (*startRestoringMasterIndex)(struct master_index *masterIndex,
                                   struct buffered_reader **bufferedReaders,
                                   int numReaders);
  int (*startSavingMasterIndex)(const struct master_index *masterIndex,
                                unsigned int zoneNumber,
                                struct buffered_writer *bufferedWriter);
};

/**
 * Return the combined master index stats.
 *
 * @param masterIndex The master index
 * @param stats       Combined stats for the index
 **/
void getMasterIndexCombinedStats(const struct master_index *masterIndex,
                                 struct master_index_stats *stats);

/**
 * Make a new master index.
 *
 * @param config       The configuration of the master index
 * @param numZones     The number of zones
 * @param volumeNonce  The nonce used to store the index
 * @param masterIndex  Location to hold new master index ptr
 *
 * @return error code or UDS_SUCCESS
 **/
int __must_check makeMasterIndex(const struct configuration *config,
				 unsigned int numZones,
				 uint64_t volumeNonce,
				 struct master_index **masterIndex);

/**
 * Compute the number of blocks required to save a master index of a given
 * configuration.
 *
 * @param [in]  config          The configuration of a master index
 * @param [in]  blockSize       The size of a block in bytes.
 * @param [out] blockCount      The resulting number of blocks.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int __must_check
computeMasterIndexSaveBlocks(const struct configuration *config,
			     size_t blockSize,
			     uint64_t *blockCount);

/**
 * Restore a master index.  This is exposed for unit tests.
 *
 * @param readers      The readers to read from.
 * @param numReaders   The number of readers.
 * @param masterIndex  The master index
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
int __must_check restoreMasterIndex(struct buffered_reader **readers,
				    unsigned int numReaders,
				    struct master_index *masterIndex);

/**
 * Abort restoring a master index from an input stream.
 *
 * @param masterIndex  The master index
 **/
static INLINE void abortRestoringMasterIndex(struct master_index *masterIndex)
{
  masterIndex->abortRestoringMasterIndex(masterIndex);
}

/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
abortSavingMasterIndex(const struct master_index *masterIndex,
                       unsigned int zoneNumber)
{
  return masterIndex->abortSavingMasterIndex(masterIndex, zoneNumber);
}

/**
 * Finish saving a master index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
finishSavingMasterIndex(const struct master_index *masterIndex,
                        unsigned int zoneNumber)
{
  return masterIndex->finishSavingMasterIndex(masterIndex, zoneNumber);
}

/**
 * Terminate and clean up the master index
 *
 * @param masterIndex The master index to terminate
 **/
static INLINE void freeMasterIndex(struct master_index *masterIndex)
{
  masterIndex->freeMasterIndex(masterIndex);
}

/**
 * Get the number of bytes used for master index entries.
 *
 * @param masterIndex The master index
 *
 * @return The number of bytes in use
 **/
static INLINE size_t
getMasterIndexMemoryUsed(const struct master_index *masterIndex)
{
  return masterIndex->getMasterIndexMemoryUsed(masterIndex);
}

/**
 * Find the master index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * master index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If isFound is false, then we did not find an entry for the block name.
 * Information is saved in the master_index_record so that
 * put_master_index_record() will insert an entry for that block name at the
 * proper place.
 *
 * If isFound is true, then we did find an entry for the block name.
 * Information is saved in the master_index_record so that the "chapter" and
 * "isCollision" fields reflect the entry found.  Calls to
 * remove_master_index_record() will remove the entry, calls to
 * set_master_index_record_chapter() can modify the entry, and calls to
 * put_master_index_record() can insert a collision record with this entry.
 *
 * @param masterIndex The master index to search
 * @param name        The chunk name
 * @param record      Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int getMasterIndexRecord(struct master_index *masterIndex,
                                       const struct uds_chunk_name *name,
                                       struct master_index_record *record)
{
  return masterIndex->getMasterIndexRecord(masterIndex, name, record);
}

/**
 * Return the master index stats.
 *
 * @param masterIndex The master index
 * @param dense       Stats for the dense portion of the index
 * @param sparse      Stats for the sparse portion of the index
 **/
static INLINE void getMasterIndexStats(const struct master_index *masterIndex,
                                       struct master_index_stats *dense,
                                       struct master_index_stats *sparse)
{
  masterIndex->getMasterIndexStats(masterIndex, dense, sparse);
}

/**
 * Find the master index zone associated with a chunk name
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static INLINE unsigned int
getMasterIndexZone(const struct master_index *masterIndex,
                   const struct uds_chunk_name *name)
{
  return masterIndex->getMasterIndexZone(masterIndex, name);
}

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param masterIndex  The master index
 * @param name         The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool isMasterIndexSample(const struct master_index *masterIndex,
                                       const struct uds_chunk_name *name)
{
  return masterIndex->isMasterIndexSample(masterIndex, name);
}

/**
 * Have all the data been read while restoring a master index from an input
 * stream?
 *
 * @param masterIndex  The master index to restore into
 *
 * @return true if all the data are read
 **/
static INLINE bool
isRestoringMasterIndexDone(const struct master_index *masterIndex)
{
  return masterIndex->isRestoringMasterIndexDone(masterIndex);
}

/**
 * Have all the data been written while saving a master index to an
 * output stream?  If the answer is yes, it is still necessary to call
 * finishSavingMasterIndex(), which will return quickly.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static INLINE bool
isSavingMasterIndexDone(const struct master_index *masterIndex,
                        unsigned int zoneNumber)
{
  return masterIndex->isSavingMasterIndexDone(masterIndex, zoneNumber);
}

/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int lookupMasterIndexName(const struct master_index *masterIndex,
                                        const struct uds_chunk_name *name,
                                        struct master_index_triage *triage)
{
  return masterIndex->lookupMasterIndexName(masterIndex, name, triage);
}

/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name.  The zone and
 *                    isSample fields are already filled in.  Set
 *                    inSampledChapter and virtualChapter if the chunk
 *                    name is found in the index.
 *
 * @return UDS_SUCCESS or an error code
 **/
static INLINE int
lookupMasterIndexSampledName(const struct master_index *masterIndex,
                             const struct uds_chunk_name *name,
                             struct master_index_triage *triage)
{
  return masterIndex->lookupMasterIndexSampledName(masterIndex, name, triage);
}

/**
 * Create a new record associated with a block name.
 *
 * @param record          The master index record found by getRecord()
 * @param virtual_chapter The chapter number where block info is found
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check put_master_index_record(struct master_index_record *record,
                                         uint64_t virtual_chapter);

/**
 * Remove an existing record.
 *
 * @param record  The master index record found by getRecord()
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
remove_master_index_record(struct master_index_record *record);

/**
 * Restore a saved delta list
 *
 * @param masterIndex  The master index to restore into
 * @param dlsi         The delta_list_save_info describing the delta list
 * @param data         The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static INLINE int
restoreDeltaListToMasterIndex(struct master_index *masterIndex,
                              const struct delta_list_save_info *dlsi,
                              const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
  return masterIndex->restoreDeltaListToMasterIndex(masterIndex, dlsi, data);
}

/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * In normal operation, the virtual chapter number will be the next chapter
 * following the currently open chapter.  We will advance the master index
 * one chapter forward in the virtual chapter space, invalidating the
 * oldest chapter in the index and be prepared to add index entries for the
 * newly opened chapter.
 *
 * In abnormal operation we make a potentially large change to the range of
 * chapters being indexed.  This happens when we are replaying chapters or
 * rebuilding an entire index.  If we move the open chapter forward, we
 * will invalidate many chapters (potentially the entire index).  If we
 * move the open chapter backward, we invalidate any entry in the newly
 * open chapter and any higher numbered chapter (potentially the entire
 * index).
 *
 * @param masterIndex     The master index
 * @param virtualChapter  The new open chapter number
 **/
static INLINE void setMasterIndexOpenChapter(struct master_index *masterIndex,
                                             uint64_t virtualChapter)
{
  masterIndex->setMasterIndexOpenChapter(masterIndex, virtualChapter);
}

/**
 * Set the chapter number associated with a block name.
 *
 * @param record          The master index record found by getRecord()
 * @param virtualChapter  The chapter number where block info is now found.
 *
 * @return UDS_SUCCESS or an error code
 **/
int __must_check
set_master_index_record_chapter(struct master_index_record *record,
			        uint64_t virtualChapter);

/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param masterIndex  The master index
 * @param tag          The tag value
 **/
static INLINE void setMasterIndexTag(struct master_index *masterIndex,
                                     byte tag)
{
  masterIndex->setMasterIndexTag(masterIndex, tag);
}

/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The zone number
 * @param virtualChapter  The new open chapter number
 **/
static INLINE void
setMasterIndexZoneOpenChapter(struct master_index *masterIndex,
                              unsigned int zoneNumber,
                              uint64_t virtualChapter)
{
  masterIndex->setMasterIndexZoneOpenChapter(masterIndex, zoneNumber,
                                             virtualChapter);
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param masterIndex      The master index to restore into
 * @param bufferedReaders  The buffered readers to read the master index from
 * @param numReaders       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
startRestoringMasterIndex(struct master_index     *masterIndex,
                          struct buffered_reader **bufferedReaders,
                          int                      numReaders)
{
  return masterIndex->startRestoringMasterIndex(masterIndex, bufferedReaders,
                                                numReaders);
}

/**
 * Start saving a master index to a buffered output stream.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The number of the zone to save
 * @param bufferedWriter  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static INLINE int
startSavingMasterIndex(const struct master_index *masterIndex,
                       unsigned int zoneNumber,
                       struct buffered_writer *bufferedWriter)
{
  return masterIndex->startSavingMasterIndex(masterIndex, zoneNumber,
                                             bufferedWriter);
}

#endif /* MASTERINDEXOPS_H */
