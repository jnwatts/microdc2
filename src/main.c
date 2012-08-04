/* main.c - Main routine and some common functions
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
#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>		/* POSIX */
#include <stdlib.h>		/* C89 */
#include <string.h>		/* C89 */
#include <sys/socket.h>		/* POSIX */
#include <sys/stat.h>		/* ? */
#include <sys/types.h>		/* ? */
#include <sys/wait.h>		/* POSIX: waitpid */
#include <unistd.h>		/* POSIX */
#include <inttypes.h>		/* ? */
#include <fcntl.h>		/* ? */
#include <getopt.h>		/* Gnulib/POSIX */
#include <locale.h>		/* ? */
#include "full-write.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "version-etc.h"	/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "quote.h"		/* Gnulib */
#include "sig2str.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "dirname.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "human.h"		/* Gnulib */
#include "getline.h"		/* Gnulib/GNU Libc */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/error.h"
#include "common/strleftcmp.h"
#include "common/tempdir.h"
#include "common/tempfailure.h"
#include "common/msgq.h"
#include "microdc.h"

enum {
    VERSION_OPT = 256,
    HELP_OPT
};

#define LISTEN_QUEUE_CONNS 16

/* interactive state (move to transfer.c/browse.c) */
DCFileList *browse_list = NULL;
DCUserInfo *browse_user = NULL;
char *browse_path = NULL;
char *browse_path_previous = NULL;
bool browsing_myself = false;

/* misc */
uint64_t bytes_received = 0;
uint64_t bytes_sent = 0;
uint64_t max_speed = 0, prev_max_speed = 0;
uint16_t listen_port = 0;
bool running = true;
fd_set read_fds;
fd_set write_fds;
PtrV *delete_files = NULL;
PtrV *delete_dirs = NULL;
static PtrV *search_udpmsg_out;	/* pending outgoing search results */
pid_t shell_child = -1;
struct sockaddr_in local_addr;
HMap *user_conns = NULL;
HMap *pending_userinfo = NULL;	/* users we want to communicate with, either
                                 * us connecting to them or they connecting
                                 * to us. (UserInfo->nick => UserInfo) */


/* XXX: change to ("Copyright %s %d Oskar Liljeblad", gettext("(C)"), 2005) */
const char version_etc_copyright[] =
    "Copyright (C) 2006 Vladimir Chugunov, based on Oskar Liljeblad's microdc 0.11.0\nmicrodc is copyright (C) 2004, 2005 Oskar Liljeblad.";

static uint32_t user_conn_unknown_last = 0;
static PtrV *user_conn_unknown_free = NULL;
static ByteQ *search_recvq;

static pid_t main_process_id;
static int signal_pipe[2] = { -1, -1 };
static int listen_socket = -1;
static int search_socket = -1;

static const char *short_opts = "c:n";
static struct option long_opts[] = {
    { "config", required_argument, NULL, 'c' },
    { "no-config", no_argument, NULL, 'n' },
    { "version", no_argument, NULL, VERSION_OPT },
    { "help", no_argument, NULL, HELP_OPT },
    { 0, }
};

void
warn_file_error(int res, bool write, const char *filename)
{
    if (write) {
	warn(_("%s: Cannot write to file - %s\n"), quotearg(filename), errstr);
    } else {
	if (res < 0) {
	    warn(_("%s: Cannot read from file - %s\n"), quotearg(filename), errstr);
	} else {
	    warn(_("%s: Premature end of file\n"), quotearg(filename));
	}
    }
}

void
warn_socket_error(int res, bool write, const char *format, ...)
{
    char *subject;
    va_list args;

    va_start(args, format);
    subject = xvasprintf(format, args);
    va_end(args);
    if (write) {
        warn("Cannot send to %s - %s\n", subject, errstr);
    } else {
        if (res < 0) {
            warn("Cannot receive from %s - %s\n", subject, errstr);
        } else {
            warn("Disconnected from %s.\n", subject);
        }
    }
    free(subject);
}

bool
get_package_file(const char *name, char **outname)
{
    *outname = xasprintf("%s/.%s/%s", getenv_default("HOME", ""), PACKAGE, name);
    return true;
}

bool
has_user_conn(DCUserInfo *ui, DCTransferDirection dir)
{
    int c;
    for (c = 0; c < ui->conn_count; c++) {
	if (ui->conn[c]->dir == dir)
    	    return true;
    }
    return false;
}

static void
update_user_connection_name(DCUserConn *uc, const char *format, ...)
{
    char *newname;
    va_list args;
    
    va_start(args, format);
    newname = xvasprintf(format, args);
    va_end(args);

    hmap_remove(user_conns, uc->name);
    flag_putf(DC_DF_CONNECTIONS, _("User connection %s renamed to %s.\n"), quote_n(0, uc->name), quote_n(1, newname));
    if (strchr(uc->name, '|') == NULL)
        ptrv_append(user_conn_unknown_free, uc->name);
    else
        free(uc->name);
    uc->name = newname;
    hmap_put(user_conns, uc->name, uc);
}

static bool
validate_nick(DCUserConn *uc, const char *nick)
{
    DCUserInfo *ui;

    ui = hmap_remove(pending_userinfo, nick);
    if (ui != NULL) {
    	if (ui->conn_count < DC_USER_MAX_CONN) {
	    uc->info = ui;
	    uc->info->refcount++;
	    uc->info->conn[uc->info->conn_count++] = uc;
	    hmap_remove(pending_userinfo, ui->nick);
	    user_info_free(ui);
	    update_user_connection_name(uc, "%s|", uc->info->nick);
	    return true;
	}
        warn(_("No more connections to user %s allowed.\n"), quotearg(nick));
	user_info_free(ui);
	return false;
    }

    ui = hmap_get(hub_users, nick);
    if (ui != NULL) {
    	if (ui->conn_count < DC_USER_MAX_CONN) {
    	    uc->info = ui;
	    uc->info->refcount++;
	    uc->info->conn[uc->info->conn_count++] = uc;
	    update_user_connection_name(uc, "%s|", uc->info->nick);
	    return true;
	}
        warn(_("No more connections to user %s allowed.\n"), quotearg(nick));
        return false;
    }

    return false;
}

static bool
validate_direction(DCUserConn *uc, DCTransferDirection dir)
{
    if (uc->dir != DC_DIR_UNKNOWN)
    	return false; /* Shouldn't happen, but better be sure */

    if (has_user_conn(uc->info, dir))
    	return false;

    uc->dir = dir;
    update_user_connection_name(uc, "%s|%s", uc->info->nick, dir == DC_DIR_SEND ? _("UL") : _("DL"));
    /* Don't check for a free slot here. We will do that later. */

    /* Make new connection if we want to download but couldn't. */
    if (dir == DC_DIR_SEND
	    && uc->info->conn_count < DC_USER_MAX_CONN
    	    && !has_user_conn(uc->info, DC_DIR_RECEIVE)
	    && uc->info->download_queue->cur > 0)
	hub_connect_user(uc->info);

    return true;
}

static void
display_transfer_ended_msg(bool upload, DCUserConn *uc, bool success, const char *extras_fmt, ...)
{
    char timebuf[LONGEST_ELAPSED_TIME+1];
    char ratebuf[LONGEST_HUMAN_READABLE+1];
    char sizebuf[LONGEST_HUMAN_READABLE+1];
    char *str1;
    char *str2;
    time_t now;
    uint64_t len;
    va_list args;

    len = uc->transfer_pos - uc->transfer_start;

    if (uc->transfer_time == (time_t) -1) {
        str2 = xstrdup("");
    } else {
	time(&now);
	if (now == (time_t) -1) {
	    warn(_("Cannot get current time - %s\n"), errstr);
            str2 = xstrdup("");
	} else {
	    str2 = xasprintf(_(" in %s (%s/s)"),
	                elapsed_time_to_string(now - uc->transfer_time, timebuf),
	                human_readable(
	                    len/MAX(now - uc->transfer_time, 1), ratebuf,
                            human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B,
                            1, 1));
	}
    }

    va_start(args, extras_fmt);
    str1 = xvasprintf(extras_fmt, args);
    va_end(args);

    flag_putf(upload ? DC_DF_UPLOAD : DC_DF_DOWNLOAD,
	_("%s: %s of %s %s%s. %s %s%s.\n"),
	quotearg_n(0, uc->info->nick),
	upload ? _("Upload") : _("Download"),
	quote_n(1, base_name(uc->transfer_file)),
	success ? _("succeeded") : _("failed"),
	str1,
        human_readable(len, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1),
	ngettext("transferred", "transferred", len),
	str2);

    free(str1);
    free(str2);
}

static void
handle_ended_upload(DCUserConn *uc, bool success, const char *reason)
{
    time_t now = (time_t)-1;
    uint64_t len = uc->transfer_pos - uc->transfer_start;
    uint64_t speed = len/MAX(now - uc->transfer_time, 1);

    time(&now);
    if (speed > max_speed) {
        prev_max_speed = max_speed;
        max_speed = speed;
    }

    bytes_sent += len;

    if (uc->occupied_slot || uc->occupied_minislot) {
        /* Only display "Upload failed" if "Starting upload" was
         * displayed previously.
         */
	    display_transfer_ended_msg(true, uc, success, " (%s)", reason);
        if (uc->occupied_slot) {
            used_ul_slots--;
            uc->occupied_slot = false;
        }
        if (uc->occupied_minislot) {
            used_mini_slots--;
            uc->occupied_minislot = false;
        }
    }
    free(uc->transfer_file);
    uc->transfer_file = NULL;
    uc->transfer_start = 0;
    uc->transfer_pos = 0;
    uc->transferring = false;
}

static void
handle_ended_download(DCUserConn *uc, bool success, const char *reason)
{
    bytes_received += uc->transfer_pos - uc->transfer_start;
    if (uc->occupied_slot) {
        used_dl_slots--;
        uc->occupied_slot = false;
    }
    /* If we removed from the queued item during download,
     * queue_pos will point outside the queue. */
    if (uc->queued_valid) {
        DCQueuedFile *queued;

        /*
        queued = uc->info->download_queue->buf[uc->queue_pos];
        uc->queue_pos++;
        */
        queued = ptrv_remove(uc->info->download_queue, uc->queue_pos);
        uc->queued_valid = false;
        if (success) {
            queued->status = DC_QS_DONE;
            if (queued->flag == DC_TF_LIST) {
                if (uc->local_file != NULL) {
                    ptrv_append(delete_files, xstrdup(uc->local_file));
                }
                if (browse_user != NULL && strcmp(browse_user->nick, uc->info->nick) == 0 && browse_list == NULL) {
                    add_parse_request(browse_list_parsed, uc->local_file, xstrdup(browse_user->nick));
                }
            } else {
                char *final_file = xstrndup(uc->local_file, strlen(uc->local_file)-5);
                if (safe_rename(uc->local_file, final_file) != 0) {
                    warn(_("%s: Cannot rename file to %s - %s\n"), quotearg_n(0, uc->local_file), quote_n(1, final_file), errstr);
                    reason = _("cannot rename file"); /* XXX: would like a more elaborate error message here perhaps? */
                    queued->status = DC_QS_ERROR;
                    success = false;
                }
                free(final_file);
            }
        } else {
            queued->status = DC_QS_ERROR;
        }
        display_transfer_ended_msg(false, uc, success, " (%s)", reason);
    } else {
        display_transfer_ended_msg(false, uc, success, " but unqueued (%s)", reason);
    }
    free(uc->transfer_file);
    free(uc->local_file);
    uc->transfer_file = NULL;
    uc->local_file = NULL;
    uc->transfer_start = 0;
    uc->transfer_pos = 0;
    uc->transferring = false;
}

DCUserConn *
user_connection_new(struct sockaddr_in *addr, int user_socket)
{
    DCUserConn *uc;
    int get_fd[2] = { -1, -1 };
    int put_fd[2] = { -1, -1 };
    pid_t pid;

    if (pipe(get_fd) != 0 || pipe(put_fd) != 0) {
        warn(_("Cannot create pipe pair - %s\n"), errstr);
        goto cleanup;
    }

    pid = fork();
    if (pid < 0) {
    	warn(_("Cannot create process - %s\n"), errstr);
    	goto cleanup;
    }
    if (pid == 0)
	user_main(put_fd, get_fd, addr, user_socket);

    if (close(get_fd[1]) != 0 || close(put_fd[0]) != 0)
        warn(_("Cannot close pipe - %s\n"), errstr);
    /* Non-blocking mode is not required, but there may be some latency otherwise. */
    if (!fd_set_nonblock_flag(get_fd[0], true) || !fd_set_nonblock_flag(put_fd[1], true))
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
    /* The user socket is only used by the newly created user process. */
    if (user_socket >= 0 && close(user_socket) < 0)
    	warn(_("Cannot close socket - %s\n"), errstr);

    uc = xmalloc(sizeof(DCUserConn));
    uc->pid = pid;
    uc->info = NULL;
    uc->occupied_slot = 0;
    uc->occupied_minislot = 0;
    uc->dir = DC_DIR_UNKNOWN;
    uc->transfer_file = NULL;
    uc->local_file = NULL;
    uc->transfer_start = 0;
    uc->transfer_pos = 0;
    uc->transfer_total = 0;
    uc->transfer_time = (time_t) -1;
    uc->transferring = false;
    uc->queue_pos = 0;
    uc->queued_valid = false;
    /* uc->we_connected = (user_socket < 0); */
    uc->get_mq = msgq_new(get_fd[0]);
    uc->put_mq = msgq_new(put_fd[1]);
    FD_SET(uc->get_mq->fd, &read_fds);
    if (user_conn_unknown_free->cur > 0) {
        uc->name = ptrv_remove_first(user_conn_unknown_free);
    } else {
        /* TRANSLATORS: This represents the connection name used when
         * the user name is not yet known. It must not contains '|',
         * because that's used to distinguish between 'unknown' and
         * (perhaps partially) identified connections.
         */
        uc->name = xasprintf(_("unknown%" PRIu32), user_conn_unknown_last+1);
        user_conn_unknown_last++;
    }
    hmap_put(user_conns, uc->name, uc);
    return uc;

  cleanup:
    if (get_fd[0] != -1)
        close(get_fd[0]);
    if (get_fd[1] != -1)
        close(get_fd[1]);
    if (put_fd[0] != -1)
        close(put_fd[0]);
    if (put_fd[1] != -1)
        close(put_fd[1]);
    return NULL;
}

void
user_disconnect(DCUserConn *uc)
{
    flag_putf(DC_DF_CONNECTIONS, _("Shutting down user connection process for %s.\n"), quote(uc->name)); /* XXX: move where? */

    hmap_remove(user_conns, uc->name);

    if (uc->occupied_slot) { /* could also check that uc->transfer_file != NULL */
        if (uc->dir == DC_DIR_SEND) {
            handle_ended_upload(uc, false, "connection terminated prematurely");
        } else { /* uc->dir == DC_DIR_RECV */
            handle_ended_download(uc, false, "connection terminated prematurely");
        }
    }

    if (uc->info != NULL) {
    	int c;
	for (c = 0; c < uc->info->conn_count; c++) {
	    if (uc->info->conn[c] == uc)
		break;
	}
	assert(c < uc->info->conn_count);
	uc->info->conn[c] = NULL;
    	uc->info->conn_count--;
	for (; c < uc->info->conn_count; c++)
	    uc->info->conn[c] = uc->info->conn[c+1];
	user_info_free(uc->info);
    }

    FD_CLR(uc->get_mq->fd, &read_fds);
    FD_CLR(uc->put_mq->fd, &write_fds);
    if (close(uc->get_mq->fd) != 0 || close(uc->put_mq->fd) != 0)
	warn(_("Cannot close pipe - %s\n"), errstr);
    msgq_free(uc->get_mq);
    uc->get_mq = NULL;
    msgq_free(uc->put_mq);
    uc->put_mq = NULL;
    if (strchr(uc->name, '|') == NULL)
        ptrv_append(user_conn_unknown_free, uc->name);
    else
        free(uc->name);
    free(uc);
}

char *
user_conn_status_to_string(DCUserConn *uc, time_t now)
{
    char *status;

    if (uc->transferring) {
    	int percent = (uc->transfer_pos * 100) / MAX(uc->transfer_total, 1);
	int rate = 0;

	if (now != (time_t) -1 && uc->transfer_time != (time_t) -1)
	    rate = (uc->transfer_pos-uc->transfer_start)/1024/MAX(now-uc->transfer_time, 1);
	status = xasprintf(
	        uc->dir == DC_DIR_RECEIVE
		    ? _("Downloading %3d%% (at %5d kb/s) %s")
		    : _("Uploading   %3d%% (at %5d kb/s) %s"),
                percent, rate, base_name(uc->transfer_file));
    } else {
	status = xasprintf(_("Idle"));
    }

    return status;
}

/* This function should be called when the user process it to be notified
 * about the termination (when it isn't terminating by itself).
 */
void
user_conn_cancel(DCUserConn *uc)
{
    user_disconnect(uc); /* MSG: "connection terminated by request" */
}

static void
user_request_fd_writable(DCUserConn *uc)
{
    int res;

    res = msgq_write(uc->put_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, true, _("user process %s"), quote(uc->name));
        user_disconnect(uc); /* MSG: socket error above */
        return;
    }
    if (!msgq_has_partial_msg(uc->put_mq))
        FD_CLR(uc->put_mq->fd, &write_fds);
}

static void
user_result_fd_readable(DCUserConn *uc)
{
    int res;

    res = msgq_read(uc->get_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, false, _("user process %s"), quote(uc->name));
        user_disconnect(uc); /* MSG: socket error above */
        return;
    }
    while (msgq_has_complete_msg(uc->get_mq)) {
        int id;

        msgq_peek(uc->get_mq, MSGQ_INT, &id, MSGQ_END);
        switch (id) {
        case DC_MSG_SCREEN_PUT: {
            char *msg;
            uint32_t flag;

            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_INT32, &flag, MSGQ_STR, &msg, MSGQ_END);
            flag_putf(flag, _("User %s: %s"), quotearg(uc->name), msg); /* msg already quoted */
            free(msg);
            break;
        }
        case DC_MSG_WANT_DOWNLOAD: {
    	    bool reply;

    	    msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_END);
    	    reply = !has_user_conn(uc->info, DC_DIR_RECEIVE)
    	            && (uc->queue_pos < uc->info->download_queue->cur);
            msgq_put(uc->put_mq, MSGQ_BOOL, reply, MSGQ_END);
            FD_SET(uc->put_mq->fd, &write_fds);
            break;
        }
        case DC_MSG_VALIDATE_DIR: {
    	    DCTransferDirection dir;
    	    bool reply;

    	    msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_INT, &dir, MSGQ_END);
    	    reply = validate_direction(uc, dir);
    	    msgq_put(uc->put_mq, MSGQ_BOOL, reply, MSGQ_END);
    	    FD_SET(uc->put_mq->fd, &write_fds);
    	    break;
        }
        case DC_MSG_VALIDATE_NICK: {
    	    char *nick;
    	    bool reply;
    
    	    msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_STR, &nick, MSGQ_END);
    	    reply = validate_nick(uc, nick);
    	    free(nick);
            if (uc->info != NULL) {
	        /* The user can only be DC_ACTIVE_SENT_PASSIVE if we are passive.
	         * So if we managed to connect to them even though we were passive,
	         * they must be active.
	         */
	        if (uc->info->active_state == DC_ACTIVE_SENT_PASSIVE)
		    uc->info->active_state = DC_ACTIVE_KNOWN_ACTIVE;
                /* The user can only be DC_ACTIVE_SENT_ACTIVE if we are active.
                 * We must set to DC_ACTIVE_UNKNOWN because otherwise
                 * hub.c:hub_connect_user might say "ConnectToMe already sent to
                 * user" next time we're trying to connect to them.
                 */
                 if (uc->info->active_state == DC_ACTIVE_SENT_ACTIVE)
                     uc->info->active_state = DC_ACTIVE_UNKNOWN;
            }
            msgq_put(uc->put_mq, MSGQ_BOOL, reply, MSGQ_END);
            FD_SET(uc->put_mq->fd, &write_fds);
            break;
        }
        case DC_MSG_GET_MY_NICK:
            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_END);
            msgq_put(uc->put_mq, MSGQ_STR, my_nick, MSGQ_END);
            FD_SET(uc->put_mq->fd, &write_fds);
            break;
        case DC_MSG_TRANSFER_STATUS:
    	    msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_INT64, &uc->transfer_pos, MSGQ_END);
    	    break;
        case DC_MSG_TRANSFER_START: {
            char *share_filename, *local_filename;
    	    msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_STR, &local_filename, MSGQ_STR, &share_filename, MSGQ_INT64, &uc->transfer_start, MSGQ_INT64, &uc->transfer_total, MSGQ_END);
    	    if (uc->transfer_start != 0) {
                flag_putf(DC_DF_CONNECTIONS, _("%s: Starting %s of %s (%" PRIu64 " of %" PRIu64 " %s).\n"),
                          quotearg_n(0, uc->info->nick),
                          uc->dir == DC_DIR_SEND ? _("upload") : _("download"),
                          quote_n(1, base_name(share_filename)),
                          uc->transfer_total - uc->transfer_start,
                          uc->transfer_total,
                          ngettext("byte", "bytes", uc->transfer_total));
            } else {
                flag_putf(DC_DF_CONNECTIONS, _("%s: Starting %s of %s (%" PRIu64 " %s).\n"),
                          quotearg_n(0, uc->info->nick),
                          uc->dir == DC_DIR_SEND ? _("upload") : _("download"),
                          quote_n(1, base_name(share_filename)),
                          uc->transfer_total,
                          ngettext("byte", "bytes", uc->transfer_total));
            }
            free(uc->transfer_file);
            uc->transfer_file = share_filename;
            free(uc->local_file);
            uc->local_file = local_filename;
            uc->transferring = true;
            uc->transfer_pos = uc->transfer_start;
            uc->transfer_time = time(NULL);
            if (uc->transfer_time == (time_t) -1)
                warn(_("Cannot get current time - %s\n"), errstr);
            break;
        }
        case DC_MSG_CHECK_DOWNLOAD:
            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_END);
            for (; uc->queue_pos < uc->info->download_queue->cur; uc->queue_pos++) {
                DCQueuedFile *queued;
                char *local_file;

                queued = uc->info->download_queue->buf[uc->queue_pos];
                if (queued->status != DC_QS_DONE) {
                    local_file = resolve_download_file(uc->info, queued);
                    uc->queued_valid = true;
                    uc->transfer_file = xstrdup(queued->filename);
                    uc->local_file = xstrdup(local_file);
                    uc->occupied_slot = true;
                    queued->status = DC_QS_PROCESSING;
                    used_dl_slots++;
                    msgq_put(uc->put_mq, MSGQ_STR, local_file, MSGQ_STR, uc->transfer_file, MSGQ_INT64, queued->length, MSGQ_INT, queued->flag, MSGQ_END);
                    FD_SET(uc->put_mq->fd, &write_fds);
                    free(local_file);
                    return;
                }
            }
            msgq_put(uc->put_mq, MSGQ_STR, NULL, MSGQ_STR, NULL, MSGQ_INT64, (uint64_t) 0, MSGQ_INT, DC_TF_NORMAL, MSGQ_END);
            FD_SET(uc->put_mq->fd, &write_fds);
            break;
        case DC_MSG_DOWNLOAD_ENDED: {
            bool success;
            char *reason;

            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_BOOL, &success, MSGQ_STR, &reason, MSGQ_END);
            handle_ended_download(uc, success, reason);
            free(reason);
            break;
        }
        case DC_MSG_CHECK_UPLOAD: {
            char *remote_file;
            char *local_file;
            int type;
            uint64_t size = 0;
            DCTransferFlag flag = DC_TF_NORMAL;
            bool permit_transfer = false;

            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_INT, &type, MSGQ_STR, &remote_file, MSGQ_END);
            local_file = resolve_upload_file(uc->info, type, remote_file, &flag, &size);
            free(remote_file);
            uc->transfer_file = NULL;
            if (local_file != NULL) {
                if (flag == DC_TF_LIST || (flag == DC_TF_NORMAL && size <= minislot_size)) {
                    if (used_mini_slots < minislot_count) {
                        used_mini_slots ++;
                        uc->occupied_minislot = true;
                        permit_transfer = true;
                    } else if (used_ul_slots < my_ul_slots || uc->info->slot_granted) {
                        used_ul_slots ++;
                        uc->occupied_slot = true;
                        permit_transfer = true;
                    }
                } else if (flag == DC_TF_NORMAL && size > minislot_size) {
                    if (used_ul_slots < my_ul_slots || uc->info->slot_granted) {
                        used_ul_slots ++;
                        uc->occupied_slot = true;
                        permit_transfer = true;
                    }
                }
                if (permit_transfer) {
                    uc->transfer_file = local_file;
                } else {
                    free(local_file);
                    local_file = NULL;
                }
            } else {
                permit_transfer = true;
            }
            msgq_put(uc->put_mq, MSGQ_BOOL, permit_transfer, MSGQ_STR, local_file, MSGQ_END);
            FD_SET(uc->put_mq->fd, &write_fds);
            break;
        }
        case DC_MSG_UPLOAD_ENDED: {
            bool success;
            char *reason;

            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_BOOL, &success, MSGQ_STR, &reason, MSGQ_END);
            handle_ended_upload(uc, success, reason);
            free(reason);
            break;
        }
        case DC_MSG_TERMINATING:
            /* The TERMINATING message will be sent from users when they're about to
             * shut down properly.
	     * XXX: This message may contain the reason it was terminated in the future.
	     */
            msgq_get(uc->get_mq, MSGQ_INT, &id, MSGQ_END);
            user_disconnect(uc); /* MSG: reason from DC_MSG_TERMINATING */
            return; /* not break! this is the last message. */
        default:
            warn(_("Received unknown message %d from user process, shutting down process.\n"), id);
            user_disconnect(uc); /* MSG: local communication error? */
            return; /* not break! this is the last message. */
        }
    }
}

void
transfer_completion_generator(DCCompletionInfo *ci)
{
    HMapIterator it;

    /* XXX: perhaps use a tmap for transfers as well? to speed up this */
    hmap_iterator(user_conns, &it);
    while (it.has_next(&it)) {
    	DCUserConn *uc = it.next(&it);
	if (strleftcmp(ci->word, uc->name) == 0)
	    ptrv_append(ci->results, new_completion_entry(uc->name, NULL));
    }
    ptrv_sort(ci->results, completion_entry_display_compare);
}

/* This is the actual signal handler, registered in main_init with
 * sigaction.
 */
static void
signal_received(int signal)
{
    uint8_t signal_char;

    /* This check is to stop a small risk: The signal may be received in a
     * newly created child process that hasn't had its signal handler
     * changed yet.
     */
    if (getpid() != main_process_id)
        return;

    signal_char = signal;
    if (write(signal_pipe[1], &signal, sizeof(uint8_t)) < sizeof(uint8_t)) {
	/* Die only if the signal is fatal.
         * If SIGCHLD is not delivered, we would be stuck in a
         * state where the user cannot enter any commands.
         */
	if (signal == SIGTERM || signal == SIGINT || signal == SIGCHLD)
	    die(_("Cannot write to signal pipe - %s\n"), errstr); /* die OK */
        warn(_("Cannot write to signal pipe - %s\n"), errstr);
    }
}

static void
read_signal_input(void)
{
    uint8_t signal;

    /* This read is atomic since sizeof(int) < PIPE_BUF!
     * It also doesn't block since all data is already
     * available (otherwise select wouldn't tell us there
     * was data).
     */
    if (read(signal_pipe[0], &signal, sizeof(uint8_t)) < 0) {
	warn(_("Cannot read from signal pipe - %s\n"), errstr);
	running = false;
	return;
    }

    if (signal == SIGTERM) {
        warn(_("Received TERM signal, shutting down.\n"));
        running = false;
    }
    else if (signal == SIGINT) {
        screen_erase_and_new_line();
    }
    else if (signal == SIGCHLD) {
        pid_t child;
        int status;

        while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
            char *name;

            if (child == shell_child) {
                screen_wakeup(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
                shell_child = -1;
                name = _("Shell process");
            } else if (child == lookup_child) {
                name = _("Lookup process");
                running = false;
            } else if (child == parse_child) {
                name = _("Parse process");
                running = false;
            } else if (child == update_child) {
                name = _("FileList Update process");
                running = false;
            } else {
                /* Assume it was a child process */
                name = _("User process");
            }
            if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                warn(_("%s exited with return code %d.\n"), name, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                char signame[SIG2STR_MAX];

                if (sig2str(WTERMSIG(status), signame) < 0)
                    sprintf(signame, "%d", WTERMSIG(status));
                warn(_("%s terminated by signal %s.\n"), name, signame);
            }
        }
        if (child < 0 && errno != ECHILD)
            warn(_("Cannot wait for processes - %s\n"), errstr);
    }
    else if (signal == SIGUSR1) {
        /* Not implemented yet, do nothing for now. */
    }
}

/* Load and run a script.
 */
void
run_script(const char *filename, bool allow_missing)
{
    FILE *file;
    char *line = NULL;
    size_t line_len = 0;

    file = fopen(filename, "r");
    if (file == NULL) {
    	if (!allow_missing || errno != ENOENT)
    	    warn(_("%s: Cannot open file - %s\n"), quotearg(filename), errstr);
    	return;
    }
    while (getline(&line, &line_len, file) > 0) {
    	int c;
	for (c = strlen(line)-1; c > 0 && (line[c] == '\n' || line[c] == '\r'); c--)
	    line[c] = '\0';
    	command_execute(line);
    }
    free(line);
    if (ferror(file)) {
    	warn(_("%s: Cannot read from file - %s\n"), quotearg(filename), errstr);
	if (fclose(file) < 0)
	    warn(_("%s: Cannot close file - %s\n"), quotearg(filename), errstr);
	return;
    }
    if (fclose(file) < 0)
	warn(_("%s: Cannot close file - %s\n"), quotearg(filename), errstr);
}

bool
add_share_dir(const char *dir)
{
    return true;
}

void
add_search_result(struct sockaddr_in *addr, char *results, uint32_t resultlen)
{
    DCUDPMessage *msg;
    /*
    int res = 0;
    */

    /*
    if (search_udpmsg_out->cur == 0) {
    	res = sendto(search_socket, results, resultlen, 0, (struct sockaddr *) addr, sizeof(struct sockaddr_in));
        if (res == 0 || (res < 0 && errno != EAGAIN)) {
	        warn_socket_error(res, true, _("user (search result)"));
	        return;
	    }
	    if (res == resultlen)
	        return;
    }
    */

    msg = xmalloc(sizeof(DCUDPMessage)+resultlen);
    msg->addr = *addr;
    msg->len = resultlen;
    memcpy(msg->data, results, resultlen);

    ptrv_append(search_udpmsg_out, msg);
    FD_SET(search_socket, &write_fds);
}

static void
search_input_available(void)
{
    struct sockaddr_in addr;
    socklen_t addrlen;
    int res;

    /* UDP socket reads are atomic! We are also non-blocking because select told us
     * there was data available.
     *
     * We don't really keep the received address, so we don't need recvfrom here.
     */
    addrlen = sizeof(addr);
    res = byteq_recvfrom(search_recvq, search_socket, 0, (struct sockaddr *) &addr, &addrlen);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
    	warn_socket_error(res, false, _("user (search result)"));
        return;
    }

    dump_command(_("<=="), search_recvq->buf, search_recvq->cur);
    search_recvq->buf[search_recvq->cur-1] = '\0'; /* not strictly necessary */
    handle_search_result(search_recvq->buf, search_recvq->cur);
    byteq_clear(search_recvq);
}

static void
search_now_writable(void)
{
    while (search_udpmsg_out->cur > 0) {
        DCUDPMessage *msg = search_udpmsg_out->buf[0];
        int res;

    	/* <Erwin> I don't think you can send off half a UDP packet, no.
    	 * Therefore we don't attempt to use non-blocking I/O on search_socket.
	    */
    	dump_command(_("==>"), msg->data, msg->len);

        res = sendto(search_socket, msg->data, msg->len, 0, (struct sockaddr *) &msg->addr, sizeof(struct sockaddr_in));
        if (res == 0 || (res < 0 && errno != EAGAIN))
	        warn_socket_error(res, true, _("user (search result)"));
        if (res < 0 && errno == EAGAIN)
            break;

        ptrv_remove_first(search_udpmsg_out);
        free(msg);
    }

    FD_CLR(search_socket, &write_fds);
}

static void
disable_active(void)
{
    if (listen_socket >= 0) {
	if (close(listen_socket) < 0)
	    warn(_("Cannot close socket - %s\n"), errstr); /* XXX: (user connections server/listen socket) */
	FD_CLR(listen_socket, &read_fds);
	listen_socket = -1;
    }
}

static bool
enable_search(void)
{
    struct sockaddr_in addr;
    socklen_t addr_len;
    int val;

    search_socket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (search_socket < 0) {
	warn(_("Cannot create socket - %s\n"), errstr);
	return false;
    }

    if (!fd_set_nonblock_flag(search_socket, true)) {
    	warn(_("Cannot set non-blocking flag - %s\n"), errstr);
	return false;
    }

    val = 1;
    if (setsockopt(search_socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
	warn(_("Cannot enable address reusing - %s\n"), errstr);

    if (listen_port != 0) {
    	addr_len = sizeof(addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons(listen_port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(search_socket, (struct sockaddr *) &addr, addr_len) < 0) {
	    warn(_("Cannot bind to address - %s\n"), errstr);
	    return false;
	}
    }

    FD_SET(search_socket, &read_fds);

    return true;
}

static bool
enable_active(uint16_t port)
{
    struct sockaddr_in addr;
    socklen_t addr_len;
    int val;

    /* Create sockets. */
    listen_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket < 0) {
        warn(_("Cannot create socket - %s\n"), errstr);
        disable_active();
        return false;
    }

    /* Set non-blocking */
    if (!fd_set_nonblock_flag(listen_socket, true)) {
    	warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        disable_active();
        return false;
    }

    /* Enable listen address reusing */
    val = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
        warn(_("Cannot enable address reusing - %s\n"), errstr);

    /* Bind to address. If not binding, port is selected randomly. */
    addr_len = sizeof(addr);
    if (port != 0) {
    	addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (force_listen_addr.s_addr != INADDR_NONE) {
            addr.sin_addr.s_addr = force_listen_addr.s_addr;
        } else {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        }
    	if (bind(listen_socket, (struct sockaddr *) &addr, addr_len) < 0) {
            warn(_("Cannot bind to address - %s\n"), errstr);
            disable_active();
            return false;
        }
    }

    /* Start listening for connections. */
    if (listen(listen_socket, LISTEN_QUEUE_CONNS) < 0) {
        warn(_("Cannot listen - %s\n"), errstr);
        disable_active();
        return false;
    }

    /* Find port we are listening on. */
    if (getsockname(listen_socket, (struct sockaddr *) &addr, &addr_len) < 0) {
    	warn(_("Cannot get socket address - %s\n"), errstr);
        disable_active();
        return false;
    }

    screen_putf(_("Listening on %s.\n"), sockaddr_in_str(&addr));
    listen_port = ntohs(addr.sin_port);
    FD_SET(listen_socket, &read_fds);
    return true;
}

bool
set_active(bool newactive, uint16_t port)
{
    if (newactive) {
        disable_active();
    	if (!enable_active(port))
	        return false;
    } else {
    	disable_active();
        listen_port = port;
    }
    /* Start of disable_search. */
    if (search_socket >= 0) {
	    if (close(search_socket) < 0)
            warn(_("Cannot close socket - %s\n"), errstr);
	    search_socket = -1;
    }
    /* End of disable_search. */
    enable_search();
    is_active = newactive;
    return true;
}

static void
handle_listen_connection(void)
{
    struct sockaddr_in addr;
    socklen_t addr_len;
    int socket;

    addr_len = sizeof(addr);
    socket = accept(listen_socket, (struct sockaddr *) &addr, &addr_len);
    if (socket < 0) {
	warn(_("Cannot accept user connection - %s\n"), errstr);
	return;
    }

    flag_putf(DC_DF_CONNECTIONS, _("User from %s connected.\n"), sockaddr_in_str(&addr));
    user_connection_new(&addr, socket);
}

int
main (int argc, char **argv)
{
    uint32_t c;
    char *config_file;
    bool custom_config;
    char *tmpdir;
    struct sigaction sigact;

    set_quoting_style(NULL, escape_quoting_style);
    
    if (setlocale(LC_ALL, "") == NULL)
       warn(_("%s: Cannot set locale: %s\n"), argv[0], errstr);
#ifdef ENABLE_NLS
    if (bindtextdomain(PACKAGE, LOCALEDIR) == NULL)
       warn(_("%s: Cannot bind message domain: %s\n"), argv[0], errstr);
    if (textdomain(PACKAGE) == NULL)
        warn(_("%s: Cannot set message domain: %s\n"), argv[0], errstr);
#endif

    custom_config = false;
    get_package_file("config", &config_file);

    while (true) {
	    c = getopt_long(argc, argv, short_opts, long_opts, NULL);
	    if (c == -1)
	        break;

	    switch (c) {
	    case 'c': /* --config */
	        custom_config = true;
	        free(config_file);
	        config_file = xstrdup(optarg);
	        break;
	    case 'n': /* --no-config */
	        free(config_file);
	        config_file = NULL;
	        break;
	    case HELP_OPT: /* --help */
	        printf(_("Usage: %s [OPTION]...\n"), quotearg(argv[0]));
	        puts(_("Start microdc, a command-line based Direct Connect client.\n"));
	        printf(_("  -n, --no-config  do not read config file on startup\n"));
	        printf(_("      --help       display this help and exit\n"));
	        printf(_("      --version    output version information and exit\n"));
	        printf(_("\nReport bugs to <%s>.\n"), PACKAGE_BUGREPORT);
	        exit(EXIT_SUCCESS);
	    case VERSION_OPT: /* --version */
	        version_etc(stdout, NULL, PACKAGE, VERSION, /*"Oskar Liljeblad",*/ "Vladimir Chugunov", NULL);
	        exit(EXIT_SUCCESS);
        default:
            exit(EXIT_FAILURE);
	    }
    }
    
    if (pipe(signal_pipe) < 0) {
    	warn(_("Cannot create pipe pair - %s\n"), errstr);
	    goto cleanup;
    }

    main_process_id = getpid();
    sigact.sa_handler = signal_received;
    if (sigemptyset(&sigact.sa_mask) < 0) {
        warn(_("Cannot empty signal set - %s\n"), errstr);
	    goto cleanup;
    }
    sigact.sa_flags = SA_RESTART;
#ifdef HAVE_STRUCT_SIGACTION_SA_RESTORER
    sigact.sa_restorer = NULL;
#endif
    /* Note: every signal registered with a non-ignore action here must
     * also be registered in user.c, either with an action or as ignored.
     */
    if (sigaction(SIGINT,  &sigact, NULL) < 0 ||
        sigaction(SIGTERM, &sigact, NULL) < 0 ||
        sigaction(SIGUSR1, &sigact, NULL) < 0 ||
        sigaction(SIGCHLD, &sigact, NULL) < 0) {
        warn(_("Cannot register signal handler - %s\n"), errstr);
	    goto cleanup;
    }
    sigact.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sigact, NULL) < 0) {
        warn(_("Cannot register signal handler - %s\n"), errstr);
	    goto cleanup;
    }

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_SET(signal_pipe[0], &read_fds);
    /*FD_SET(STDIN_FILENO, &read_fds);*/

    hub_recvq = byteq_new(128);
    hub_sendq = byteq_new(128);
    user_conns = hmap_new();
    hub_users = hmap_new();
    pending_userinfo = hmap_new();

    set_main_charset("");
    set_hub_charset("");
    set_fs_charset("");

    user_conn_unknown_free = ptrv_new();
    delete_files = ptrv_new();
    delete_dirs = ptrv_new();
    search_udpmsg_out = ptrv_new();
    our_searches = ptrv_new();
    search_recvq = byteq_new(8192); // same size as DC++
    my_nick = xstrdup(PACKAGE);
    my_description = xstrdup("");
    my_email = xstrdup("");
    my_speed = xstrdup("56Kbps");
    my_tag = xasprintf("%s V:%s", PACKAGE, VERSION);
    download_dir = xstrdup(".");
    tmpdir = tempdir();
    if (tmpdir == NULL) {
	    warn(_("Cannot find directory for temporary files - %s\n"), errstr);
	    goto cleanup;
    }
    {
        char *filename = xasprintf("%s.%d", PACKAGE, getpid());
        listing_dir = catfiles(tmpdir, filename);
        free(filename);
    }
    ptrv_append(delete_dirs, xstrdup(listing_dir));
    is_active = false;
    listen_port = 0;
    if (!local_file_list_update_init())
        goto cleanup;
    if (!set_active(false, listen_port))
    	goto cleanup;
    if (!enable_search())
    	goto cleanup;
    my_ul_slots = 3;

    if (!lookup_init())
        goto cleanup;
    if (!file_list_parse_init())
        goto cleanup;
    command_init();

    if (!local_file_list_init()) {
        goto cleanup;
    }

    if (config_file != NULL) {
        run_script(config_file, !custom_config);
        free(config_file);
        config_file = NULL;
    }

    screen_prepare();

    while (running) {
    	fd_set res_read_fds;
	    fd_set res_write_fds;
    	int res;
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        screen_redisplay_prompt();

	    res_read_fds = read_fds;
	    res_write_fds = write_fds;
    	res = TEMP_FAILURE_RETRY(select(FD_SETSIZE, &res_read_fds, &res_write_fds, NULL, &tv));
	    if (res < 0) {
	        warn(_("Cannot select - %s\n"), errstr);
	        break;
	    }

    	if (running && FD_ISSET(signal_pipe[0], &res_read_fds))
	        read_signal_input();
    	if (running && FD_ISSET(STDIN_FILENO, &res_read_fds))
    	    screen_read_input();
    	if (running && listen_socket >= 0 && FD_ISSET(listen_socket, &res_read_fds))
	        handle_listen_connection();
	    if (running && hub_socket >= 0 && FD_ISSET(hub_socket, &res_read_fds))
	        hub_input_available();
	    if (running && hub_socket >= 0 && FD_ISSET(hub_socket, &res_write_fds))
	        hub_now_writable();
        if (running)
            check_hub_activity();
	    if (running && search_socket >= 0 && FD_ISSET(search_socket, &res_read_fds))
    	    search_input_available();
	    if (running && search_socket >= 0 && FD_ISSET(search_socket, &res_write_fds))
	        search_now_writable();
        if (running && FD_ISSET(lookup_request_mq->fd, &res_write_fds))
            lookup_request_fd_writable();
        if (running && FD_ISSET(lookup_result_mq->fd, &res_read_fds))
            lookup_result_fd_readable();
        if (running && FD_ISSET(parse_request_mq->fd, &res_write_fds))
            parse_request_fd_writable();
        if (running && FD_ISSET(parse_result_mq->fd, &res_read_fds))
            parse_result_fd_readable();
        if (running && FD_ISSET(update_request_mq->fd, &res_write_fds))
            update_request_fd_writable();
        if (running && FD_ISSET(update_result_mq->fd, &res_read_fds))
            update_result_fd_readable();

	    if (running) {
	        HMapIterator it;

	        hmap_iterator(user_conns, &it);
	        while (running && it.has_next(&it)) {
	    	    DCUserConn *uc = it.next(&it);
	    	    if (uc->put_mq != NULL && FD_ISSET(uc->put_mq->fd, &res_write_fds))
	    	        user_request_fd_writable(uc);
                    if (uc->get_mq != NULL && FD_ISSET(uc->get_mq->fd, &res_read_fds))
                        user_result_fd_readable(uc);
	        }
	    }
    }

cleanup:

    hub_disconnect();
    screen_finish();
    command_finish();
    local_file_list_update_finish();
    file_list_parse_finish();
    lookup_finish();

    byteq_free(hub_recvq);
    byteq_free(hub_sendq);
    hmap_free(hub_users); /* Emptied by hub_disconnect */
    hmap_free(pending_userinfo); /* Emptied by hub_disconnect */

    byteq_free(search_recvq);

    ptrv_foreach(user_conn_unknown_free, free);
    ptrv_free(user_conn_unknown_free);

    ptrv_foreach(search_udpmsg_out, free);
    ptrv_free(search_udpmsg_out);

    ptrv_foreach(our_searches, (PtrVForeachCallback) free_search_request);
    ptrv_free(our_searches);

    hmap_foreach_value(user_conns, user_conn_cancel);
    /* XXX: follow up and wait for user connections to die? */
    hmap_free(user_conns);

    if (our_filelist != NULL)
    	filelist_free(our_filelist);

    set_main_charset(NULL);
    set_hub_charset(NULL);
    set_fs_charset(NULL);

    free(hub_name);
    free(my_nick);
    free(my_description);
    free(my_email);
    free(my_speed);
    free(my_tag);
    free(download_dir);
    free(listing_dir);

    if (delete_files != NULL) {
	    for (c = 0; c < delete_files->cur; c++) {
    	        char *filename = delete_files->buf[c];
    	        struct stat st;

	        if (stat(filename, &st) < 0) {
		        if (errno != ENOENT)
	    	        warn(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
		        free(filename);
		        continue;
	        }
    	    if (unlink(filename) < 0)
		        warn(_("%s: Cannot remove file - %s\n"), quotearg(filename), errstr);
	        free(filename);
	    }
	    ptrv_free(delete_files);
    }

    if (delete_dirs != NULL) {
	    for (c = 0; c < delete_dirs->cur; c++) {
    	        char *filename = delete_dirs->buf[c];
    	        struct stat st;

	        if (stat(filename, &st) < 0) {
		        if (errno != ENOENT)
	    	        warn(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
		        free(filename);
		        continue;
	        }
    	    if (rmdir(filename) < 0)
		        warn(_("%s: Cannot remove file - %s\n"), quotearg(filename), errstr);
	        free(filename);
	    }
	    ptrv_free(delete_dirs);
    }

    if (search_socket >= 0 && close(search_socket) < 0)
    	warn(_("Cannot close search results socket - %s\n"), errstr);
    if (listen_socket >= 0 && close(listen_socket) < 0)
    	warn(_("Cannot close user connections socket - %s\n"), errstr);
    if (signal_pipe[0] >= 0 && close(signal_pipe[0]) < 0)
    	warn(_("Cannot close signal pipe - %s\n"), errstr);
    if (signal_pipe[1] >= 0 && close(signal_pipe[1]) < 0)
    	warn(_("Cannot close signal pipe - %s\n"), errstr);

    free(config_file);

    exit(EXIT_SUCCESS);
}
