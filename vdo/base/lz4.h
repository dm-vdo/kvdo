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
 * $Id: //eng/vdo-releases/aluminum/src/c++/vdo/base/lz4.h#1 $
 */

#ifndef LZ4_H
#define LZ4_H

/*
   LZ4 - Fast LZ compression algorithm
   Header File
   Copyright (C) 2011-2013, Yann Collet.
   BSD 2-Clause License (http://www.opensource.org/licenses/bsd-license.php)

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

       * Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
       * Redistributions in binary form must reproduce the above
   copyright notice, this list of conditions and the following disclaimer
   in the documentation and/or other materials provided with the
   distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   You can contact the author at :
   - LZ4 homepage : http://fastcompression.blogspot.com/p/lz4.html
   - LZ4 source repository : http://code.google.com/p/lz4/
*/

/*
 * With authors permission dual licensed as BSD/GPL for linux kernel
 *
 * Origin: http://lz4.googlecode.com/svn/trunk
 * Revision: 88
 */

/**
 * Compress 'isize' bytes from 'source' into an output buffer 'dest' of
 * maximum size 'maxOutputSize'.  If it cannot achieve it, compression will
 * stop, and result of the function will be zero.  This function never
 * writes outside of provided output buffer.
 *
 * @param ctx            Scratch space that will not fit on the stack
 * @param source         Input data
 * @param dest           Output data
 * @param isize          Input size. Max supported value is ~1.9GB
 * @param maxOutputSize  Size of the destination buffer
 *
 * @return the number of bytes written in buffer 'dest' or 0 if the
 *         compression fails
 **/
int LZ4_compress_ctx_limitedOutput(void       *ctx,
                                   const char *source,
                                   char       *dest,
                                   int         isize,
                                   int         maxOutputSize);

/**
 * Return the size of the "ctx" block needed by the compression method.
 *
 * @return the number of bytes needed in the "ctx" argument to
 *         Z4_compress_ctx_limitedOutput
 **/
int LZ4_context_size(void);

/**
 * Uncompress 'isize' bytes from 'source' into an output buffer 'dest' of
 * maximum size 'maxOutputSize'.  This function never writes beyond dest +
 * maxOutputSize, and is therefore protected against malicious data packets
 *
 * @param source         Input data
 * @param dest           Output data
 * @param isize          Input size, therefore the compressed size
 * @param maxOutputSize  Size of the destination buffer
 *
 * @return the number of bytes decoded in the destination buffer
 *         (necessarily <= maxOutputSize).  If the source stream is
 *         malformed, the function will stop decoding and return a negative
 *         result, indicating the byte position of the faulty instruction
 **/
int LZ4_uncompress_unknownOutputSize(const char *source,
                                     char       *dest,
                                     int         isize,
                                     int         maxOutputSize);

#endif // LZ4_H
