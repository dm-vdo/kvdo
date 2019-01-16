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
 * $Id: //eng/linux-vdo/src/c++/vdo/base/adminState.h#1 $
 */

#ifndef ADMIN_STATE_H
#define ADMIN_STATE_H

#include "common.h"

typedef enum {
  ADMIN_STATE_NORMAL_OPERATION = 0,
  ADMIN_STATE_CLOSE_REQUESTED,
  ADMIN_STATE_CLOSING,
  ADMIN_STATE_CLOSED,
  ADMIN_STATE_SUSPENDED,
} AdminState;

/**********************************************************************/
static inline bool isClosing(AdminState state)
{
  return ((state >= ADMIN_STATE_CLOSE_REQUESTED)
          && (state <= ADMIN_STATE_CLOSED));
}

#endif // ADMIN_STATE_H
