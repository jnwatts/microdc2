/* msgq.c - Message passing that is non-blocking
 *
 * Copyright (C) 2005 Oskar Liljeblad
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
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "xalloc.h"		/* Gnulib */
#include "byteq.h"
#include "msgq.h"

#define DEFAULT_MSGQ_BYTEQ_SIZE	128

/* It is assumed that fd is in non-blocking mode.
 */
MsgQ *
msgq_new(int fd)
{
    MsgQ *mq = xmalloc(sizeof(MsgQ));
    mq->fd = fd;
    mq->queue = byteq_new(DEFAULT_MSGQ_BYTEQ_SIZE);
    return mq;
}

void
msgq_free(MsgQ *mq)
{
    if (mq != NULL) {
        byteq_free(mq->queue);
        free(mq);
    }
}

ssize_t
msgq_write(MsgQ *mq)
{
    return byteq_write(mq->queue, mq->fd);
}

/* This function returns -1 on error, and 1 on success.
 */
int
msgq_write_all(MsgQ *mq)
{
    int cur = mq->queue->cur;
    if (byteq_full_write(mq->queue, mq->fd) < cur)
        return -1;
    return 1;
}

ssize_t
msgq_read(MsgQ *mq)
{
    return byteq_read(mq->queue, mq->fd);
}

/* This assumes that mq->queue is empty!
 * It returns 0 on EOF, -1 on error, and 1 on success.
 */
int
msgq_read_complete_msg(MsgQ *mq)
{
    size_t size;
    int res;

    if (mq->queue->cur < sizeof(size)) {
        res = byteq_full_read_upto(mq->queue, mq->fd, sizeof(size));
        if (res < sizeof(size))
            return (errno == 0 ? 0 : -1);
    }

    memcpy(&size, mq->queue->buf, sizeof(size));
    if (mq->queue->cur < sizeof(size)+size) {
        res = byteq_full_read_upto(mq->queue, mq->fd, sizeof(size)+size);
        if (res < size)
            return (errno == 0 ? 0 : -1);
    }

    return 1;
}

bool
msgq_has_partial_msg(MsgQ *mq)
{
    return mq->queue->cur != 0;
}

bool
msgq_has_complete_msg(MsgQ *mq)
{
    size_t size;

    if (mq->queue->cur < sizeof(size))
        return false;
    memcpy(&size, mq->queue->buf, sizeof(size));
    if (mq->queue->cur < sizeof(size) + size)
        return false;
    return true;
}

size_t
msgq_calc_put_args_size(MsgQ *mq, va_list args)
{
    size_t size = 0;
    MsgQType type;

    while ((type = va_arg(args, MsgQType)) != MSGQ_END) {
        if (type == MSGQ_INT) {
            int intval = va_arg(args, int);
            size += sizeof(intval);
        } else if (type == MSGQ_INT32) {
            int32_t intval = va_arg(args, int32_t);
            size += sizeof(intval);
        } else if (type == MSGQ_INT64) {
            int64_t intval = va_arg(args, int64_t);
	        size += sizeof(intval);
        } else if (type == MSGQ_BOOL) {
            bool boolval = va_arg(args, int);
            size += sizeof(boolval);
        } else if (type == MSGQ_STR) {
            char *strval = va_arg(args, char *);
            size += sizeof(size_t) + (strval == NULL ? 0 : strlen(strval)+1);
        } else if (type == MSGQ_BLOB) {
            void *dataval = va_arg(args, void *);
            size_t sizeval = va_arg(args, size_t);
            size += sizeof(size_t) + (dataval == NULL ? 0 : sizeval);
        } else if (type == MSGQ_STRARY) {
            char **aryval = va_arg(args, char **);
            size += sizeof(size_t);
            if (aryval != NULL) {
                for (; *aryval != NULL; aryval++)
                    size += sizeof(size_t) + strlen(*aryval)+1;
            }
        }
    }
    return size;
}

void
msgq_vput(MsgQ *mq, size_t size, va_list args)
{
    MsgQType type;

    byteq_append(mq->queue, &size, sizeof(size));
    while ((type = va_arg(args, MsgQType)) != MSGQ_END) {
        if (type == MSGQ_INT) {
            int intval = va_arg(args, int);
            byteq_append(mq->queue, &intval, sizeof(intval));
        } else if (type == MSGQ_INT32) {
            int32_t intval = va_arg(args, int32_t);
            byteq_append(mq->queue, &intval, sizeof(intval));
        } else if (type == MSGQ_INT64) {
            int64_t intval = va_arg(args, int64_t);
            byteq_append(mq->queue, &intval, sizeof(intval));
        } else if (type == MSGQ_BOOL) {
            bool boolval = va_arg(args, int);
            byteq_append(mq->queue, &boolval, sizeof(boolval));
        } else if (type == MSGQ_STR) {
            char *strval = va_arg(args, char *);
            size_t len = (strval == NULL ? SIZE_MAX : strlen(strval)+1);
            byteq_append(mq->queue, &len, sizeof(len));
            if (strval != NULL)
                byteq_append(mq->queue, strval, len);
        } else if (type == MSGQ_BLOB) {
            void *dataval = va_arg(args, void *);
            size_t sizeval = va_arg(args, size_t);
            if (dataval == NULL)
                sizeval = SIZE_MAX;
            byteq_append(mq->queue, &sizeval, sizeof(sizeval));
            if (dataval != NULL)
                byteq_append(mq->queue, dataval, sizeval);
        } else if (type == MSGQ_STRARY) {
            char **aryval = va_arg(args, char **);
            size_t len;
            if (aryval == NULL) {
                len = SIZE_MAX;
                byteq_append(mq->queue, &len, sizeof(len));
            } else {
                for (len = 0; aryval[len] != NULL; len++);
                byteq_append(mq->queue, &len, sizeof(len));
                for (; *aryval != NULL; aryval++) {
                    len = strlen(*aryval)+1;
                    byteq_append(mq->queue, &len, sizeof(len));
                    byteq_append(mq->queue, *aryval, len);
                }
            }
        }
    }
}

int
msgq_put_sync(MsgQ *mq, ...)
{
    va_list args;
    size_t size;

    va_start(args, mq);
    size = msgq_calc_put_args_size(mq, args);
    va_end(args);
    va_start(args, mq);
    msgq_vput(mq, size, args);
    va_end(args);

    return msgq_write_all(mq); /* FIXME handle error? */
}

void
msgq_put(MsgQ *mq, ...)
{
    va_list args;
    size_t size;

    va_start(args, mq);
    size = msgq_calc_put_args_size(mq, args);
    va_end(args);
    va_start(args, mq);
    msgq_vput(mq, size, args);
    va_end(args);
}

/* This function returns the number of bytes that were in the message
 * (including header).
 */
static size_t
msgq_vget(MsgQ *mq, va_list args)
{
    size_t size;
    char *buf;
    MsgQType type;

    buf = mq->queue->buf;
    memcpy(&size, buf, sizeof(size));
    buf += sizeof(size);

    while ((type = va_arg(args, MsgQType)) != MSGQ_END) {
	if (type == MSGQ_INT) {
	    int *intptr = va_arg(args, int *);
	    memcpy(intptr, buf, sizeof(*intptr));
	    buf += sizeof(*intptr);
	} else if (type == MSGQ_INT32) {
	    int32_t *intptr = va_arg(args, int32_t *);
	    memcpy(intptr, buf, sizeof(*intptr));
	    buf += sizeof(*intptr);
	} else if (type == MSGQ_INT64) {
	    int64_t *intptr = va_arg(args, int64_t *);
	    memcpy(intptr, buf, sizeof(*intptr));
	    buf += sizeof(*intptr);
        } else if (type == MSGQ_BOOL) {
            bool *boolptr = va_arg(args, bool *);
            memcpy(boolptr, buf, sizeof(*boolptr));
            buf += sizeof(*boolptr);
	} else if (type == MSGQ_STR) {
	    char **strptr = va_arg(args, char **);
	    size_t len;
	    memcpy(&len, buf, sizeof(len));
	    buf += sizeof(len);
	    if (len == SIZE_MAX) {
		    *strptr = NULL;
	    } else {
		    *strptr = xstrdup(buf);
		    buf += strlen(buf)+1;
	    }
	} else if (type == MSGQ_BLOB) {
	    void **dataptr = va_arg(args, void **);
	    size_t *sizeptr = va_arg(args, size_t *);
	    size_t len;
	    memcpy(&len, buf, sizeof(len));
	    buf += sizeof(len);
	    if (len == SIZE_MAX) {
		*dataptr = NULL;
		*sizeptr = 0;
	    } else {
		*dataptr = xmemdup(buf, len);
		*sizeptr = len;
		buf += len;
	    }
	} else if (type == MSGQ_STRARY) {
	    char ***aryptr = va_arg(args, char ***);
	    size_t c;
	    size_t len;

	    memcpy(&len, buf, sizeof(len)); buf += sizeof(len);
	    if (len == SIZE_MAX) {
		*aryptr = NULL;
	    } else {
		*aryptr = xmalloc((len+1) * sizeof(char *));
		for (c = 0; c < len; c++) {
		    size_t lenstr;
		    memcpy(&lenstr, buf, sizeof(lenstr)); buf += sizeof(lenstr);
		    (*aryptr)[c] = xmemdup(buf, lenstr); buf += lenstr;
		}
		(*aryptr)[c] = NULL;
	    }
	}
    }

    return sizeof(size)+size;
}

void
msgq_peek(MsgQ *mq, ...)
{
    va_list args;

    va_start(args, mq);
    msgq_vget(mq, args);
    va_end(args);
}

/* This function assumes that msgq_has_complete_msg is true.
 */
void
msgq_get(MsgQ *mq, ...)
{
    size_t count;
    va_list args;

    va_start(args, mq);
    count = msgq_vget(mq, args);    
    va_end(args);
    byteq_remove(mq->queue, count);
}

int
msgq_get_sync(MsgQ *mq, ...)
{
    int res;
    size_t count;
    va_list args;
    
    res = msgq_read_complete_msg(mq);
    if (res <= 0)
        return res;

    va_start(args, mq);
    count = msgq_vget(mq, args);
    va_end(args);
    byteq_remove(mq->queue, count);

    return 1;
}
