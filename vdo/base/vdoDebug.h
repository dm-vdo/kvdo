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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDebug.h#7 $
 */

#ifndef VDO_DEBUG_H
#define VDO_DEBUG_H

#include "completion.h"
#include "vdo.h"

/**
 * A completion used to pass information to a potentially asynchronous
 * (because it must run in a different zone) extended command.
 *
 * These commands are dispatched according to argv[0], which is of the form
 * "x-some-command-name", and intentionally open ended for debugging.
 *
 * The command "x-log-debug-message" is currently defined to echo the
 * remainder of the arguments into the kernel log via the vdo logger at
 * info level.
 **/
struct vdo_command_completion {
	struct vdo_completion completion;
	struct vdo_completion sub_completion;
	struct vdo *vdo;
	int argc;
	char **argv;
};

/**
 * Initialize a vdo command completion.
 *
 * @param command       The command completion to initialize.
 * @param vdo           The vdo.
 * @param argc          An argument count.
 * @param argv          An argument vector of length argc.
 **/
void initialize_vdo_command_completion(struct vdo_command_completion *command,
				       struct vdo *vdo,
				       int argc,
				       char **argv);

/**
 * Perform an asynchronous extended command (usually debugging related).
 *
 * @param completion    The completion embedded in vdo_command_completion.
 **/
void execute_vdo_extended_command(struct vdo_completion *completion);

#endif // VDO_DEBUG_H
