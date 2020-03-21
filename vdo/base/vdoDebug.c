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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDebug.c#8 $
 */

#include "vdoDebug.h"

#include "logger.h"
#include "stringUtils.h"
#include "vdoInternal.h"

static const char x_log_debug_message[] = "x-log-debug-message";

/**********************************************************************/
int initialize_vdo_command_completion(struct vdo_command_completion *command,
				      struct vdo *vdo,
				      int argc,
				      char **argv)
{
	*command = (struct vdo_command_completion) {
		.vdo = vdo,
		.argc = argc,
		.argv = argv,
	};
	initialize_completion(&command->completion, VDO_COMMAND_COMPLETION,
			      vdo->layer);
	return initialize_enqueueable_completion(&command->sub_completion,
					         VDO_COMMAND_SUB_COMPLETION,
					         vdo->layer);
}

/**********************************************************************/
int destroy_vdo_command_completion(struct vdo_command_completion *command)
{
	if (command == NULL) {
		return VDO_SUCCESS;
	}

	destroy_enqueueable(&command->sub_completion);
	return command->completion.result;
}

/**********************************************************************/
static inline struct vdo_command_completion *
as_vdo_command_completion(struct vdo_completion *completion)
{
	if (completion->type == VDO_COMMAND_COMPLETION) {
		return container_of(completion, struct vdo_command_completion,
				    completion);
	} else if (completion->type == VDO_COMMAND_SUB_COMPLETION) {
		return container_of(completion,
				    struct vdo_command_completion,
				    sub_completion);
	} else {
		ASSERT_LOG_ONLY(((completion->type == VDO_COMMAND_COMPLETION) ||
				 (completion->type == VDO_COMMAND_SUB_COMPLETION)),
				"completion type is %s instead of VDO_COMMAND_COMPLETION or VDO_COMMAND_SUB_COMPLETION",
				get_completion_type_name(completion->type));
		return NULL;
	}
}

/**********************************************************************/
static void log_debug_message(struct vdo_command_completion *cmd)
{
	static char buffer[256];

	char *buf = buffer;
	char *end = buffer + sizeof(buffer);

	int i;
	for (i = 1; i < cmd->argc; ++i) {
		buf = appendToBuffer(buf, end, " %s", cmd->argv[i]);
	}
	if (buf == end) {
		strcpy(buf - 4, "...");
	}
	logInfo("debug message:%s", buffer);
	finish_completion(&cmd->completion, VDO_SUCCESS);
}

/**********************************************************************/
void execute_vdo_extended_command(struct vdo_completion *completion)
{
	struct vdo_command_completion *cmd =
		as_vdo_command_completion(completion);

	if ((cmd->vdo == NULL) || (cmd->argc == 0)) {
		finish_completion(&cmd->completion, VDO_COMMAND_ERROR);
		return;
	}
	if (strcmp(cmd->argv[0], x_log_debug_message) == 0) {
		log_debug_message(cmd);
	} else {
		finish_completion(&cmd->completion, VDO_UNKNOWN_COMMAND);
	}
}
