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
 * $Id: //eng/uds-releases/gloria/kernelLinux/uds/loggerDefs.h#3 $
 */

#ifndef LINUX_KERNEL_LOGGER_DEFS_H
#define LINUX_KERNEL_LOGGER_DEFS_H

#include <linux/ratelimit.h>
#include <linux/version.h>

#define LOG_EMERG       0       /* system is unusable */
#define LOG_ALERT       1       /* action must be taken immediately */
#define LOG_CRIT        2       /* critical conditions */
#define LOG_ERR         3       /* error conditions */
#define LOG_WARNING     4       /* warning conditions */
#define LOG_NOTICE      5       /* normal but significant condition */
#define LOG_INFO        6       /* informational */
#define LOG_DEBUG       7       /* debug-level messages */

// Make it easy to log real pointer values using %px when in development.
#define PRIptr "pK"

/*
 * Apply a rate limiter to a log method call.
 *
 * @param logFunc  A method that does logging, which is not invoked if the
 *                 ratelimiter detects that we are calling it frequently.
 */
#define logRatelimit(logFunc, ...)                                 \
  do {                                                             \
    static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL, \
                                  DEFAULT_RATELIMIT_BURST);        \
    if (__ratelimit(&_rs)) {                                       \
      logFunc(__VA_ARGS__);                                        \
    }                                                              \
  } while (0)

#endif // LINUX_KERNEL_LOGGER_DEFS_H
