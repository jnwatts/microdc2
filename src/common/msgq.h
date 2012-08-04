/* msgq.h - Message passing that is non-blocking
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

#ifndef COMMON_MSGQ_H
#define COMMON_MSGQ_H

#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include "byteq.h"

typedef enum {
    MSGQ_END = 0,
    MSGQ_INT,	/* int (signed or not) */
    MSGQ_INT32, /* int32 (signed or not) */
    MSGQ_INT64,	/* int64 (signed or not) */
    MSGQ_BOOL,	/* bool */
    MSGQ_STR,	/* char * */
    MSGQ_BLOB,	/* void *, size_t */
    MSGQ_STRARY		/* char ** (terminated by NULL entry) */
} MsgQType;

struct _MsgQ {
    int fd;
    ByteQ *queue;
};

typedef struct _MsgQ MsgQ;

MsgQ *msgq_new(int fd);
void msgq_free(MsgQ *mq);
ssize_t msgq_read(MsgQ *mq);
ssize_t msgq_write(MsgQ *mq);
int msgq_read_complete_msg(MsgQ *mq);
int msgq_write_all(MsgQ *mq);
bool msgq_has_partial_msg(MsgQ *mq);
bool msgq_has_complete_msg(MsgQ *mq);
void msgq_put(MsgQ *mq, ...);
void msgq_get(MsgQ *mq, ...);
void msgq_peek(MsgQ *mq, ...);
int msgq_put_sync(MsgQ *mq, ...);
int msgq_get_sync(MsgQ *mq, ...);

#endif
