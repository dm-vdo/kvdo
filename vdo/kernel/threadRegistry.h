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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/threadRegistry.h#2 $
 */

#ifndef THREAD_REGISTRY_H
#define THREAD_REGISTRY_H 1

#include <linux/list.h>
#include <linux/spinlock.h>

/*
 * We don't expect this set to ever get really large, so a linked list
 * is adequate.
 */

typedef struct threadRegistry {
	struct list_head links;
	rwlock_t lock;
} ThreadRegistry;

typedef struct registeredThread {
	struct list_head links;
	const void *pointer;
	struct task_struct *task;
} RegisteredThread;

/*****************************************************************************/

/**
 * Initialize a registry of threads and associated data pointers.
 *
 * @param  registry  The registry to initialize
 **/
void initializeThreadRegistry(ThreadRegistry *registry);

/**
 * Register the current thread and associate it with a data pointer.
 *
 * This call will log messages if the thread is already registered.
 *
 * @param registry    The thread registry
 * @param new_thread  RegisteredThread structure to use for the current thread
 * @param pointer     The value to associated with the current thread
 **/
void registerThread(ThreadRegistry *registry,
		    RegisteredThread *new_thread,
		    const void *pointer);

/**
 * Remove the registration for the current thread.
 *
 * A message may be logged if the thread was not registered.
 *
 * @param  registry  The thread registry
 **/
void unregisterThread(ThreadRegistry *registry);

/**
 * Fetch a pointer that may have been registered for the current
 * thread. If the thread is not registered, a null pointer is
 * returned.
 *
 * @param  registry  The thread registry
 *
 * @return  the registered pointer, if any, or NULL
 **/
const void *lookupThread(ThreadRegistry *registry);

#endif /* THREAD_REGISTRY_H */
