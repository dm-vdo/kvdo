/**
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
 * $Id: //eng/uds-releases/gloria/src/uds/accessMode.h#1 $
 **/

#ifndef ACCESS_MODE_H
#define ACCESS_MODE_H

typedef enum {
  IO_READ              = 0x1,
  IO_WRITE             = 0x2,
  IO_CREATE            = 0x4,
  IO_READ_WRITE        = IO_READ | IO_WRITE,
  IO_CREATE_READ_WRITE = IO_READ_WRITE | IO_CREATE,
  IO_CREATE_WRITE      = IO_WRITE | IO_CREATE,
  IO_ACCESS_MASK       = 0x7,
} IOAccessMode;

#endif // ACCESS_MODE_H
