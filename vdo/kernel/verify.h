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
 * $Id: //eng/vdo-releases/magnesium/src/c++/vdo/kernel/verify.h#1 $
 */
#include "kernelLayer.h"

/**
 * Verify the Albireo-provided deduplication advice, and invoke a callback once
 * the answer is available. This is done through a call to kvdoReadBlock()
 * which will eventually call back to verifyDuplication() once the block is
 * read and possibly uncompressed.
 *
 * @param dataVIO   The DataVIO with advice filled in.
 **/
void kvdoVerifyDuplication(DataVIO *dataVIO);
