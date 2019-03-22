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
 * $Id: //eng/uds-releases/jasper/kernelLinux/uds/errorsLinuxKernel.c#1 $
 */


#include <linux/errno.h>

#include "compiler.h"
#include "errorDefs.h"

static char * messageTable[] = {
  [EPERM]   = "Operation not permitted",
  [ENOENT]  = "No such file or directory",
  [ESRCH]   = "No such process",
  [EINTR]   = "Interrupted system call",
  [EIO]     = "Input/output error",
  [ENXIO]   = "No such device or address",
  [E2BIG]   = "Argument list too long",
  [ENOEXEC] = "Exec format error",
  [EBADF]   = "Bad file descriptor",
  [ECHILD]  = "No child processes",
  [EAGAIN]  = "Resource temporarily unavailable",
  [ENOMEM]  = "Cannot allocate memory",
  [EACCES]  = "Permission denied",
  [EFAULT]  = "Bad address",
  [ENOTBLK] = "Block device required",
  [EBUSY]   = "Device or resource busy",
  [EEXIST]  = "File exists",
  [EXDEV]   = "Invalid cross-device link",
  [ENODEV]  = "No such device",
  [ENOTDIR] = "Not a directory",
  [EISDIR]  = "Is a directory",
  [EINVAL]  = "Invalid argument",
  [ENFILE]  = "Too many open files in system",
  [EMFILE]  = "Too many open files",
  [ENOTTY]  = "Inappropriate ioctl for device",
  [ETXTBSY] = "Text file busy",
  [EFBIG]   = "File too large",
  [ENOSPC]  = "No space left on device",
  [ESPIPE]  = "Illegal seek",
  [EROFS]   = "Read-only file system",
  [EMLINK]  = "Too many links",
  [EPIPE]   = "Broken pipe",
  [EDOM]    = "Numerical argument out of domain",
  [ERANGE]  = "Numerical result out of range"
};

/**********************************************************************/
const char *systemStringError(int errnum, char *buf, size_t buflen)
{
  char *errorString = NULL;
  if ((errnum > 0) && (errnum < COUNT_OF(messageTable))) {
    errorString = messageTable[errnum];
  }

  size_t len = ((errorString == NULL)
                ? snprintf(buf, buflen, "Unknown error %d", errnum)
                : snprintf(buf, buflen, "%s", errorString));
  if (len < buflen) {
    return buf;
  }

  buf[0] = '\0';
  return "System error";
}
