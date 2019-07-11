/*
 * Copyright (c) 2019 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/resourceUsageLinuxKernel.c#1 $
 */

#include <linux/version.h>

#include <linux/sched.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,11,0)
// Needed for task->signal definition after 4.11.0
#include <linux/sched/signal.h>
#endif
#include <linux/task_io_accounting_ops.h>

#include "logger.h"
#include "memoryAlloc.h"
#include "resourceDefs.h"
#include "threads.h"

/**
 * Thread statistics as gathered from task_struct
 **/
struct threadStatistics {
  char comm[TASK_COMM_LEN];  /* thread name (may be unterminated) */
  unsigned long cputime;     /* Nanoseconds using CPU */
  unsigned long inblock;     /* Sectors read */
  unsigned long outblock;    /* Sectors written */
  pid_t id;                  /* Thread id */
  ThreadStatistics *next;
};

/***********************************************************************/
static void addThreadStatistics(ThreadStatistics **tsList,
                                const ThreadStatistics *tsNew)
{
  // Allocate a new ThreadStatistics and copy the data into it
  ThreadStatistics *ts;
  if (ALLOCATE(1, ThreadStatistics, __func__, &ts) == UDS_SUCCESS) {
    *ts = *tsNew;
    // Insert the new one into the list, sorted by id
    while ((*tsList != NULL) && (ts->id > (*tsList)->id)) {
      tsList = &(*tsList)->next;
    }
    ts->next = *tsList;
    *tsList = ts;
  }
}

/***********************************************************************/
static void addOneThread(void *arg, struct task_struct *task)
{
  ThreadStatistics ts = {
    .cputime  = task->se.sum_exec_runtime,
    .id       = task->pid,
    .inblock  = task_io_get_inblock(task) + task->signal->inblock,
    .outblock = task_io_get_oublock(task) + task->signal->oublock,
  };
  memcpy(ts.comm, task->comm, TASK_COMM_LEN);
  addThreadStatistics(arg, &ts);
}

/***********************************************************************/
void freeThreadStatistics(ThreadStatistics *ts)
{
  while (ts != NULL) {
    ThreadStatistics *tsNext = ts->next;
    FREE(ts);
    ts = tsNext;
  }
}

/***********************************************************************/
ThreadStatistics *getThreadStatistics(void)
{
  ThreadStatistics *tsList = NULL;
  applyToThreads(addOneThread, &tsList);
  return tsList;
}

/***********************************************************************/
void printThreadStatistics(ThreadStatistics *prev, ThreadStatistics *cur)
{
  const unsigned long MILLION = 1000 * 1000;
  const unsigned long BILLION = 1000 * 1000 * 1000;
  logInfo("Thread           CPUTime    Inblock Outblock Note");
  logInfo("================ ========== ======= ======== ====");
  while ((prev != NULL) && (cur != NULL)) {
    if ((cur == NULL) || (prev->id < cur->id)) {
      logInfo("  %-45.*s gone", TASK_COMM_LEN, prev->comm);
      prev = prev->next;
    } else if ((prev == NULL) || (prev->id > cur->id)) {
      logInfo("%-16.*s %3lu.%06lu %7lu %8lu new",
              TASK_COMM_LEN, cur->comm,
              cur->cputime / BILLION, cur->cputime / 1000 % MILLION,
              cur->inblock, cur->outblock);
      cur = cur->next;
    } else {
      logInfo("%-16.*s %3lu.%06lu %7lu %8lu",
              TASK_COMM_LEN, cur->comm,
              (cur->cputime - prev->cputime) / BILLION,
              (cur->cputime - prev->cputime) / 1000 % MILLION,
              cur->inblock - prev->inblock, cur->outblock - prev->outblock);
      prev = prev->next;
      cur = cur->next;
    }
  }
}
