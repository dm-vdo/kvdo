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
 * $Id: //eng/uds-releases/krusty/kernelLinux/uds/threadsLinuxKernel.c#9 $
 */

#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include "memoryAlloc.h"
#include "logger.h"
#include "threads.h"
#include "uds-error.h"

static struct hlist_head kernel_thread_list;
static struct mutex kernel_thread_mutex;
static once_state_t kernel_thread_once;

struct thread {
	void (*thread_func)(void *);
	void *thread_data;
	struct hlist_node thread_links;
	struct task_struct *thread_task;
	struct completion thread_done;
};

/**********************************************************************/
static void kernel_thread_init(void)
{
	mutex_init(&kernel_thread_mutex);
}

/**********************************************************************/
static int thread_starter(void *arg)
{
	struct thread *kt = arg;
	kt->thread_task = current;
	perform_once(&kernel_thread_once, kernel_thread_init);
	mutex_lock(&kernel_thread_mutex);
	hlist_add_head(&kt->thread_links, &kernel_thread_list);
	mutex_unlock(&kernel_thread_mutex);
	struct registered_thread allocating_thread;
	register_allocating_thread(&allocating_thread, NULL);
	kt->thread_func(kt->thread_data);
	unregister_allocating_thread();
	complete(&kt->thread_done);
	return 0;
}

/**********************************************************************/
int create_thread(void (*thread_func)(void *),
		  void *thread_data,
		  const char *name,
		  struct thread **new_thread)
{
	char *name_colon = strchr(name, ':');
	char *my_name_colon = strchr(current->comm, ':');
	struct thread *kt;
	int result = ALLOCATE(1, struct thread, __func__, &kt);
	if (result != UDS_SUCCESS) {
		log_warning("Error allocating memory for %s", name);
		return result;
	}
	kt->thread_func = thread_func;
	kt->thread_data = thread_data;
	init_completion(&kt->thread_done);
	struct task_struct *thread;
	/*
	 * Start the thread, with an appropriate thread name.
	 *
	 * If the name supplied contains a colon character, use that name. This
	 * causes uds module threads to have names like "uds:callbackW" and the
	 * main test runner thread to be named "zub:runtest".
	 *
	 * Otherwise if the current thread has a name containing a colon
	 * character, prefix the name supplied with the name of the current
	 * thread up to (and including) the colon character.  Thus when the
	 * "kvdo0:dedupeQ" thread opens an index session, all the threads
	 * associated with that index will have names like "kvdo0:foo".
	 *
	 * Otherwise just use the name supplied.  This should be a rare
	 * occurrence.
	 */
	if ((name_colon == NULL) && (my_name_colon != NULL)) {
		thread = kthread_run(thread_starter,
				     kt,
				     "%.*s:%s",
				     (int) (my_name_colon - current->comm),
				     current->comm,
				     name);
	} else {
		thread = kthread_run(thread_starter, kt, "%s", name);
	}
	if (IS_ERR(thread)) {
		FREE(kt);
		return UDS_ENOTHREADS;
	}
	*new_thread = kt;
	return UDS_SUCCESS;
}
/**********************************************************************/
int join_threads(struct thread *kt)
{
	while (wait_for_completion_interruptible(&kt->thread_done) != 0) {
	}
	mutex_lock(&kernel_thread_mutex);
	hlist_del(&kt->thread_links);
	mutex_unlock(&kernel_thread_mutex);
	FREE(kt);
	return UDS_SUCCESS;
}

/**********************************************************************/
void apply_to_threads(void apply_func(void *, struct task_struct *),
		      void *argument)
{
	struct thread *kt;
	perform_once(&kernel_thread_once, kernel_thread_init);
	mutex_lock(&kernel_thread_mutex);
	hlist_for_each_entry (kt, &kernel_thread_list, thread_links) {
		apply_func(argument, kt->thread_task);
	}
	mutex_unlock(&kernel_thread_mutex);
}

/**********************************************************************/
void thread_exit(void)
{
	struct thread *kt;
	struct completion *completion = NULL;
	perform_once(&kernel_thread_once, kernel_thread_init);
	mutex_lock(&kernel_thread_mutex);
	hlist_for_each_entry (kt, &kernel_thread_list, thread_links) {
		if (kt->thread_task == current) {
			completion = &kt->thread_done;
			break;
		}
	}
	mutex_unlock(&kernel_thread_mutex);
	unregister_allocating_thread();
	complete_and_exit(completion, 1);
}

/**********************************************************************/
pid_t get_thread_id(void)
{
	return current->pid;
}

/**********************************************************************/
unsigned int get_num_cores(void)
{
	return num_online_cpus();
}

/**********************************************************************/
int initialize_barrier(struct barrier *barrier, unsigned int thread_count)
{
	barrier->arrived = 0;
	barrier->thread_count = thread_count;
	int result = initialize_semaphore(&barrier->mutex, 1);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return initialize_semaphore(&barrier->wait, 0);
}

/**********************************************************************/
int destroy_barrier(struct barrier *barrier)
{
	int result = destroy_semaphore(&barrier->mutex);
	if (result != UDS_SUCCESS) {
		return result;
	}
	return destroy_semaphore(&barrier->wait);
}

/**********************************************************************/
int enter_barrier(struct barrier *barrier, bool *winner)
{
	acquire_semaphore(&barrier->mutex);
	bool last_thread = ++barrier->arrived == barrier->thread_count;
	if (last_thread) {
		// This is the last thread to arrive, so wake up the others
		int i;
		for (i = 1; i < barrier->thread_count; i++) {
			release_semaphore(&barrier->wait);
		}
		// Then reinitialize for the next cycle
		barrier->arrived = 0;
		release_semaphore(&barrier->mutex);
	} else {
		// This is NOT the last thread to arrive, so just wait
		release_semaphore(&barrier->mutex);
		acquire_semaphore(&barrier->wait);
	}
	if (winner != NULL) {
		*winner = last_thread;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int yield_scheduler(void)
{
	yield();
	return UDS_SUCCESS;
}
