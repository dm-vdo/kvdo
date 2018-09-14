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
 * $Id: //eng/uds-releases/gloria/src/uds/singleFileLayoutInternals.h#2 $
 */

#ifndef SINGLE_FILE_LAYOUT_INTERNALS_H
#define SINGLE_FILE_LAYOUT_INTERNALS_H

#include "singleFileLayout.h"

#include "buffer.h"
#include "compiler.h"
#include "indexConfig.h"
#include "layoutRegion.h"
#include "permassert.h"
#include "timeUtils.h"

/*
 * Overall layout of an index on disk:
 *
 * The layout is divided into a number of fixed-size regions, the sizes of
 * which are computed when the index is created. Every header and region
 * begins on 4K block boundary. Save regions are further sub-divided into
 * regions of their own.
 *
 * Each region has a kind and an instance number. Some kinds only have one
 * instance and therefore use RL_SOLE_INSTANCE (-1) as the instance number.
 * The RL_KIND_INDEX uses instances to represent sub-indices, where used.
 * A save region can either hold a checkpoint or a clean shutdown (determined
 * by the type). The instances determine which available save slot is used.
 * The RL_KIND_MASTER_INDEX uses instances to record which zone is being saved.
 *
 *     +-+-+--------+--------+--------+-----+---  -+-+
 *     | | |   I N D E X   0      101, 0    | ...  | |
 *     |H|C+--------+--------+--------+-----+---  -+S|
 *     |D|f| Volume | Save   | Save   |     |      |e|
 *     |R|g| Region | Region | Region | ... | ...  |a|
 *     | | | 201 -1 | 202  0 | 202  1 |     |      |l|
 *     +-+-+--------+--------+--------+-----+---  -+-+
 *
 * The header contains the encoded regional layout table as well as
 * the saved index configuration record. The sub-index regions and their
 * subdivisions are maintained in the same table.
 *
 * There are at least two save regions per sub-index to preserve the old
 * state should the saving of a state be incomplete. They are used in
 * a round-robin fashion.
 *
 * Anatomy of a save region:
 *
 *     +-+-----+------+------+-----+   -+-----+
 *     |H| IPM | MI   | MI   |     |    | OC  |
 *     |D|     | zone | zone | ... |    |     |
 *     |R| 301 | 302  | 302  |     |    | 303 |
 *     | | -1  | 0    | 1    |     |    | -1  |
 *     +-+-----+------+------+-----+   -+-----+
 *
 * Every region header has a type (and version). In save regions,
 * the open chapter only appears in RL_TYPE_SAVE not RL_TYPE_CHECKPOINT,
 * although the same space is reserved for both.
 *
 * The header contains the encoded regional layout table as well as the
 * index state record for that save or checkpoint. Each save or checkpoint
 * has a unique generation number and nonce which is used to seed the
 * checksums of those regions.
 */

typedef struct indexSaveData_v1 {
  uint64_t      timestamp;              // ms since epoch...
  uint64_t      nonce;
  uint32_t      version;                // 1
  uint32_t      unused__;
} IndexSaveData;

typedef struct indexSaveLayout {
  LayoutRegion     indexSave;
  LayoutRegion     header;
  unsigned int     numZones;
  LayoutRegion     indexPageMap;
  LayoutRegion     freeSpace;
  LayoutRegion    *masterIndexZones;
  LayoutRegion    *openChapter;
  IndexSaveType    saveType;
  IndexSaveData    saveData;
  Buffer          *indexStateBuffer;
  bool             read;
  bool             written;
} IndexSaveLayout;

typedef struct subIndexLayout {
  LayoutRegion     subIndex;
  uint64_t         nonce;
  LayoutRegion     volume;
  IndexSaveLayout *saves;
} SubIndexLayout;

typedef struct superBlockData_v1 {
  byte       magicLabel[32];
  byte       nonceInfo[32];
  uint64_t   nonce;
  uint32_t   version;                   // 1
  uint32_t   blockSize;                 // for verification
  uint16_t   numIndexes;                // 1
  uint16_t   maxSaves;
  uint64_t   openChapterBlocks;
  uint64_t   pageMapBlocks;
} SuperBlockData;

struct singleFileLayout {
  IndexLayout     common;
  IORegion       *region;
  SuperBlockData  super;
  LayoutRegion    header;
  LayoutRegion    config;
  SubIndexLayout  index;
  LayoutRegion    seal;
  bool            loaded;
  bool            saved;
  bool            close;
  uint64_t        totalBlocks;
};

/**
 * Structure used to compute single file layout sizes.
 *
 * Note that the masterIndexBlocks represent all zones and are sized for
 * the maximum number of blocks that would be needed regardless of the number
 * of zones (up to the maximum value) that are used at run time.
 *
 * Similarly, the number of saves is sized for the minimum safe value
 * assuming checkpointing is enabled, since that is also a run-time parameter.
 **/
typedef struct saveLayoutSizes {
  Configuration config;                 // this is a captive copy
  Geometry      geometry;               // this is a captive copy
  unsigned int  numSaves;               // per sub-index
  size_t        blockSize;              // in bytes
  uint64_t      volumeBlocks;           // per sub-index
  uint64_t      masterIndexBlocks;      // per save
  uint64_t      pageMapBlocks;          // per save
  uint64_t      openChapterBlocks;      // per save
  uint64_t      saveBlocks;             // per sub-index
  uint64_t      subIndexBlocks;         // per sub-index
  uint64_t      totalBlocks;            // for whole layout
} SaveLayoutSizes;

/*****************************************************************************/

/**
 * Initialize a single file layout from the region table and super block data
 * stored in stable storage.
 *
 * @param sfl           the layout to initialize
 * @param region        the IO region for this layout
 * @param super         the super block data read from the superblock
 * @param table         the region table read from the superblock
 * @param firstBlock    the first block number in the region
 *
 * @return UDS_SUCCESS or an error code
 **/
int reconstituteSingleFileLayout(SingleFileLayout     *sfl,
                                 IORegion             *region,
                                 SuperBlockData       *super,
                                 RegionTable          *table,
                                 uint64_t              firstBlock)
  __attribute__((warn_unused_result));

/*****************************************************************************/
void destroySingleFileLayout(SingleFileLayout *sfl);

/*****************************************************************************/
static INLINE SingleFileLayout *asSingleFileLayout(IndexLayout *layout)
{
  return container_of(layout, SingleFileLayout, common);
}

/*****************************************************************************/
int getSingleFileLayoutRegion(SingleFileLayout  *sfl,
                              LayoutRegion      *lr,
                              IOAccessMode       access,
                              IORegion         **regionPtr)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int getSingleFileLayoutReader(SingleFileLayout  *sfl,
                              LayoutRegion      *lr,
                              BufferedReader   **readerPtr)
  __attribute__((warn_unused_result));

/**
 * Find the latest index save slot for a sub-index.
 *
 * @param [in]  sfl             The single file layout.
 * @param [out] numZonesPtr     Where to store the actual number of zones
 *                                that were saved.
 * @param [out] slotPtr         Where to store the slot number we found.
 *
 * @return UDS_SUCCESS or an error code.
 **/
int findLatestIndexSaveSlot(SingleFileLayout *sfl,
                            unsigned int     *numZonesPtr,
                            unsigned int     *slotPtr)
  __attribute__((warn_unused_result));

/**
 * Determine which index save slot to use for a new index save.
 *
 * Also allocates the masterIndex regions and, if needed, the openChapter
 * region.
 *
 * @param [in]  sfl             The single file layout.
 * @param [in]  numZones        Actual number of zones currently in use.
 * @param [in]  saveType        The index save type.
 * @param [out] saveSlotPtr     Where to store the save slot number.
 *
 * @return UDS_SUCCESS or an error code
 **/
int setupSingleFileIndexSaveSlot(SingleFileLayout *sfl,
                                 unsigned int      numZones,
                                 IndexSaveType     saveType,
                                 unsigned int     *saveSlotPtr)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int commitSingleFileIndexSave(SingleFileLayout *sfl, unsigned int saveSlot)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int cancelSingleFileIndexSave(SingleFileLayout *sfl, unsigned int saveSlot)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int discardSingleFileIndexSaves(SingleFileLayout *sfl, bool all)
  __attribute__((warn_unused_result));

/*****************************************************************************/
int validateIndexSaveLayout(IndexSaveLayout *isl,
                            uint64_t         volumeNonce,
                            uint64_t        *saveTimePtr)
  __attribute__((warn_unused_result));

#endif // SINGLE_FILE_LAYOUT_INTERNALS_H
