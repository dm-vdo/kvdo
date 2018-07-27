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
 * $Id: //eng/uds-releases/gloria/src/uds/buffer.c#4 $
 */

#include "buffer.h"

#include "bufferPrivate.h"
#include "logger.h"
#include "memoryAlloc.h"
#include "numeric.h"
#include "permassert.h"
#include "typeDefs.h"

/**********************************************************************/
int wrapBuffer(byte    *bytes,
               size_t   length,
               size_t   contentLength,
               Buffer **bufferPtr)
{
  int result = ASSERT((contentLength <= length),
                      "content length, %zu, fits in buffer size, %zu",
                      length, contentLength);
  Buffer *buffer;
  result = ALLOCATE(1, Buffer, "buffer", &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }

  buffer->data    = bytes;
  buffer->start   = 0;
  buffer->end     = contentLength;
  buffer->length  = length;
  buffer->wrapped = true;

  *bufferPtr      = buffer;
  return UDS_SUCCESS;
}

/***********************************************************************/
int makeBuffer(size_t size, Buffer **newBuffer)
{
  byte *data;
  int result = ALLOCATE(size, byte, "buffer data", &data);
  if (result != UDS_SUCCESS) {
    return result;
  }

  Buffer *buffer;
  result = wrapBuffer(data, size, 0, &buffer);
  if (result != UDS_SUCCESS) {
    FREE(data);
    return result;
  }

  buffer->wrapped = false;
  *newBuffer = buffer;
  return UDS_SUCCESS;
}

/***********************************************************************/
void freeBuffer(Buffer **pBuffer)
{
  Buffer *buffer = *pBuffer;
  *pBuffer = NULL;
  if (buffer == NULL) {
    return;
  }
  if (!buffer->wrapped) {
    FREE(buffer->data);
  }
  FREE(buffer);
}

/**********************************************************************/
size_t bufferLength(Buffer *buffer)
{
  return buffer->length;
}

/**********************************************************************/
size_t contentLength(Buffer *buffer)
{
  return buffer->end - buffer->start;
}

/**********************************************************************/
size_t uncompactedAmount(Buffer *buffer)
{
  return buffer->start;
}

/**********************************************************************/
size_t availableSpace(Buffer *buffer)
{
  return buffer->length - buffer->end;
}

/**********************************************************************/
size_t bufferUsed(Buffer *buffer)
{
  return buffer->end;
}

/***********************************************************************/
int growBuffer(Buffer *buffer, size_t length)
{
  if (buffer == NULL) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot resize NULL buffer");
  }

  if (buffer->wrapped) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot resize wrapped buffer");
  }
  if (buffer->end > length) {
    return logWarningWithStringError(UDS_INVALID_ARGUMENT,
                                     "cannot shrink buffer");
  }

  byte *data;
  int result = reallocateMemory(buffer->data, buffer->length, length,
                                "buffer data", &data);
  if (result != UDS_SUCCESS) {
    return result;
  }

  buffer->data   = data;
  buffer->length = length;
  return UDS_SUCCESS;
}

/***********************************************************************/
bool ensureAvailableSpace(Buffer *buffer, size_t bytes)
{
  if (availableSpace(buffer) >= bytes) {
    return true;
  }
  compactBuffer(buffer);
  return (availableSpace(buffer) >= bytes);
}

/***********************************************************************/
void clearBuffer(Buffer *buffer)
{
  buffer->start = 0;
  buffer->end = buffer->length;
}

/***********************************************************************/
void compactBuffer(Buffer *buffer)
{
  if ((buffer->start == 0) || (buffer->end == 0)) {
    return;
  }
  size_t bytesToMove = buffer->end - buffer->start;
  memmove(buffer->data, buffer->data + buffer->start, bytesToMove);
  buffer->start = 0;
  buffer->end = bytesToMove;
}

/**********************************************************************/
int resetBufferEnd(Buffer *buffer, size_t end)
{
  if (end > buffer->length) {
    return UDS_BUFFER_ERROR;
  }
  buffer->end = end;
  if (buffer->start > buffer->end) {
    buffer->start = buffer->end;
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int skipForward(Buffer *buffer, size_t bytesToSkip)
{
  if (contentLength(buffer) < bytesToSkip) {
    return UDS_BUFFER_ERROR;
  }

  buffer->start += bytesToSkip;
  return UDS_SUCCESS;
}

/**********************************************************************/
int rewindBuffer(Buffer *buffer, size_t bytesToRewind)
{
  if (buffer->start < bytesToRewind) {
    return UDS_BUFFER_ERROR;
  }

  buffer->start -= bytesToRewind;
  return UDS_SUCCESS;
}

/**********************************************************************/
bool hasSameBytes(Buffer *buffer, const byte *data, size_t length)
{
  return ((contentLength(buffer) >= length)
          && (memcmp(buffer->data + buffer->start, data, length) == 0));
}

/**********************************************************************/
bool equalBuffers(Buffer *buffer1, Buffer *buffer2)
{
  return hasSameBytes(buffer1, buffer2->data + buffer2->start,
                      contentLength(buffer2));
}

/**********************************************************************/
int getByte(Buffer *buffer, byte *bytePtr)
{
  if (contentLength(buffer) < sizeof(byte)) {
    return UDS_BUFFER_ERROR;
  }

  *bytePtr = buffer->data[buffer->start++];
  return UDS_SUCCESS;
}

/**********************************************************************/
int peekByte(Buffer *buffer, size_t offset, byte *bytePtr)
{
  if (contentLength(buffer) < (offset + sizeof(byte))) {
    return UDS_BUFFER_ERROR;
  }

  *bytePtr = buffer->data[buffer->start + offset];
  return UDS_SUCCESS;
}

/**********************************************************************/
int putByte(Buffer *buffer, byte b)
{
  if (!ensureAvailableSpace(buffer, sizeof(byte))) {
    return UDS_BUFFER_ERROR;
  }

  buffer->data[buffer->end++] = b;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getBytesFromBuffer(Buffer *buffer, size_t length, void *destination)
{
  if (contentLength(buffer) < length) {
    return UDS_BUFFER_ERROR;
  }

  memcpy(destination, buffer->data + buffer->start, length);
  buffer->start += length;
  return UDS_SUCCESS;
}

/**********************************************************************/
byte *getBufferContents(Buffer *buffer)
{
  return buffer->data + buffer->start;
}

/**********************************************************************/
int copyBytes(Buffer *buffer, size_t length, byte **destinationPtr)
{
  byte *destination;
  int   result = ALLOCATE(length, byte, "copyBytes() buffer",
                          &destination);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = getBytesFromBuffer(buffer, length, destination);
  if (result != UDS_SUCCESS) {
    FREE(destination);
  } else {
    *destinationPtr = destination;
  }
  return result;
}

/**********************************************************************/
int putBytes(Buffer *buffer, size_t length, const void *source)
{
  if (!ensureAvailableSpace(buffer, length)) {
    return UDS_BUFFER_ERROR;
  }
  memcpy(buffer->data + buffer->end, source, length);
  buffer->end += length;
  return UDS_SUCCESS;
}

/**********************************************************************/
int putBuffer(Buffer *target, Buffer *source, size_t length)
{
  if (contentLength(source) < length) {
    return UDS_BUFFER_ERROR;
  }

  int result = putBytes(target, length, getBufferContents(source));
  if (result != UDS_SUCCESS) {
    return result;
  }

  source->start += length;
  return UDS_SUCCESS;
}

/**********************************************************************/
int zeroBytes(Buffer *buffer, size_t length)
{
  if (!ensureAvailableSpace(buffer, length)) {
    return UDS_BUFFER_ERROR;
  }
  memset(buffer->data + buffer->end, 0, length);
  buffer->end += length;
  return UDS_SUCCESS;
}

/**********************************************************************/
int getBoolean(Buffer *buffer, bool *b)
{
  byte by;
  int result = getByte(buffer, &by);
  if (result == UDS_SUCCESS) {
    *b = (by == 1);
  }
  return result;
}

/**********************************************************************/
int putBoolean(Buffer *buffer, bool b)
{
  return putByte(buffer, (byte) (b ? 1 : 0));
}

/**********************************************************************/
int getUInt16BEFromBuffer(Buffer *buffer, uint16_t *ui)
{
  if (contentLength(buffer) < sizeof(uint16_t)) {
    return UDS_BUFFER_ERROR;
  }

  decodeUInt16BE(buffer->data, &buffer->start, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt16BEIntoBuffer(Buffer *buffer, uint16_t ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint16_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeUInt16BE(buffer->data, &buffer->end, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt32BEFromBuffer(Buffer *buffer, uint32_t *ui)
{
  if (contentLength(buffer) < sizeof(uint32_t)) {
    return UDS_BUFFER_ERROR;
  }

  decodeUInt32BE(buffer->data, &buffer->start, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt32BEIntoBuffer(Buffer *buffer, uint32_t ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint32_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeUInt32BE(buffer->data, &buffer->end, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt32BEsFromBuffer(Buffer *buffer, size_t count, uint32_t *ui)
{
  if (contentLength(buffer) < (sizeof(uint32_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    decodeUInt32BE(buffer->data, &buffer->start, ui + i);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt32BEsIntoBuffer(Buffer *buffer, size_t count, const uint32_t *ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint32_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    encodeUInt32BE(buffer->data, &buffer->end, ui[i]);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt64BEsFromBuffer(Buffer *buffer, size_t count, uint64_t *ui)
{
  if (contentLength(buffer) < (sizeof(uint64_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    decodeUInt64BE(buffer->data, &buffer->start, ui + i);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt64BEsIntoBuffer(Buffer *buffer, size_t count, const uint64_t *ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint64_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    encodeUInt64BE(buffer->data, &buffer->end, ui[i]);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt16LEFromBuffer(Buffer *buffer, uint16_t *ui)
{
  if (contentLength(buffer) < sizeof(uint16_t)) {
    return UDS_BUFFER_ERROR;
  }

  decodeUInt16LE(buffer->data, &buffer->start, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt16LEIntoBuffer(Buffer *buffer, uint16_t ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint16_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeUInt16LE(buffer->data, &buffer->end, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt16LEsFromBuffer(Buffer *buffer, size_t count, uint16_t *ui)
{
  if (contentLength(buffer) < (sizeof(uint16_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    decodeUInt16LE(buffer->data, &buffer->start, ui + i);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt16LEsIntoBuffer(Buffer *buffer, size_t count, const uint16_t *ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint16_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    encodeUInt16LE(buffer->data, &buffer->end, ui[i]);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt32LEFromBuffer(Buffer *buffer, uint32_t *ui)
{
  if (contentLength(buffer) < sizeof(uint32_t)) {
    return UDS_BUFFER_ERROR;
  }

  decodeUInt32LE(buffer->data, &buffer->start, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt32LEIntoBuffer(Buffer *buffer, uint32_t ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint32_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeUInt32LE(buffer->data, &buffer->end, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putInt64LEIntoBuffer(Buffer *buffer, int64_t i)
{
  if (!ensureAvailableSpace(buffer, sizeof(int64_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeInt64LE(buffer->data, &buffer->end, i);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt64LEFromBuffer(Buffer *buffer, uint64_t *ui)
{
  if (contentLength(buffer) < sizeof(uint64_t)) {
    return UDS_BUFFER_ERROR;
  }

  decodeUInt64LE(buffer->data, &buffer->start, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt64LEIntoBuffer(Buffer *buffer, uint64_t ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint64_t))) {
    return UDS_BUFFER_ERROR;
  }

  encodeUInt64LE(buffer->data, &buffer->end, ui);
  return UDS_SUCCESS;
}

/**********************************************************************/
int getUInt64LEsFromBuffer(Buffer *buffer, size_t count, uint64_t *ui)
{
  if (contentLength(buffer) < (sizeof(uint64_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    decodeUInt64LE(buffer->data, &buffer->start, ui + i);
  }
  return UDS_SUCCESS;
}

/**********************************************************************/
int putUInt64LEsIntoBuffer(Buffer *buffer, size_t count, const uint64_t *ui)
{
  if (!ensureAvailableSpace(buffer, sizeof(uint64_t) * count)) {
    return UDS_BUFFER_ERROR;
  }

  for (unsigned int i = 0; i < count; i++) {
    encodeUInt64LE(buffer->data, &buffer->end, ui[i]);
  }
  return UDS_SUCCESS;
}

