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
 */

#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include "errors.h"
#include "memoryAlloc.h"
#include "logger.h"
#include "uds-threads.h"

static struct hlist_head kernel_thread_list;
static struct mutex kernel_thread_mutex;
static once_state_t kernel_thread_once;

struct thread {
	void (*thread_func)(void *thread_data);
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
	struct registered_thread allocating_thread;
	struct thread *kt = arg;

	kt->thread_task = current;
	perform_once(&kernel_thread_once, kernel_thread_init);
	mutex_lock(&kernel_thread_mutex);
	hlist_add_head(&kt->thread_links, &kernel_thread_list);
	mutex_unlock(&kernel_thread_mutex);
	uds_register_allocating_thread(&allocating_thread, NULL);
	kt->thread_func(kt->thread_data);
	uds_unregister_allocating_thread();
	complete(&kt->thread_done);
	return 0;
}

/**********************************************************************/
int uds_create_thread(void (*thread_func)(void *),
		      void *thread_data,
		      const char *name,
		      struct thread **new_thread)
{
	char *name_colon = strchr(name, ':');
	char *my_name_colon = strchr(current->comm, ':');
	struct task_struct *thread;
	struct thread *kt;
	int result;

	result = UDS_ALLOCATE(1, struct thread, __func__, &kt);
	if (result != UDS_SUCCESS) {
		uds_log_warning("Error allocating memory for %s", name);
		return result;
	}
	kt->thread_func = thread_func;
	kt->thread_data = thread_data;
	init_completion(&kt->thread_done);
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
		UDS_FREE(kt);
		return PTR_ERR(thread);
	}
	*new_thread = kt;
	return UDS_SUCCESS;
}
/**********************************************************************/
int uds_join_threads(struct thread *kt)
{
	while (wait_for_completion_interruptible(&kt->thread_done) != 0) {
	}
	mutex_lock(&kernel_thread_mutex);
	hlist_del(&kt->thread_links);
	mutex_unlock(&kernel_thread_mutex);
	UDS_FREE(kt);
	return UDS_SUCCESS;
}


/**********************************************************************/
void uds_thread_exit(void)
{
	struct thread *kt;
	struct completion *completion = NULL;

	perform_once(&kernel_thread_once, kernel_thread_init);
	mutex_lock(&kernel_thread_mutex);
	hlist_for_each_entry(kt, &kernel_thread_list, thread_links) {
		if (kt->thread_task == current) {
			completion = &kt->thread_done;
			break;
		}
	}
	mutex_unlock(&kernel_thread_mutex);
	uds_unregister_allocating_thread();
	complete_and_exit(completion, 1);
}

/**********************************************************************/
pid_t uds_get_thread_id(void)
{
	return current->pid;
}

/**********************************************************************/
unsigned int uds_get_num_cores(void)
{
	return num_online_cpus();
}

/**********************************************************************/
int uds_initialize_barrier(struct barrier *barrier, unsigned int thread_count)
{
	int result = uds_initialize_semaphore(&barrier->mutex, 1);

	if (result != UDS_SUCCESS) {
		return result;
	}
	barrier->arrived = 0;
	barrier->thread_count = thread_count;
	return uds_initialize_semaphore(&barrier->wait, 0);
}

/**********************************************************************/
int uds_destroy_barrier(struct barrier *barrier)
{
	int result = uds_destroy_semaphore(&barrier->mutex);

	if (result != UDS_SUCCESS) {
		return result;
	}
	return uds_destroy_semaphore(&barrier->wait);
}

/**********************************************************************/
int uds_enter_barrier(struct barrier *barrier, bool *winner)
{
	bool last_thread;

	uds_acquire_semaphore(&barrier->mutex);
	last_thread = ++barrier->arrived == barrier->thread_count;
	if (last_thread) {
		// This is the last thread to arrive, so wake up the others
		int i;

		for (i = 1; i < barrier->thread_count; i++) {
			uds_release_semaphore(&barrier->wait);
		}
		// Then reinitialize for the next cycle
		barrier->arrived = 0;
		uds_release_semaphore(&barrier->mutex);
	} else {
		// This is NOT the last thread to arrive, so just wait
		uds_release_semaphore(&barrier->mutex);
		uds_acquire_semaphore(&barrier->wait);
	}
	if (winner != NULL) {
		*winner = last_thread;
	}
	return UDS_SUCCESS;
}

/**********************************************************************/
int uds_yield_scheduler(void)
{
	yield();
	return UDS_SUCCESS;
}
