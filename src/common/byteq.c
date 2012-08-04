/* byteq.c - A queue structure holding bytes, designed for I/O tasks
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

#include <config.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <assert.h>
#include <sys/param.h>
#include "xvasprintf.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xalloc.h"		/* Gnulib */
#include "byteq.h"
#include "full-read.h"
#include "full-write.h"

/* Arbitrary enlargement of the byteq. This function only guarantees tha
 * the structure will hold one more byte after the call.
 * Note: bq->max may never be 0.
 */
static inline void
byteq_enlarge(ByteQ *bq)
{
    bq->max *= 2;
    bq->buf = xrealloc(bq->buf, bq->max);
}

/* Allocate a new byteq with space for initial_size bytes.
 */
ByteQ *
byteq_new(size_t initial_size)
{
    ByteQ *bq;

    bq = xmalloc(sizeof(ByteQ));
    bq->cur = 0;
    bq->max = MAX(1, initial_size);
    bq->buf = xmalloc(initial_size);

    return bq;
}

/* Free byteq and its buffer.
 */
void
byteq_free(ByteQ *bq)
{
    if (bq != NULL) {
	free(bq->buf);
	free(bq);
    }
}

/* Assure the queue has space for max bytes.
 */
void
byteq_assure(ByteQ *bq, size_t max)
{
    if (max > bq->max) {
    	bq->max = max;
    	bq->buf = xrealloc(bq->buf, bq->max);
    }
}

/* Append bytes to the byteq.
 */
void
byteq_append(ByteQ *bq, void *data, size_t len)
{
    if (len > 0) {
	byteq_assure(bq, bq->cur+len);
	memcpy(bq->buf + bq->cur, data, len);
	bq->cur += len;
    }
}

/* Append bytes to the byteq, va_list style.
 * This function cannot fail!
 */
int
byteq_vappendf(ByteQ *bq, const char *format, va_list ap)
{
    char *str;
    size_t len;

    str = xvasprintf(format, ap);
    len = strlen(str);
    byteq_append(bq, str, len);
    free(str);
    return len;
}

/* Append bytes to the byteq, printf style.
 */
int
byteq_appendf(ByteQ *bq, const char *format, ...)
{
    va_list args;
    size_t len;

    va_start(args, format);
    len = byteq_vappendf(bq, format, args);
    va_end(args);

    return len;
}

/* Remove len bytes from the beginning of the queue.
 */
void
byteq_remove(ByteQ *bq, size_t len)
{
    assert(bq->cur >= len);

    bq->cur -= len;
    if (bq->cur > 0)
	memmove(bq->buf, bq->buf + len, bq->cur);
}

/* Read once from fd into queue, so that queue ends up being
 * size bytes. This is the same as calling
 *
 *   byteq_assure(bq, len);
 *   byteq_read(bq, fd);
 *
 */
ssize_t
byteq_read_upto(ByteQ *bq, int fd, size_t len)
{
    ssize_t res;

    if (len <= bq->cur)
    	return 0;

    byteq_assure(bq, bq->cur + len);
    res = read(fd, bq->buf + bq->cur, len - bq->cur);
    if (res > 0)
    	bq->cur += res;

    return res;
}

ssize_t
byteq_full_read_upto(ByteQ *bq, int fd, size_t len)
{
    ssize_t res;

    if (len <= bq->cur)
    	return 0;

    byteq_assure(bq, bq->cur + len);
    res = full_read(fd, bq->buf + bq->cur, len - bq->cur);
    if (res > 0)
    	bq->cur += res;

    return res;
}

/* Read from fd into queue. If queue if full from beginning,
 * enlarge it first.
 */
ssize_t
byteq_read(ByteQ *bq, int fd)
{
    ssize_t res;

    if (bq->cur == bq->max)
    	byteq_enlarge(bq);

    res = read(fd, bq->buf + bq->cur, bq->max - bq->cur);
    if (res > 0)
    	bq->cur += res;

    return res;
}

/* Should make sure the queue is non-full prior to calling this. */
ssize_t
byteq_full_read(ByteQ *bq, int fd)
{
    ssize_t res = 0;

    res = full_read(fd, bq->buf+bq->cur, bq->max-bq->cur);
    if (res > 0)
    	bq->cur += res;

    return res;
}

/* Write all of queue, removing written data from queue
 * afterwards.
 * Should make sure the queue is non-empty prior to calling this.
 */
ssize_t
byteq_write(ByteQ *bq, int fd)
{
    ssize_t res;

    res = write(fd, bq->buf, bq->cur);
    if (res > 0) {
        bq->cur -= res;
        if (bq->cur > 0)
            memmove(bq->buf, bq->buf + res, bq->cur);
    }

    return res;
}

/* Should make sure the queue is non-empty prior to calling this. */
ssize_t
byteq_full_write(ByteQ *bq, int fd)
{
    ssize_t res;

    res = full_write(fd, bq->buf, bq->cur);
    if (res > 0) {
	bq->cur -= res;
	if (bq->cur > 0)
            memmove(bq->buf, bq->buf + res, bq->cur);
    }

    return res;
}

void
byteq_clear(ByteQ *bq)
{
    bq->cur = 0;
}

ssize_t
byteq_sendto(ByteQ *bq, int fd, int flags, const struct sockaddr *to, socklen_t tolen)
{
    ssize_t res;

    res = sendto(fd, bq->buf, bq->cur, flags, to, tolen);
    if (res > 0) {
	bq->cur -= res;
	if (bq->cur > 0)
            memmove(bq->buf, bq->buf + res, bq->cur);
    }

    return res;
}

ssize_t
byteq_recvfrom(ByteQ *bq, int fd, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t res;

    if (bq->cur == bq->max)
    	byteq_enlarge(bq);

    res = recvfrom(fd, bq->buf+bq->cur, bq->max-bq->cur, flags, from, fromlen);
    if (res > 0)
    	bq->cur += res;

    return res;
}
