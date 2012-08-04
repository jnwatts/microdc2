/* lookup.c - Performing name lookup with getaddrinfo
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
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/signal.h>
#include <netdb.h>
#include <signal.h>		/* POSIX.1 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>		/* Gnulib */
#include <string.h>
#include <errno.h>
#include "gettext.h"            /* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xalloc.h"		/* Gnulib */
#include "common/msgq.h"
#include "common/byteq.h"
#include "common/ptrv.h"
#include "microdc.h"

struct _DCLookup {
    DCLookupCallback callback;
    void *data;
    bool cancelled;
};

static PtrV *pending_lookups = NULL;
MsgQ *lookup_request_mq = NULL;
MsgQ *lookup_result_mq = NULL;
pid_t lookup_child;

static struct addrinfo *
data_to_addrinfo(void *databuf, size_t size)
{
    struct addrinfo *first_ai = NULL;
    struct addrinfo *prev_ai = NULL;
    char *data = databuf;
    char *dataend = data + size;

    while (data < dataend) {
        struct addrinfo *ai;
        bool has_sockaddr;
        bool has_canon;

        ai = xmalloc(sizeof(*ai));
        if (first_ai == NULL)
            first_ai = ai;
        else
            prev_ai->ai_next = ai;

        memcpy(&ai->ai_flags, data, sizeof(ai->ai_flags)); data += sizeof(ai->ai_flags);
        memcpy(&ai->ai_family, data, sizeof(ai->ai_family)); data += sizeof(ai->ai_family);
        memcpy(&ai->ai_socktype, data, sizeof(ai->ai_socktype)); data += sizeof(ai->ai_socktype);
        memcpy(&ai->ai_protocol, data, sizeof(ai->ai_protocol)); data += sizeof(ai->ai_protocol);
        memcpy(&ai->ai_addrlen, data, sizeof(ai->ai_addrlen)); data += sizeof(ai->ai_addrlen);
        memcpy(&has_sockaddr, data, sizeof(has_sockaddr)); data += sizeof(has_sockaddr);
        if (has_sockaddr) {
            ai->ai_addr = xmemdup(data, ai->ai_addrlen);
            data += ai->ai_addrlen;
        } else {
            ai->ai_addr = NULL;
        }
        memcpy(&has_canon, data, sizeof(has_sockaddr)); data += sizeof(has_canon);
        if (has_canon) {
            ai->ai_canonname = xstrdup(data);
            data += strlen(ai->ai_canonname)+1;
        } else {
            ai->ai_canonname = NULL;
        }

        ai->ai_next = NULL;
        prev_ai = ai;
    }

    return first_ai;
}

static void
addrinfo_to_data(const struct addrinfo *first_ai, void **dataptr, size_t *sizeptr)
{
    const struct addrinfo *ai;
    size_t size = 0;
    char *data;

    for (ai = first_ai; ai != NULL; ai = ai->ai_next) {
        size += sizeof(ai->ai_flags) + sizeof(ai->ai_family)
                + sizeof(ai->ai_socktype) + sizeof(ai->ai_protocol)
                + sizeof(ai->ai_addrlen) + sizeof(bool) + sizeof(bool);
        if (ai->ai_addr != NULL)
            size += ai->ai_addrlen;
        if (ai->ai_canonname != NULL)
            size += strlen(ai->ai_canonname)+1;
    }

    data = size == 0 ? NULL : xmalloc(size);
    *dataptr = (void *) data;
    *sizeptr = size;

    for (ai = first_ai; ai != NULL; ai = ai->ai_next) {
        bool has_sockaddr = ai->ai_addr != NULL;
        bool has_canon = ai->ai_canonname != NULL;

        memcpy(data, &ai->ai_flags, sizeof(ai->ai_flags)); data += sizeof(ai->ai_flags);
        memcpy(data, &ai->ai_family, sizeof(ai->ai_family)); data += sizeof(ai->ai_family);
        memcpy(data, &ai->ai_socktype, sizeof(ai->ai_socktype)); data += sizeof(ai->ai_socktype);
        memcpy(data, &ai->ai_protocol, sizeof(ai->ai_protocol)); data += sizeof(ai->ai_protocol);
        memcpy(data, &ai->ai_addrlen, sizeof(ai->ai_addrlen)); data += sizeof(ai->ai_addrlen);
        memcpy(data, &has_sockaddr, sizeof(has_sockaddr)); data += sizeof(has_sockaddr);
        if (has_sockaddr) {
            memcpy(data, ai->ai_addr, ai->ai_addrlen);
            data += ai->ai_addrlen;
        }
        memcpy(data, &has_canon, sizeof(has_canon)); data += sizeof(has_canon);
        if (has_canon) {
            memcpy(data, ai->ai_canonname, strlen(ai->ai_canonname)+1);
            data += strlen(ai->ai_canonname)+1;
        }
    }
}

static void
free_read_addrinfo(struct addrinfo *ai)
{
    while (ai != NULL) {
	struct addrinfo *next_ai;

	free(ai->ai_addr);
	free(ai->ai_canonname);

	next_ai = ai->ai_next;
	free(ai);
	ai = next_ai;
    }
}

static void
__attribute__((noreturn))
lookup_main(int request_fd[2], int result_fd[2])
{
    MsgQ *request_mq;
    MsgQ *result_mq;
    struct sigaction sigact;

    close(request_fd[1]);
    close(result_fd[0]);
    request_mq = msgq_new(request_fd[0]);
    result_mq = msgq_new(result_fd[1]);

    /* Inability to register these signals is not a fatal error. */
    sigact.sa_flags = SA_RESTART;
    sigact.sa_handler = SIG_IGN;
#ifdef HAVE_STRUCT_SIGACTION_SA_RESTORER
    sigact.sa_restorer = NULL;
#endif
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGUSR1, &sigact, NULL);
    sigaction(SIGCHLD, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);

    while (msgq_read_complete_msg(request_mq) > 0) {
        char *node;
        char *service;
        void *data;
        size_t size;
        struct addrinfo *hints_ai;
        struct addrinfo *result_ai = NULL;
        int rc;

        /* XXX: msgq_get_sync, check its result, and use for (;;) */
        msgq_get(request_mq, MSGQ_STR, &node, MSGQ_STR, &service, MSGQ_BLOB, &data, &size, MSGQ_END);
        hints_ai = data_to_addrinfo(data, size);
        rc = getaddrinfo(node, service, hints_ai, &result_ai);
        free(node);
        free(service);
        free_read_addrinfo(hints_ai);
        free(data);
        addrinfo_to_data(result_ai, &data, &size);
        /* XXX: msgq_put_sync, check its result, and get rid of msgq_write_all */
        msgq_put(result_mq, MSGQ_INT, rc, MSGQ_BLOB, data, size, MSGQ_END);
        free(data);
        if (msgq_write_all(result_mq) < 0)
            break;
    }

    /* msgq_read_complete_msg may have failed if it returned < 0.
     * But we can't print any errors from this process (it would
     * interfere with the readline-managed display, so just exit
     * gracefully.
     */

    msgq_free(request_mq);
    msgq_free(result_mq);
    close(request_fd[0]);
    close(result_fd[1]);
    exit(EXIT_SUCCESS);
}

void
lookup_request_fd_writable(void)
{
    int res;

    res = msgq_write(lookup_request_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, true, "lookup request pipe");
        running = false;
        return;
    }
    if (!msgq_has_partial_msg(lookup_request_mq))
        FD_CLR(lookup_request_mq->fd, &write_fds);
}

void
lookup_result_fd_readable(void)
{
    int res;

    res = msgq_read(lookup_result_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, false, "lookup result pipe");
        running = false;
        return;
    }
    while (msgq_has_complete_msg(lookup_result_mq)) {
        int rc;
        void *data;
        size_t size;
        struct addrinfo *ai;
        DCLookup *lookup;

        msgq_get(lookup_result_mq, MSGQ_INT, &rc, MSGQ_BLOB, &data, &size, MSGQ_END);
        ai = data_to_addrinfo(data, size);
        free(data);
        lookup = ptrv_remove_first(pending_lookups);
        if (!lookup->cancelled)
            lookup->callback(rc, ai, lookup->data);
        free(lookup);
        free_read_addrinfo(ai);
    }
}

void
cancel_lookup_request(DCLookup *lookup)
{
    int c;

    for (c = 0; c < pending_lookups->cur; c++) {
        if (lookup == pending_lookups->buf[c]) {
            if (c == 0) {
                lookup->cancelled = true;
            } else {
                ptrv_remove_range(pending_lookups, c, c+1);
            }
            break;
        }
    }
}

DCLookup *
add_lookup_request(const char *node, const char *service, const struct addrinfo *hints, DCLookupCallback callback, void *userdata)
{
    void *data;
    size_t size;
    DCLookup *lookup;

    addrinfo_to_data(hints, &data, &size);
    msgq_put(lookup_request_mq, MSGQ_STR, node, MSGQ_STR, service, MSGQ_BLOB, data, size, MSGQ_END);
    free(data);
    FD_SET(lookup_request_mq->fd, &write_fds);

    lookup = xmalloc(sizeof(DCLookup));
    lookup->callback = callback;
    lookup->data = userdata;
    lookup->cancelled = false;
    ptrv_append(pending_lookups, lookup);
    
    return lookup;
}

bool
lookup_init(void)
{
    int request_fd[2];
    int result_fd[2];

    if (pipe(request_fd) != 0 || pipe(result_fd) != 0) {
        warn(_("Cannot create pipe pair - %s\n"), errstr);
        return false;
    }
    if (!fd_set_nonblock_flag(request_fd[1], true)
            || !fd_set_nonblock_flag(result_fd[0], true)) {
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        return false;
    }

    lookup_child = fork();
    if (lookup_child < 0) {
        warn(_("Cannot create process - %s\n"), errstr);
        return false;
    }
    if (lookup_child == 0)
        lookup_main(request_fd, result_fd);

    pending_lookups = ptrv_new();
    close(request_fd[0]);
    close(result_fd[1]);
    lookup_request_mq = msgq_new(request_fd[1]);
    lookup_result_mq = msgq_new(result_fd[0]);
    FD_SET(lookup_result_mq->fd, &read_fds);
    return true;
}

void
lookup_finish(void)
{
    if (pending_lookups != NULL) {
        ptrv_foreach(pending_lookups, free);
        ptrv_free(pending_lookups);
    }
    if (lookup_request_mq != NULL) {
        close(lookup_request_mq->fd);
        msgq_free(lookup_request_mq);
    }
    if (lookup_result_mq != NULL) {
        close(lookup_result_mq->fd);
        msgq_free(lookup_result_mq);
    }
}
