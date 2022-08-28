// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "dump.h"

#include <linux/module.h>

#include "memory-alloc.h"
#include "type-defs.h"

#include "constants.h"
#include "data-vio.h"
#include "dedupe.h"
#include "io-submitter.h"
#include "kernel-types.h"
#include "logger.h"
#include "types.h"
#include "vdo.h"

enum dump_options {
	/* Work queues */
	SHOW_QUEUES,
	/* Memory pools */
	SHOW_VIO_POOL,
	/* Others */
	SHOW_VDO_STATUS,
	/*
	 * This one means an option overrides the "default" choices, instead
	 * of altering them.
	 */
	SKIP_DEFAULT
};

enum dump_option_flags {
	/* Work queues */
	FLAG_SHOW_QUEUES = (1 << SHOW_QUEUES),
	/* Memory pools */
	FLAG_SHOW_VIO_POOL = (1 << SHOW_VIO_POOL),
	/* Others */
	FLAG_SHOW_VDO_STATUS = (1 << SHOW_VDO_STATUS),
	/* Special */
	FLAG_SKIP_DEFAULT = (1 << SKIP_DEFAULT)
};

enum {
	FLAGS_ALL_POOLS = (FLAG_SHOW_VIO_POOL),
	DEFAULT_DUMP_FLAGS = (FLAG_SHOW_QUEUES | FLAG_SHOW_VDO_STATUS)
};

static inline bool is_arg_string(const char *arg, const char *this_option)
{
	/* convention seems to be case-independent options */
	return strncasecmp(arg, this_option, strlen(this_option)) == 0;
}

static void do_dump(struct vdo *vdo,
		    unsigned int dump_options_requested,
		    const char *why)
{
	uint32_t active, maximum;
	int64_t outstanding;

	uds_log_info("%s dump triggered via %s", UDS_LOGGING_MODULE_NAME, why);
	active = get_data_vio_pool_active_requests(vdo->data_vio_pool);
	maximum = get_data_vio_pool_maximum_requests(vdo->data_vio_pool);
	outstanding = (atomic64_read(&vdo->stats.bios_submitted) -
		       atomic64_read(&vdo->stats.bios_completed));
	uds_log_info("%u device requests outstanding (max %u), %lld bio requests outstanding, device '%s'",
		     active,
		     maximum,
		     outstanding,
		     vdo_get_device_name(vdo->device_config->owning_target));
	if (((dump_options_requested & FLAG_SHOW_QUEUES) != 0)
	    && (vdo->threads != NULL)) {
		thread_id_t id;

		for (id = 0; id < vdo->thread_config->thread_count; id++) {
			dump_work_queue(vdo->threads[id].queue);
		}
	}

	vdo_dump_hash_zones(vdo->hash_zones);
	dump_data_vio_pool(vdo->data_vio_pool,
			   (dump_options_requested & FLAG_SHOW_VIO_POOL) != 0);
	if ((dump_options_requested & FLAG_SHOW_VDO_STATUS) != 0) {
		vdo_dump_status(vdo);
	}

	report_uds_memory_usage();
	uds_log_info("end of %s dump", UDS_LOGGING_MODULE_NAME);
}

static int parse_dump_options(unsigned int argc,
			      char *const *argv,
			      unsigned int *dump_options_requested_ptr)
{
	unsigned int dump_options_requested = 0;

	static const struct {
		const char *name;
		unsigned int flags;
	} option_names[] = {
		{ "viopool", FLAG_SKIP_DEFAULT | FLAG_SHOW_VIO_POOL },
		{ "vdo", FLAG_SKIP_DEFAULT | FLAG_SHOW_VDO_STATUS },
		{ "pools", FLAG_SKIP_DEFAULT | FLAGS_ALL_POOLS },
		{ "queues", FLAG_SKIP_DEFAULT | FLAG_SHOW_QUEUES },
		{ "threads", FLAG_SKIP_DEFAULT | FLAG_SHOW_QUEUES },
		{ "default", FLAG_SKIP_DEFAULT | DEFAULT_DUMP_FLAGS },
		{ "all", ~0 },
	};

	bool options_okay = true;
	int i;

	for (i = 1; i < argc; i++) {
		int j;

		for (j = 0; j < ARRAY_SIZE(option_names); j++) {
			if (is_arg_string(argv[i], option_names[j].name)) {
				dump_options_requested |=
					option_names[j].flags;
				break;
			}
		}
		if (j == ARRAY_SIZE(option_names)) {
			uds_log_warning("dump option name '%s' unknown",
					argv[i]);
			options_okay = false;
		}
	}
	if (!options_okay) {
		return -EINVAL;
	}
	if ((dump_options_requested & FLAG_SKIP_DEFAULT) == 0) {
		dump_options_requested |= DEFAULT_DUMP_FLAGS;
	}
	*dump_options_requested_ptr = dump_options_requested;
	return 0;
}

/*
 * Dump as specified by zero or more string arguments.
 */
int vdo_dump(struct vdo *vdo,
	     unsigned int argc,
	     char *const *argv,
	     const char *why)
{
	unsigned int dump_options_requested = 0;
	int result = parse_dump_options(argc, argv, &dump_options_requested);

	if (result != 0) {
		return result;
	}

	do_dump(vdo, dump_options_requested, why);
	return 0;
}

/*
 * Dump everything we know how to dump
 */
void vdo_dump_all(struct vdo *vdo, const char *why)
{
	do_dump(vdo, ~0, why);
}

/*
 * Dump out the data_vio waiters on a wait queue.
 * wait_on should be the label to print for queue (e.g. logical or physical)
 */
static void dump_vio_waiters(struct wait_queue *queue, char *wait_on)
{
	struct waiter *waiter, *first = get_first_waiter(queue);
	struct data_vio *data_vio;

	if (first == NULL) {
		return;
	}

	data_vio = waiter_as_data_vio(first);

	uds_log_info("      %s is locked. Waited on by: vio %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
		     wait_on, data_vio, get_data_vio_allocation(data_vio),
		     data_vio->logical.lbn, data_vio->duplicate.pbn,
		     get_data_vio_operation_name(data_vio));


	for (waiter = first->next_waiter; waiter != first;
	     waiter = waiter->next_waiter) {
		data_vio = waiter_as_data_vio(waiter);
		uds_log_info("     ... and : vio %px pbn %llu lbn %llu d-pbn %llu lastOp %s",
			     data_vio, get_data_vio_allocation(data_vio),
			     data_vio->logical.lbn, data_vio->duplicate.pbn,
			     get_data_vio_operation_name(data_vio));
	}
}

/*
 * Encode various attributes of a data_vio as a string of one-character flags.
 * This encoding is for logging brevity:
 *
 * R => vio completion result not VDO_SUCCESS
 * W => vio is on a wait queue
 * D => vio is a duplicate
 *
 * The common case of no flags set will result in an empty, null-terminated
 * buffer. If any flags are encoded, the first character in the string will be
 * a space character.
 */
static void encode_vio_dump_flags(struct data_vio *data_vio, char buffer[8])
{
	char *p_flag = buffer;
	*p_flag++ = ' ';
	if (data_vio_as_completion(data_vio)->result != VDO_SUCCESS) {
		*p_flag++ = 'R';
	}
	if (data_vio->waiter.next_waiter != NULL) {
		*p_flag++ = 'W';
	}
	if (data_vio->is_duplicate) {
		*p_flag++ = 'D';
	}
	if (p_flag == &buffer[1]) {
		/* No flags, so remove the blank space. */
		p_flag = buffer;
	}
	*p_flag = '\0';
}

/*
 * Implements buffer_dump_function.
 */
void dump_data_vio(void *data)
{
	struct data_vio *data_vio = (struct data_vio *) data;

	/*
	 * This just needs to be big enough to hold a queue (thread) name
	 * and a function name (plus a separator character and NUL). The
	 * latter is limited only by taste.
	 *
	 * In making this static, we're assuming only one "dump" will run at
	 * a time. If more than one does run, the log output will be garbled
	 * anyway.
	 */
	static char vio_completion_dump_buffer[100 + MAX_VDO_WORK_QUEUE_NAME_LEN];
	/*
	 * Another static buffer...
	 * log10(256) = 2.408+, round up:
	 */
	enum { DIGITS_PER_UINT64_T = (int) (1 + 2.41 * sizeof(uint64_t)) };
	static char vio_block_number_dump_buffer[sizeof("P L D")
						 + 3 * DIGITS_PER_UINT64_T];
	static char vio_flush_generation_buffer[sizeof(" FG")
						+ DIGITS_PER_UINT64_T] = "";
	static char flags_dump_buffer[8];

	/*
	 * We're likely to be logging a couple thousand of these lines, and
	 * in some circumstances syslogd may have trouble keeping up, so
	 * keep it BRIEF rather than user-friendly.
	 */
	dump_completion_to_buffer(data_vio_as_completion(data_vio),
				  vio_completion_dump_buffer,
				  sizeof(vio_completion_dump_buffer));
	if (data_vio->is_duplicate) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu D%llu",
			 get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn,
			 data_vio->duplicate.pbn);
	} else if (data_vio_has_allocation(data_vio)) {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer),
			 "P%llu L%llu",
			 get_data_vio_allocation(data_vio),
			 data_vio->logical.lbn);
	} else {
		snprintf(vio_block_number_dump_buffer,
			 sizeof(vio_block_number_dump_buffer), "L%llu",
			 data_vio->logical.lbn);
	}

	if (data_vio->flush_generation != 0) {
		snprintf(vio_flush_generation_buffer,
			 sizeof(vio_flush_generation_buffer), " FG%llu",
			 data_vio->flush_generation);
	}

	encode_vio_dump_flags(data_vio, flags_dump_buffer);

	uds_log_info("  vio %px %s%s %s %s%s", data_vio,
		     vio_block_number_dump_buffer, vio_flush_generation_buffer,
		     get_data_vio_operation_name(data_vio),
		     vio_completion_dump_buffer,
		     flags_dump_buffer);
	/*
	 * might want info on: wantUDSAnswer / operation / status
	 * might want info on: bio / bios_merged
	 */

	dump_vio_waiters(&data_vio->logical.waiters, "lbn");

	/* might want to dump more info from vio here */
}
