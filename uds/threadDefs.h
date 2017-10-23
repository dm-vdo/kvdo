/*
 * Copyright (c) 2017 Red Hat, Inc.
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
 * $Id: //eng/uds-releases/flanders/kernelLinux/uds/threadDefs.h#3 $
 */

#ifndef LINUX_KERNEL_THREAD_DEFS_H
#define LINUX_KERNEL_THREAD_DEFS_H

#include <linux/mutex.h>
#include <linux/semaphore.h>

#include "util/eventCount.h"

typedef struct kernelThread *Thread;
typedef pid_t                ThreadId;

typedef struct { EventCount *eventCount;    } CondVar;
typedef struct { struct mutex *pmut;        } Mutex;
typedef struct { struct hr_semaphore *psem; } Semaphore;

typedef struct {
  Semaphore mutex;       // Mutex for this barrier object
  Semaphore wait;        // Semaphore for threads waiting at the barrier
  int       arrived;     // Number of threads which have arrived
  int       threadCount; // Total number of threads using this barrier
} Barrier;

/**
 * Apply a function to every thread that we have created.
 *
 * @param applyFunc  The function to apply
 * @param argument   The first argument to applyFunc
 *
 **/
void applyToThreads(void applyFunc(void *, struct task_struct *),
                    void *argument);

/**
 * Exit the current thread.  This is a platform-specific function that is
 * intended to be an alternative to using BUG() or BUG_ON().
 **/
void exitThread(void);

#endif // LINUX_KERNEL_THREAD_DEFS_H
