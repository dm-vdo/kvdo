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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/vdoDebug.c#4 $
 */

#include "vdoDebug.h"

#include "logger.h"
#include "stringUtils.h"
#include "vdoInternal.h"

static const char xLogDebugMessage[]     = "x-log-debug-message";

/**********************************************************************/
int initializeVDOCommandCompletion(struct vdo_command_completion  *command,
                                   struct vdo                     *vdo,
                                   int                             argc,
                                   char                          **argv)
{
  *command = (struct vdo_command_completion) {
    .vdo  = vdo,
    .argc = argc,
    .argv = argv,
  };
  initializeCompletion(&command->completion, VDO_COMMAND_COMPLETION,
                       vdo->layer);
  return initializeEnqueueableCompletion(&command->subCompletion,
                                         VDO_COMMAND_SUB_COMPLETION,
                                         vdo->layer);
}

/**********************************************************************/
int destroyVDOCommandCompletion(struct vdo_command_completion *command)
{
  if (command == NULL) {
    return VDO_SUCCESS;
  }

  destroyEnqueueable(&command->subCompletion);
  return command->completion.result;
}

/**********************************************************************/
static inline struct vdo_command_completion *
asVDOCommandCompletion(struct vdo_completion *completion)
{
  if (completion->type == VDO_COMMAND_COMPLETION) {
    return (struct vdo_command_completion *)
      ((uintptr_t) completion - offsetof(struct vdo_command_completion,
                                         completion));
  } else if (completion->type == VDO_COMMAND_SUB_COMPLETION) {
    return (struct vdo_command_completion *)
      ((uintptr_t) completion - offsetof(struct vdo_command_completion,
                                         subCompletion));
  } else {
    ASSERT_LOG_ONLY(((completion->type == VDO_COMMAND_COMPLETION) ||
                     (completion->type == VDO_COMMAND_SUB_COMPLETION)),
                    "completion type is %s instead of "
                    "VDO_COMMAND_COMPLETION or VDO_COMMAND_SUB_COMPLETION",
                    getCompletionTypeName(completion->type));
    return NULL;
  }
}

/**********************************************************************/
static void logDebugMessage(struct vdo_command_completion *cmd)
{
  static char buffer[256];

  char *buf = buffer;
  char *end = buffer + sizeof(buffer);

  for (int i = 1; i < cmd->argc; ++i) {
    buf = appendToBuffer(buf, end, " %s", cmd->argv[i]);
  }
  if (buf == end) {
    strcpy(buf - 4, "...");
  }
  logInfo("debug message:%s", buffer);
  finishCompletion(&cmd->completion, VDO_SUCCESS);
}

/**********************************************************************/
void executeVDOExtendedCommand(struct vdo_completion *completion)
{
  struct vdo_command_completion *cmd = asVDOCommandCompletion(completion);

  if ((cmd->vdo == NULL) || (cmd->argc == 0)) {
    finishCompletion(&cmd->completion, VDO_COMMAND_ERROR);
    return;
  }
  if (strcmp(cmd->argv[0], xLogDebugMessage) == 0) {
    logDebugMessage(cmd);
  } else {
    finishCompletion(&cmd->completion, VDO_UNKNOWN_COMMAND);
  }
}
