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
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include "gettext.h"            /* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xalloc.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "common/msgq.h"
#include "common/byteq.h"
#include "common/ptrv.h"
#include "microdc.h"

#include "tth/tth.h"

MsgQ *hash_request_mq = NULL;
MsgQ *hash_result_mq = NULL;
pid_t hash_child;

static void
__attribute__((noreturn))
hash_main(int request_fd[2], int result_fd[2])
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
        char *filename, *hash;
        struct stat st;

        msgq_get(request_mq, MSGQ_STR, &filename, MSGQ_END);

        /*
        fprintf(stderr, "HASH: begin processing %s\n", filename);
        fflush(stderr);
        */

        if (stat(filename, &st) < 0) {
            hash = xasprintf("FAILED");
        } else {
            char* tthl = NULL;
            size_t tthl_size;
            hash = tth(filename, &tthl, &tthl_size);
            if (tthl != NULL)
                free(tthl);
        }

        /*
        fprintf(stderr, "HASH: %s: %s\n", filename, hash);
        fflush(stderr);
        */

        msgq_put(result_mq, MSGQ_STR, hash, MSGQ_END);
        free(hash);
        if (msgq_write_all(result_mq) < 0) {
            /*
            fprintf(stderr, "HASH: msgq_write_all error: %d, %s\n", errno, errstr);
            fflush(stderr);
            */
            break;
        }
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

bool
hash_init(void)
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

    hash_child = fork();
    if (hash_child < 0) {
        warn(_("Cannot create process - %s\n"), errstr);
        return false;
    }
    if (hash_child == 0) {
        setpriority(PRIO_PROCESS, 0, 16);
        hash_main(request_fd, result_fd);
    }

    close(request_fd[0]);
    close(result_fd[1]);
    hash_request_mq = msgq_new(request_fd[1]);
    hash_result_mq = msgq_new(result_fd[0]);
    return true;
}

void
hash_finish(void)
{
    if (hash_request_mq != NULL) {
        close(hash_request_mq->fd);
        msgq_free(hash_request_mq);
    }
    if (hash_result_mq != NULL) {
        close(hash_result_mq->fd);
        msgq_free(hash_result_mq);
    }
}
