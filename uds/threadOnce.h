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
 * $Id: //eng/uds-releases/krusty/src/uds/threadOnce.h#7 $
 */

#ifndef THREAD_ONCE_H
#define THREAD_ONCE_H

#include <linux/atomic.h>

#define ONCE_STATE_INITIALIZER ATOMIC_INIT(0)

typedef atomic_t once_state_t;

/**
 * Thread safe once only initialization.
 *
 * @param once_state     pointer to object to record that initialization
 *                       has been performed
 * @param init_function  called if once_state does not indicate
 *                       initialization has been performed
 *
 * @note Generally the following declaration of once_state is performed in
 *       at file scope:
 *
 *       static once_state_t once_state = ONCE_STATE_INITIALIZER;
 **/
void perform_once(once_state_t *once_state, void (*init_function) (void));

#endif /* THREAD_ONCE_H */
