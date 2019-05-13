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
 * $Id: //eng/linux-vdo/src/c++/vdo/kernel/threadRegistry.c#2 $
 */

#include "threadRegistry.h"

#include <linux/gfp.h>
#include <linux/slab.h>

#include "permassert.h"

/*
 * We need to be careful when using other facilities that may use
 * threadRegistry functions in their normal operation.  For example,
 * we do not want to invoke the logger while holding a lock.
 */

/*****************************************************************************/
void registerThread(ThreadRegistry *registry,
		    RegisteredThread *new_thread,
		    const void *pointer)
{
	INIT_LIST_HEAD(&new_thread->links);
	new_thread->pointer = pointer;
	new_thread->task = current;

	bool found_it = false;
	RegisteredThread *thread;
	write_lock(&registry->lock);
	list_for_each_entry (thread, &registry->links, links) {
		if (thread->task == current) {
			// This should not have been there.
			// We'll complain after releasing the lock.
			list_del_init(&thread->links);
			found_it = true;
			break;
		}
	}
	list_add_tail(&new_thread->links, &registry->links);
	write_unlock(&registry->lock);
	ASSERT_LOG_ONLY(!found_it, "new thread not already in registry");
}

/*****************************************************************************/
void unregisterThread(ThreadRegistry *registry)
{
	bool found_it = false;
	RegisteredThread *thread;
	write_lock(&registry->lock);
	list_for_each_entry (thread, &registry->links, links) {
		if (thread->task == current) {
			list_del_init(&thread->links);
			found_it = true;
			break;
		}
	}
	write_unlock(&registry->lock);
	ASSERT_LOG_ONLY(found_it, "thread found in registry");
}

/*****************************************************************************/
void initializeThreadRegistry(ThreadRegistry *registry)
{
	INIT_LIST_HEAD(&registry->links);
	rwlock_init(&registry->lock);
}

/*****************************************************************************/
const void *lookupThread(ThreadRegistry *registry)
{
	const void *result = NULL;
	read_lock(&registry->lock);
	RegisteredThread *thread;
	list_for_each_entry (thread, &registry->links, links) {
		if (thread->task == current) {
			result = thread->pointer;
			break;
		}
	}
	read_unlock(&registry->lock);
	return result;
}
