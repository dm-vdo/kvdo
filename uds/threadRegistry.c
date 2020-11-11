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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadRegistry.c#3 $
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
void register_thread(struct thread_registry *registry,
		     struct registered_thread *new_thread,
		     const void *pointer)
{
	INIT_LIST_HEAD(&new_thread->links);
	new_thread->pointer = pointer;
	new_thread->task = current;

	bool found_it = false;
	struct registered_thread *thread;
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
void unregister_thread(struct thread_registry *registry)
{
	bool found_it = false;
	struct registered_thread *thread;
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
void initialize_thread_registry(struct thread_registry *registry)
{
	INIT_LIST_HEAD(&registry->links);
	rwlock_init(&registry->lock);
}

/*****************************************************************************/
const void *lookup_thread(struct thread_registry *registry)
{
	const void *result = NULL;
	read_lock(&registry->lock);
	struct registered_thread *thread;
	list_for_each_entry (thread, &registry->links, links) {
		if (thread->task == current) {
			result = thread->pointer;
			break;
		}
	}
	read_unlock(&registry->lock);
	return result;
}
