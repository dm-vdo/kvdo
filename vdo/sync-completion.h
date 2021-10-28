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

#ifndef SYNC_COMPLETION_H
#define SYNC_COMPLETION_H

#include "completion.h"
#include "types.h"

/**
 * Launch an action on a VDO thread and wait for it to complete.
 *
 * @param vdo        The vdo
 * @param action     The callback to launch
 * @param thread_id  The thread on which to run the action
 * @param parent     The parent of the sync completion (may be NULL)
 **/
int perform_synchronous_vdo_action(struct vdo *vdo,
				   vdo_action * action,
				   thread_id_t thread_id,
				   void *parent);

#endif // SYNC_COMPLETION_H
