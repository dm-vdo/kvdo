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
 * $Id: //eng/uds-releases/gloria/src/uds/threadOnce.c#1 $
 */

#include "errors.h"
#include "threads.h"

enum {
  ONCE_NOT_DONE    = 0,
  ONCE_IN_PROGRESS = 1,
  ONCE_COMPLETE    = 2,
};

/*****************************************************************************/
int performOnce(OnceState *once, void (*function)(void))
{
  for (;;) {
    switch (atomic_cmpxchg(once, ONCE_NOT_DONE, ONCE_IN_PROGRESS)) {
    case ONCE_NOT_DONE:
      function();
      atomic_set_release(once, ONCE_COMPLETE);
      return UDS_SUCCESS;
    case ONCE_IN_PROGRESS:
      yieldScheduler();
      break;
    case ONCE_COMPLETE:
      return UDS_SUCCESS;
    default:
      return UDS_BAD_STATE;
    }
  }
}
