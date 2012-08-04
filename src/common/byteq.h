/* byteq.h - A queue structure holding bytes, designed for I/O tasks
 *
 * Copyright (C) 2004, 2005 Oskar Liljeblad
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef BYTEQ_H
#define BYTEQ_H

#include <config.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct _ByteQ ByteQ;

struct _ByteQ {
    char *buf;	  /* Byte buffer */
    size_t cur;   /* Current number of bytes used in buffer */
    size_t max;   /* Number of bytes (used or not) in buffer */
#ifdef HAVE_FOPENCOOKIE
    FILE *file;     /* Custom stream for speeding up byteq_(v)appendf */
#endif
};

ByteQ *byteq_new(size_t initial_size);
void byteq_free(ByteQ *bq);
void byteq_append(ByteQ *bq, void *data, size_t len);
int byteq_vappendf(ByteQ *bq, const char *format, va_list ap) __attribute__ ((format (printf, 2, 0)));
int byteq_appendf(ByteQ *bq, const char *format, ...) __attribute__ ((format (printf, 2, 3)));
void byteq_remove(ByteQ *bq, size_t len);
void byteq_assure(ByteQ *bq, size_t max);
ssize_t byteq_read(ByteQ *bq, int fd);
ssize_t byteq_full_read(ByteQ *bq, int fd);
ssize_t byteq_read_upto(ByteQ *bq, int fd, size_t len);
ssize_t byteq_full_read_upto(ByteQ *bq, int fd, size_t len);
ssize_t byteq_write(ByteQ *bq, int fd);
ssize_t byteq_full_write(ByteQ *bq, int fd);
void byteq_clear(ByteQ *bq);
ssize_t byteq_sendto(ByteQ *bq, int fd, int flags, const struct sockaddr *to, socklen_t tolen);
ssize_t byteq_recvfrom(ByteQ *bq, int fd, int flags, struct sockaddr *from, socklen_t *fromlen);

#endif
