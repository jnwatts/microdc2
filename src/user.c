/* user.c - User communication (in separate process)
 *
 * Copyright (C) 2004, 2005 Oskar Liljeblad
 * Copyright (C) 2006 Alexey Illarionov <littlesavage@rambler.ru>
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

#include <assert.h>		/* ? */
#include <unistd.h>		/* POSIX */
#include <sys/types.h>		/* ? */
#include <sys/stat.h>		/* ? */
#include <sys/wait.h>		/* ? */
#include <fcntl.h>		/* ? */
#include <signal.h>		/* ? */
#include <sys/socket.h>		/* ? */
#include <sys/types.h>		/* ? */
#include <netinet/in.h>		/* ? */
#include <arpa/inet.h>		/* ? */
#include <inttypes.h>		/* ? */
#include "dirname.h"		/* Gnulib */
#include "full-write.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "memmem.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#include "dirname.h"		/* Gnulib */

#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/error.h"
#include "common/intutil.h"
#include "common/tempfailure.h"
#include "iconvme.h"
#include "tth/tigertree.h"
#include "microdc.h"

#define LOCK_STRING "EXTENDEDPROTOCOLABCABCABCABCABCABCA"
#define LOCK_STRING_LEN (sizeof(LOCK_STRING)-1)
#define LOCK_PK_STRING "MICRODCABCABCABCABCAB"

#define DEFAULT_RECVQ_SIZE (64*1024)
#define DEFAULT_SENDQ_SIZE (64*1024)

#define USER_CONN_IDLE_TIMEOUT (3*60)

typedef struct _DCUserConnLocal DCUserConnLocal;

struct _DCUserConnLocal {
    MsgQ *get_mq;/*=NULL*/
    MsgQ *put_mq;/*=NULL*/
    int signal_pipe[2]/* = { -1, -1 }*/;
    char *user_nick/* = NULL*/;
    uint32_t user_recvq_last/* = 0*/;
    ByteQ *user_recvq;
    ByteQ *user_sendq;
    int user_socket/* = -1*/;
    uint64_t data_size/* = 0*/;		/* only useful when receiving files */
    uint16_t dir_rand;
    bool we_connected;
    DCTransferDirection our_dir;
    DCUserState user_state/* = DC_USER_CONNECT*/;
    bool user_running/* = true*/;
    fd_set user_read_fds;
    fd_set user_write_fds;

    PtrV*  supports;

    //union {
      //struct {
        char *share_file;	/* complete filename in shared file namespace. */
        char *local_file;	/* complete filename in local physical file namespace. */
        int transfer_fd;	/* file descriptor for opened local_file */
        uint64_t file_pos;	/* how much of local_file that has been written */
        uint64_t final_pos;	/* how much of local_file shuld be written */
        uint64_t file_size;	/* the final size of local_file */
        uint64_t transfer_pos;	/* how much that has been read (but not necessarily written yet) */
        bool local_exists;	/* does local_file exist already? */
      //} dl;
      //struct {
        //char *share_file;	/* complete filename in shared file namespace. */
        //char *local_file;	/* complete filename in local physical file namespace. */
        //int fd;			/* file descriptor for opened local_file */
        //uint64_t file_pos;	/* how much of local_file that has been read */
        //uint64_t file_size;	/* the size of local_file */
        //uint64_t transfer_pos;	/* how much of what's been read that has been sent to remote */
      //} ul;
    //};
};

static DCUserConnLocal *cur_ucl;

static void upload_file(DCUserConnLocal *ucl);

/* NOTE: All the code below assumes that main never disconnects a user
 * (close on main_socket) on purpose. The user must do this first if
 * things happen gracefully.
 */

/* This function should be called when the communication to the
 * main process becomes unusable. It will terminate the user
 * process. If it appears that the communication was closed by
 * the remote end, no error is printed.
 */
static void
fatal_error(DCUserConnLocal *ucl, int res, bool writing) /* XXX: rename communication_error or something */
{
    warn_writer = default_warn_writer;
    /* These tests are for the case when the main process closed
     * the connection first. The first test is when reading (and
     * the result is EOF because of closed pipe), and the second
     * set of tests are used when writing. The latter case is
     * probably rare - the child process would have to be
     * 'cancelled' in main when the child was just about to call
     * msgq_put.
     */
    if (res != 0 && !(writing && res < 0 && errno == EPIPE))
        warn_socket_error(res, writing, _("main process"));
    ucl->user_running = false;
}

/* This function should be called when the user process decided to
 * terminate itself (but the communication to the main process
 * is still usable).
 */
static void
terminate_process(DCUserConnLocal *ucl)
{
    int res;

    ucl->user_running = false;
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_TERMINATING, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return;
    }
}

static bool
check_state(DCUserConnLocal *ucl, char *buf, DCUserState state)
{
    if (ucl->user_state != state) {
	warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
	terminate_process(ucl); /* protocol error */
	return false;
    }
    return true;
}

static bool
send_user_status(DCUserConnLocal *ucl, uint64_t pos)
{
    int res;

    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_TRANSFER_STATUS, MSGQ_INT64, pos, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return false;
    }
    return true;
}

static void
user_screen_writer(DCDisplayFlag flag, const char *format, va_list args)
{
    DCUserConnLocal *ucl = cur_ucl; /* XXX */
    char *msg;
    int res;

    msg = xvasprintf(format, args);
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_SCREEN_PUT, MSGQ_INT32, (int32_t) flag, MSGQ_STR, msg, MSGQ_END);
    free(msg);
    if (res <= 0) /* XXX: should we maybe print this message manually? */
        fatal_error(ucl, res, true); /* delay handling until later. */
}

/* Put some data onto the connection, printf style.
 */
bool
user_putf(DCUserConnLocal *ucl, const char *format, ...)
{
    va_list args;
    uint32_t oldcur;
    int res;

    oldcur = ucl->user_sendq->cur;
    va_start(args, format);
    res = byteq_vappendf(ucl->user_sendq, format, args);
    va_end(args);

    /* byteq_vappendf cannot fail. */
    /*if (res < 0) {
    	warn(_("Cannot append to user send queue - %s\n"), errstr);
	terminate_process(ucl);
	return false;
    }*/

    if (ucl->data_size/*DL*/ == 0)
        dump_command(_("-->"), ucl->user_sendq->buf+oldcur, ucl->user_sendq->cur-oldcur);

    res = byteq_write(ucl->user_sendq, ucl->user_socket);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
	warn_socket_error(res, true, _("user"));
	terminate_process(ucl); /* MSG: socket error above */
	return false;
    }

    if (oldcur == 0 && ucl->user_sendq->cur > 0)
    	FD_SET(ucl->user_socket, &ucl->user_write_fds);

    return true;
}

static bool
wants_to_download(DCUserConnLocal *ucl, bool *reply)
{
    int res;

    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_WANT_DOWNLOAD, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return false;
    }
    res = msgq_get_sync(ucl->get_mq, MSGQ_BOOL, reply, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return false;
    }
    return true;
}

static bool
direction_validate(DCUserConnLocal *ucl, DCTransferDirection dir)
{
    bool reply;
    int res;

    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_VALIDATE_DIR, MSGQ_INT, dir, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return false;
    }
    res = msgq_get_sync(ucl->get_mq, MSGQ_BOOL, &reply, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return false;
    }
    if (!reply) {
	warn(_("Too many connections to user, or no free slots.\n"));
	terminate_process(ucl); /* MSG: msg above */
	return false;
    }
    return true;
}

static bool
get_our_nick(DCUserConnLocal *ucl, char **str)
{
    int res;

    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_GET_MY_NICK, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return false;
    }
    res = msgq_get_sync(ucl->get_mq, MSGQ_STR, str, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return false;
    }
    return true;
}

static bool
nick_validate(DCUserConnLocal *ucl, const char *str)
{
    bool reply;
    int res;

    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_VALIDATE_NICK, MSGQ_STR, str, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return false;
    }
    res = msgq_get_sync(ucl->get_mq, MSGQ_BOOL, &reply, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return false;
    }
    if (!reply) {
	warn(_("User %s not on hub, or too many connections to user.\n"), quotearg(str));/*XXX: move main.c */
	terminate_process(ucl); /* MSG: msg above */
    	return false;
    }
    return true;
}

static void
end_download(DCUserConnLocal *ucl, bool success, const char *reason_fmt, ...)
{
    va_list args;
    char *reason = NULL;
    int res;

    if (ucl->transfer_fd/*DL*/ >= 0) {
        /* Inability to close the downloaded file is a serious error. */
        if (close(ucl->transfer_fd/*DL*/) < 0) {
            warn(_("%s: Cannot close file - %s\n"), quotearg(ucl->local_file/*DL*/), errstr);
            if (success) { /* don't let this error obscure a previous error */
                reason = xstrdup("local error");
                success = false;
            }
        }
        ucl->transfer_fd/*DL*/ = -1;
    }

    va_start(args, reason_fmt);
    if (reason == NULL)
        reason = xvasprintf(reason_fmt, args);
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_DOWNLOAD_ENDED, MSGQ_BOOL, success, MSGQ_STR, reason, MSGQ_END);
    if (res <= 0)
        fatal_error(ucl, res, true);
    free(reason);
    va_end(args);

    free(ucl->share_file/*DL*/);
    ucl->share_file/*DL*/ = NULL; /* if "user terminated" calls this function, then this is not necessary */
    free(ucl->local_file/*DL*/);
    ucl->local_file/*DL*/ = NULL; /* if "user terminated" calls this function, then this is not necessary */
}

/* This is called when the next file should be downloaded.
 * It will send $Get to the user.
 */
static void
download_next_file(DCUserConnLocal *ucl)
{
    int flag = 0;
    char *share_file;
    char *local_file, *conv_local_file;
    char *remote_file, *hub_remote_file;
    uint64_t resume_pos;
    uint64_t file_size;
    struct stat sb;
    int res;

    if (!ucl->user_running)
        return;
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_CHECK_DOWNLOAD, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return;
    }
    /* local_file is in filesystem charset */
    res = msgq_get_sync(ucl->get_mq, MSGQ_STR, &local_file, MSGQ_STR, &share_file, MSGQ_INT64, &file_size, MSGQ_INT, &flag, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return;
    }
    if (local_file == NULL) {
        flag_putf(DC_DF_CONNECTIONS, _("No more files to download.\n"));/*XXX:move main.c?*/
        terminate_process(ucl); /* MSG: no more to download */
        return;
    }

    /*
    int i = 0;
    for (i = 0; ucl->supports != NULL && i < ucl->supports->cur; i++) {
        if (ucl->supports->buf[i] != NULL) {
	        flag_putf(DC_DF_DEBUG, _("%s\n"), (char*)ucl->supports->buf[i]); 
        }
    }
    */

    if (flag == DC_TF_LIST) {
        free(share_file);
#if defined(HAVE_LIBXML2)
        if (ucl->supports != NULL && ptrv_find(ucl->supports, "XmlBZList", (comparison_fn_t)strcasecmp) >= 0) {
            share_file = strdup("/files.xml.bz2");
        } else {
            share_file = strdup("/MyList.DcLst");
        }
#else
        share_file = strdup("/MyList.DcLst");
#endif
        char* tmp = xasprintf("%s.%s", local_file, base_name(share_file));
        free(local_file);
        local_file = tmp;
    }

    conv_local_file = fs_to_main_string(local_file);
    ucl->share_file/*DL*/ = share_file;
	ucl->local_file = conv_local_file;

    /* XXX: what if file without ".part" exists and is complete? */
    /* Check if file already exists, and if it need to be resumed. */
    if (flag == DC_TF_LIST) {
        unlink(local_file);
        ucl->local_exists = false;
        resume_pos = 0;
    } else {
        if (lstat(local_file, &sb) != 0) {
            if (errno != ENOENT) {
                warn(_("%s: Cannot get file status: %s\n"), quotearg(local_file), errstr);
                end_download(ucl, false, _("local error"));
                download_next_file(ucl);
                return;
            }
            ucl->local_exists = false;
            resume_pos = 0;
        } else {
            if (!S_ISREG(sb.st_mode)) {
                warn(_("%s: File exists and is not a regular file\n"), quotearg(local_file));
                end_download(ucl, false, _("local error"));
                download_next_file(ucl);
                return;
            }
            ucl->local_exists = true;
            resume_pos = sb.st_size;
        }
    }
	free(local_file);

    remote_file = translate_local_to_remote(share_file);
    if (remote_file == NULL) {
        end_download(ucl, false, _("communication error"));
        return;
    }

    hub_remote_file = main_to_hub_string(remote_file);
    if (!user_putf(ucl, "$Get %s$%" PRIu64 "|", hub_remote_file , resume_pos+1)) { /* " */
        end_download(ucl, false, _("communication error"));
        free(remote_file);
        free(hub_remote_file);
        return;
    }
    free(remote_file);
    free(hub_remote_file);

    ucl->user_state = DC_USER_FILE_LENGTH;
    ucl->file_size = file_size;
    ucl->file_pos = resume_pos;
    ucl->transfer_pos = resume_pos;
}

static void
open_download_file(DCUserConnLocal *ucl, uint64_t file_size)
{
    int res;
    char *conv_local_file, *conv_share_file;

    if (ucl->file_size == UINT64_MAX) {
        /* A file size of UINT64_MAX means that we did not know the size
         * of the file in advance which is only true for file list files.
         */
        ucl->file_size = file_size;
    } else if (file_size < ucl->file_size) {
        end_download(ucl, false,
            _("remote file is smaller than local (expected %" PRIu64 ", got %" PRIu64 " %s)"),
            ucl->file_size, file_size, ngettext("byte", "bytes", file_size));
        /* We're probably left in a weird state here. It is possible that
         * some clients (at least DC++ 0.700) won't let us download more
         * files from here.
         */
        download_next_file(ucl);
        return;
    }

    conv_local_file = main_to_fs_string(ucl->local_file);

    if (ucl->local_exists) { /* Resuming */
        ucl->transfer_fd/*DL*/ = open/*64*/(conv_local_file/*DL*/, O_CREAT|O_WRONLY, 0644);
    } else {
        ucl->transfer_fd/*DL*/ = open/*64*/(conv_local_file/*DL*/, O_CREAT|O_EXCL|O_WRONLY, 0644);
    }

    free(conv_local_file);

    if (ucl->transfer_fd/*DL*/ < 0) {
        warn(_("%s: Cannot open file for writing - %s\n"), quotearg(ucl->local_file/*DL*/), errstr);
        end_download(ucl, false, _("local error"));
        download_next_file(ucl);
        return;
    }
    if (ucl->file_pos != 0 && lseek(ucl->transfer_fd/*DL*/, ucl->file_pos, SEEK_SET) < 0) {
        warn(_("%s: Cannot seek to resume position - %s\n"), quotearg(ucl->local_file/*DL*/), errstr);
        end_download(ucl, false, _("local error"));
        download_next_file(ucl);
        return;
    }

    if (!user_putf(ucl, "$Send|")) {
        end_download(ucl, false, _("communication error"));
        return;
    }

    conv_share_file = hub_to_main_string(ucl->share_file);
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_TRANSFER_START, MSGQ_STR, ucl->local_file, MSGQ_STR, conv_share_file, MSGQ_INT64, ucl->file_pos, MSGQ_INT64, ucl->file_size, MSGQ_END);
    free(conv_share_file);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        /*end_download(ucl, false, "internal communication error");*/ /* Just won't go through! */
        return;
    }

    ucl->data_size/*DL*/ = ucl->file_size - ucl->file_pos;
    if (ucl->data_size/*DL*/ == 0) {
        end_download(ucl, true, _("no data to transfer"));
        download_next_file(ucl);
        return;
    }
    ucl->user_state = DC_USER_DATA_RECV;

    /* Places to go from here
     *   user_running becomes false
     *     User process termination. user_disconnect deals with used_*_slots.
     *   data received until file complete (possible no data)
     *     Will issue DC_MSG_DOWNLOAD_ENDED prior to calling
     *     download_next_file.
     */
}

static void
end_upload(DCUserConnLocal *ucl, bool success, const char *reason)
{
    int res;

    if (ucl->transfer_fd/*DL*/ >= 0) {
        close(ucl->transfer_fd/*DL*/);  /* Ignore errors */
        ucl->transfer_fd/*DL*/ = -1;
    }
    free(ucl->share_file/*UL*/);
    ucl->share_file/*UL*/ = NULL;
    free(ucl->local_file/*UL*/);
    ucl->local_file/*UL*/ = NULL;
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_UPLOAD_ENDED, MSGQ_BOOL, success, MSGQ_STR, reason, MSGQ_END);
    if (res <= 0)
        fatal_error(ucl, res, true);
}

static int
open_upload_file_main(DCUserConnLocal *ucl, const char *str, uint64_t offset, DCAdcgetType type)
{
    struct stat st;
    bool may_upload;
    char *share_file;
    char *local_file, *conv_local_file;
    int res;

    if (strlen(str) > 0) {
#if 0
        if ( type == DC_ADCGET_TTHL) {
	        if (offset == 0)
                offset = /*fsize*/ sizeof(uint64_t) + /*mtime*/sizeof(time_t)
                   + /*ctime*/ sizeof(time_t) + /*tth base32*/39;
	        else {
                flag_putf(DC_DF_CONNECTIONS, _("%s: Incorrect offset in tthl request\n"), quotearg(ucl->local_file/*UL*/));
                user_putf(ucl, "$Error Incorrect offset in tthl request|");
                end_upload(ucl, false, _("Incorrect offset in tthl request"));

                return -9;
            }
        }
#endif
        if (type == DC_ADCGET_FILE)
            share_file = translate_remote_to_local(str);
        else
            share_file = xstrdup(str);

        res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_CHECK_UPLOAD, MSGQ_INT, type, MSGQ_STR, share_file, MSGQ_END);
        if (res <= 0) {
            free(share_file);
            return -1;
        }
        res = msgq_get_sync(ucl->get_mq, MSGQ_BOOL, &may_upload, MSGQ_STR, &local_file, MSGQ_END);
        if (res <= 0) {
            free(share_file);
            return -2;
        }
        if (!may_upload) {
            user_putf(ucl, "$MaxedOut|");
            free(share_file);
            return -3;
        }

        if (type == DC_ADCGET_FILE) {
            ucl->share_file/*UL*/ = share_file;
        } else {
            free(share_file);
            ucl->share_file = xstrdup(base_name(local_file));
        }
        ucl->local_file/*UL*/ = local_file;
    } else {
        ucl->local_file/*UL*/ = NULL;
    }

    if (ucl->local_file/*UL*/ == NULL) {
        flag_putf(DC_DF_CONNECTIONS, _("%s: File Not Available\n"), quotearg(ucl->local_file/*UL*/));
        user_putf(ucl, "$Error File Not Available|");
        end_upload(ucl, false, _("no such shared file"));
        return -4;
    }

    conv_local_file = main_to_fs_string(ucl->local_file);

    if (stat(conv_local_file/*UL*/, &st) < 0) {
        free (conv_local_file);
        flag_putf(DC_DF_CONNECTIONS, _("%s: Cannot get file status - %s\n"), quotearg(ucl->local_file/*UL*/), errstr);
        user_putf(ucl, "$Error File Not Available|");
        end_upload(ucl, false, _("local error"));
        return -5;
    }

#if 0
    if ( type == DC_ADCGET_TTHL) {
        if ( (offset + 2*TIGERSIZE > st.st_size)
            || ( (st.st_size - offset) % TIGERSIZE)) {
            free (conv_local_file);
            flag_putf(DC_DF_CONNECTIONS, _("%s: Resume offset %" PRIu64 " outside tthl file\n"), quotearg(ucl->local_file/*UL*/), offset);
            user_putf(ucl, "$Error File Not Available|");
            end_upload(ucl, false, _("resume offset out of tthl range"));
            return -10;
        }
    }
#endif

    if (offset > st.st_size) {
        free (conv_local_file);
        flag_putf(DC_DF_CONNECTIONS, _("%s: Resume offset %" PRIu64 " outside file\n"), quotearg(ucl->local_file/*UL*/), offset);
        user_putf(ucl, "$Error Offset out of range|");
        end_upload(ucl, false, _("resume offset out of range"));
        return -6;
    }
    ucl->transfer_fd/*UL*/ = open/*64*/(conv_local_file/*UL*/, O_RDONLY);
    if (ucl->transfer_fd/*UL*/ < 0) {
        free (conv_local_file);
        flag_putf(DC_DF_CONNECTIONS, _("%s: Cannot open file for reading - %s\n"), quotearg(ucl->local_file/*UL*/), errstr);
        user_putf(ucl, "$Error File Not Available|");
        end_upload(ucl, false, _("local error"));
        return -7;
    }
    free (conv_local_file);
    if (offset != 0 && lseek/*64*/(ucl->transfer_fd/*UL*/, offset, SEEK_SET) < 0) {
        flag_putf(DC_DF_CONNECTIONS, _("%s: Cannot seek in file - %s\n"), quotearg(ucl->local_file/*UL*/), errstr);
        user_putf(ucl, "$Error File Not Available|");
        end_upload(ucl, false, _("local error"));
        return -8;
    }

    ucl->file_pos = offset;
    ucl->transfer_pos = offset;
    ucl->user_state = DC_USER_SEND_GET;
    ucl->file_size = st.st_size;

    return 0;

    /* Places to go from here:
     *   user_running becomes false
     *     User process termination. user_disconnect deals with used_*_slots.
     *   $Get received
     *     Will issue DC_MSG_UPLOAD_ENDED prior to calling this
     *     function again.
     *   $Send received with file_size == 0
     *   $Send received with file_size != 0, download finished
     *     Will issue DC_MSG_UPLOAD_ENDED prior to setting state
     *     to DC_USER_GET.
     */
}

static void
open_upload_file(DCUserConnLocal *ucl, const char *str, uint64_t offset)
{
    if ( open_upload_file_main(ucl, str, offset, DC_ADCGET_FILE) < 0 )
	return;

    ucl->final_pos = ucl->file_size;

    if (!user_putf(ucl, "$FileLength %" PRIu64 "|", ucl->file_size)) {
        end_upload(ucl, false, _("communication error"));
            return;
        }

}

#if defined(HAVE_LIBXML2)

static void
open_upload_file_block(DCUserConnLocal *ucl, const char *str, uint64_t offset, uint64_t numbytes) 
{

    if ( open_upload_file_main(ucl, str, offset, DC_ADCGET_FILE) < 0)
        return;

    if ((numbytes == (uint64_t)-1) ||
        (offset + numbytes >= ucl->file_size))
        ucl->final_pos = ucl->file_size;
    else
        ucl->final_pos = offset + numbytes;

    if (!user_putf(ucl, "$Sending %" PRIu64 "|", ucl->final_pos - ucl->file_pos)){
        end_upload(ucl, false, _("communication error"));
        return;
    }

    upload_file(ucl);
}


static void
open_upload_file_adcget(DCUserConnLocal *ucl, const char *type, const char *str, uint64_t offset, uint64_t numbytes)
{
    char *filename;
    DCAdcgetType t = DC_ADCGET_FILE;

    if ( strcmp(type, "file") == 0)
        t = DC_ADCGET_FILE;
    else if ( strcmp(type, "tthl") == 0)
        t = DC_ADCGET_TTHL;
    else {
	    if ( !user_putf(ucl, "$Error Unknown ADCGET type: %s|", type) ){
	        end_upload(ucl, false, _("communication error"));
	        return;
        }
    }

    if ((strlen(str) == 4 + 39)
	    && (str[0] == 'T')
	    && (str[1] == 'T')
	    && (str[2] == 'H')
	    && (str[3] == '/')) {
        filename = xstrdup(str+4);

        if (t != DC_ADCGET_TTHL)
	        t = DC_ADCGET_TTH;
    } else {
        char *s1, *s2;

        // name must be converted from UTF-8 to local charset
        filename = utf8_to_main_string(str);
        if (filename == NULL)
	        return;

        /* unescape filename */
        s1 = s2 = filename;
        while  ( *s1 != '\0' ) {
	        if ( *s2 == '\\')
                s2++;
	        *s1++ = *s2++;
        }
    }


    if (numbytes == (uint64_t)-1) {
        flag_putf(DC_DF_DEBUG, _("User requests entire file <%s> starting from %" PRIu64 "\n"), filename, offset);
    } else {
        flag_putf(DC_DF_DEBUG, _("User requests %" PRIu64 " bytes of <%s> starting from %" PRIu64 "\n"), numbytes, filename, offset);
    }

    if (open_upload_file_main(ucl, filename, offset, t) < 0) {
        free(filename);
        return;
    }

    free(filename);

    if ((numbytes == (uint64_t)-1) || (offset + numbytes >= ucl->file_size))
        ucl->final_pos = ucl->file_size;
    else
        ucl->final_pos = offset + numbytes;

    if (!user_putf(ucl, "$ADCSND %s %s %" PRIu64 " %" PRIu64 "|", type, str,
                   ucl->transfer_pos, ucl->final_pos - ucl->transfer_pos)) {
        end_upload(ucl, false, _("communication error"));
        return;
    }

    upload_file(ucl);
}

#endif

static void
upload_file(DCUserConnLocal *ucl)
{
    int res;

    if (ucl->final_pos == ucl->file_pos) {
        /* There's nothing to send - size is 0 or resuming at end. */
        end_upload(ucl, true, _("no data to transfer"));
        ucl->user_state = DC_USER_GET;
        return;
    }
    res = msgq_put_sync(ucl->put_mq, MSGQ_INT, DC_MSG_TRANSFER_START, MSGQ_STR, ucl->local_file, MSGQ_STR, ucl->share_file, MSGQ_INT64, ucl->file_pos, MSGQ_INT64, ucl->final_pos, MSGQ_END);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        /*end_upload(ucl, true);*/ /* Just won't go through */
        return;
    }
    FD_SET(ucl->user_socket, &ucl->user_write_fds);
    assert(ucl->user_sendq->cur == 0);
    ucl->user_state = DC_USER_DATA_SEND;
}

/* Handle commands and data sent by the hub.
 */
static void
user_handle_command(DCUserConnLocal *ucl, char *buf, uint32_t len)
{
    if (ucl->user_state == DC_USER_DATA_RECV) {
    	ssize_t res;

    	res = full_write(ucl->transfer_fd/*DL*/, buf, len);
	    if (res < len) {
	        /* We cannot expect to synchronize with remote at this point, so shut down. */
	        warn_file_error(res, true, ucl->share_file/*DL*/);
	        end_download(ucl, false, _("local error"));
	        terminate_process(ucl); /* MSG: local error */
	        return;
	    }
	    ucl->file_pos += len;
	    ucl->transfer_pos += len;
	    ucl->data_size/*DL*/ -= len;
    	send_user_status(ucl, ucl->file_pos);
    	if (ucl->file_pos == ucl->file_size) {
	        end_download(ucl, true, _("transfer complete"));
	        download_next_file(ucl);
	        return;
	    }
    }
    else if (len >= 8 && strncmp(buf, "$MyNick ", 8) == 0) {
	    char *local_nick;

        if (!check_state(ucl, buf, DC_USER_MYNICK))
	        return;
	    local_nick = hub_to_main_string(buf+8);

	    if (!nick_validate(ucl, local_nick)){
            free(local_nick);
	        return;
	    }

        ucl->user_nick = local_nick;

        if (!ucl->we_connected) {
            char *our_nick;
            char *hub_my_nick;

            if (!get_our_nick(ucl, &our_nick))
                return;

            hub_my_nick = main_to_hub_string(our_nick);

            if (!user_putf(ucl, "$MyNick %s|", hub_my_nick)) {
	            free(our_nick);
                free(hub_my_nick);
		        return;
            }
            free(our_nick);
            free(hub_my_nick);
    	    if (!user_putf(ucl, "$Lock %s Pk=%s|", LOCK_STRING, LOCK_PK_STRING))
		        return;
	    }
	    ucl->user_state = DC_USER_LOCK;
    }
    else if (len >= 6 && strncmp(buf, "$Lock ", 6) == 0) {
        char *key;
	    bool download;

        if (!check_state(ucl, buf, DC_USER_LOCK))
	        return;
        key = memmem(buf+6, len-6, " Pk=", 4);
        if (key == NULL) {
            warn(_("Invalid $Lock message: Missing Pk value\n"));
	        key = buf+len;
	    }
        key = decode_lock(buf+6, key-buf-6, DC_CLIENT_BASE_KEY);
        if (!wants_to_download(ucl, &download)) {
            free(key);
	        return;
        }
#if defined(HAVE_LIBXML2)
	    /*if (!user_putf(ucl, "$Supports XmlBZList|")) {*/
	    /*if (!user_putf(ucl, "$Supports XmlBZList ADCGet TTHF TTHL|")) {*/
	    if (!user_putf(ucl, "$Supports MiniSlots XmlBZList ADCGet TTHF|")) {
	        free(key);
	        return;
	    }
#endif
	    if (!user_putf(ucl, "$Direction %s %d|", download ? "Download" : "Upload", ucl->dir_rand)) {
	        free(key);
	        return;
	    }
        if (!user_putf(ucl, "$Key %s|", key)) {
	        free(key);
	        return;
	    }
	    free(key);
	    ucl->user_state = DC_USER_SUPPORTS;
    }
    else if (len >= 10 && strncmp(buf, "$Supports ", 10) == 0) {
        char* p = buf+10;
        if (!check_state(ucl, buf, DC_USER_SUPPORTS))
	        return;

        if (ucl->supports != NULL) {
            ptrv_foreach(ucl->supports, free);
        }

        do {
            char* token = strsep(&p, " ");
            if (token != NULL)
                ptrv_append(ucl->supports, strdup(token));
        } while (p != NULL);
	    ucl->user_state = DC_USER_DIRECTION;
    }
    else if (len >= 11 && strncmp(buf, "$Direction ", 11) == 0) {
    	char *token;
    	uint16_t remote_rand;
	    bool they_download;
	    bool we_download;

        if (!check_state(ucl, buf, DC_USER_DIRECTION))
	        return;

    	token = strtok(buf+11, " ");
	    if (token == NULL) {
	        warn(_("Invalid $Direction message: Missing direction parameter\n"));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }
    	if (strcmp(token, "Upload") == 0) {
	        they_download = false;
	    } else if (strcmp(token, "Download") == 0) {
	        they_download = true;
	    } else {
	        warn(_("Invalid $Direction message: Invalid direction parameter\n"));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }

	    token = strtok(NULL, " ");
	    if (token == NULL) {
	        warn(_("Invalid $Direction message: Missing challenge parameter\n"));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }
	    if (!parse_uint16(token, &remote_rand)) {
	        warn(_("Invalid $Direction message: Invalid challenge parameter\n"));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }

	    if (!wants_to_download(ucl, &we_download))
	        return;

    	if (they_download) { /* Remote wants to download. Do we want to download too? */
	        if (we_download) {
		        if (remote_rand >= ucl->dir_rand) { /* We lost. Let them download. */
		            ucl->our_dir = DC_DIR_SEND;
		        } else { /* We won. */
		            ucl->our_dir = DC_DIR_RECEIVE;
		        }
                /* XXX: what should happen if remote_rand == dir_rand!? */
	        } else { /* We don't want to download anything. Let remote download. */
		        ucl->our_dir = DC_DIR_SEND;
	        }
	    } else { /* Remote wants to upload. Do we want to upload too? */
	        if (!we_download) {
		        warn(_("User does not want to download, nor do we.\n"));
		        terminate_process(ucl); /* MSG: no one wants to exchange files */
		        return;
	        }
	        ucl->our_dir = DC_DIR_RECEIVE;
	    }
	    if (!direction_validate(ucl, ucl->our_dir))
	        return;
	    ucl->user_state = DC_USER_KEY;
    }
    else if (len >= 5 && strncmp(buf, "$Key ", 5) == 0) {
   	    char *key;

        if (!check_state(ucl, buf, DC_USER_KEY))
	        return;
    	key = decode_lock(LOCK_STRING, LOCK_STRING_LEN, DC_CLIENT_BASE_KEY);
	    if (strcmp(buf+5, key) != 0)
	        warn(_("Invalid $Key message: Incorrect key, ignoring\n"));
	    free(key);
    	if (ucl->our_dir == DC_DIR_SEND) {
	        ucl->user_state = DC_USER_GET;
	    } else {
	        download_next_file(ucl);
	    }
    }
    else if (len >= 5 && strncmp(buf, "$Get ", 5) == 0) {
	    char *token;
	    uint64_t offset = 0;
        int filename_len = 0;

	    if (ucl->user_state != DC_USER_SEND_GET && ucl->user_state != DC_USER_GET) {
	        warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }

        token = strtok(buf+5, "$");
        if (*(buf+5) != '$') {
            filename_len = strlen(buf+5);
        }

	    if (token == NULL) {
	        warn(_("Invalid $Get message: Missing offset, assuming start\n"));
	        offset = 0;
	    } else if (filename_len > 0) {
	        token = strtok(NULL, ""); /* Cannot fail! */

            if (token == NULL || !parse_uint64(token, &offset)) {
	    	    warn(_("Invalid $Get message: Offset not integer\n"));
            
		        terminate_process(ucl); /* MSG: protocol error */
		        return;
	        }

            if (offset > 0)
	    	    offset--;
	    }

        /* Maybe the remote user did not want the file after all? */
	    if (ucl->user_state == DC_USER_SEND_GET)
	        end_upload(ucl, false, _("remote did not want file"));

        if (filename_len > 0) {
            /* Convert filename from hub to local charset
             * Try utf-8 if it fails
             */
            char *s =
            /*
                hub_to_main_string(buf+5);
            */
                utf8_to_main_string(buf+5);
            /*
            if (s == NULL)
	            s = iconv_alloc(from_utf8, buf+5);
            */
            open_upload_file(ucl, s, offset); /* Changes state */
            free(s);
        }
    }
    else if (len == 9 && strcmp(buf, "$MaxedOut") == 0) {
    	if (!check_state(ucl, buf, DC_USER_FILE_LENGTH))
	        return;
	    end_download(ucl, false, _("remote is maxed out"));
	    terminate_process(ucl); /* MSG: msg above */
    }
    else if (len >= 12 && strncmp(buf, "$FileLength ", 12) == 0) {
        uint64_t file_size;

    	if (!check_state(ucl, buf, DC_USER_FILE_LENGTH))
	        return;
	    if (!parse_uint64(buf+12, &file_size)) {
	        end_download(ucl, false, _("protocol error: invalid $FileLength message"));
	        download_next_file(ucl);
	        return;
	    }
    	open_download_file(ucl, file_size); /* Will change state */
    }
    else if (len >= 7 && strncmp(buf, "$Error ", 7) == 0) {
	    if (ucl->user_state == DC_USER_FILE_LENGTH) {
    	    if (strcmp(buf+7, "File Not Available") == 0) {
	    	    end_download(ucl, false, _("file not available on remote"));
	        } else {
	            end_download(ucl, false, _("remote error: %s"), quotearg(buf+7));
	        }
            download_next_file(ucl);
	        return;
	    }
	    warn(_("Received error from user: %s\n"), quotearg(buf+7));
	    terminate_process(ucl); /* MSG: remote replied with error (including msg above) */
    }
    else if (len >= 5 && strncmp(buf, "$Send ", 5) == 0) {
    	if (!check_state(ucl, buf, DC_USER_SEND_GET))
	        return;
        upload_file(ucl);
    }
#if defined(HAVE_LIBXML2)
    else if (len >= 11 && strncmp(buf, "$UGetBlock ", 11) == 0) {
        char filename[10240];
        char* p_filename = NULL;
        uint64_t offset = 0, block_size = 0;

        if (!check_state(ucl, buf, DC_USER_GET)) {
	        warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
	        terminate_process(ucl); /* MSG: protocol error */
	        return;
	    }

        if (3 != sscanf(buf+11, "%" PRIu64 "%" PRIu64 "%*c%[^\n]", &offset, &block_size, filename)) {
            warn(_("Invalid $UGetBlock message\n"));
            terminate_process(ucl); /* MSG: protocol error */
            return;
	    }

        p_filename = utf8_to_main_string(filename);
  
	    flag_putf(DC_DF_DEBUG, _("User requests %" PRIu64 " bytes of <%s> starting from %" PRIu64 "\n"), block_size, p_filename, offset); 

        open_upload_file_block(ucl, p_filename, offset, block_size);

        free(p_filename);

    }
    else if (len >= 8 && strncmp(buf, "$ADCGET ", 8) == 0) {
        char *type, *filename, *startpos, *numbytes, *flags;
        uint64_t n_startpos, n_numbytes;
  
        if (!check_state(ucl, buf, DC_USER_GET)) {
            warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
            terminate_process(ucl); /* MSG: protocol error */
            return;
        }

        filename = buf + 8;
        type = strsep(&filename, " ");

        if ( (type[0] == '\0') 
            || (filename == NULL)
            || (filename == '\0')
            || (filename[0] == ' ' )) {
            warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
            terminate_process(ucl); /* MSG: protocol error */
            return;
        }

        /* search for first non-escaped space */
        for ( startpos = filename + 1; *startpos; startpos++) {
            if ( ( startpos[0] == ' ')
            && ( startpos[-1] != '\\' ) )
	            break;
        }

        if (*startpos == '\0') {
            warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
            terminate_process(ucl); /* MSG: protocol error */
            return;
        }

        startpos[0] = '\0';
        numbytes = startpos + 1;
        startpos = strsep(&numbytes, " ");

        if ( (numbytes == NULL)
            || (!parse_uint64(startpos, &n_startpos))) {
            warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
            terminate_process(ucl); /* MSG: protocol error */
            return;
        }

        flags = numbytes;
        numbytes = strsep(&flags, " \r\n");

        if ( (!parse_int64(numbytes, &n_numbytes))) {
            warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
            terminate_process(ucl); /* MSG: protocol error */
            return;
        }

        if ( flags != NULL ) {
            warn(_("Ignoring $ADCGET flags: %s\n"), flags);
        }

        open_upload_file_adcget(ucl, type, filename , n_startpos, n_numbytes);
    }
#endif
}


static void
user_input_available(DCUserConnLocal *ucl)
{
    int start = 0;
    int c;
    int res;

    alarm(0); /* cannot fail */
    res = byteq_read(ucl->user_recvq, ucl->user_socket);
    if (res == 0 || (res < 0 && errno != EAGAIN && errno != EINTR)) {
 	    warn_socket_error(res, false, _("user"));
	    terminate_process(ucl); /* MSG: socket error above */
	    return;
    }

    for (c = ucl->user_recvq_last; c < ucl->user_recvq->cur; c++) {
	if (ucl->data_size/*DL*/ > 0) {
	    uint32_t size;

	    /* Handle what've got, but never more than data_size */
	    size = MIN(ucl->data_size/*DL*/, ucl->user_recvq->cur - start);
    	    user_handle_command(ucl, ucl->user_recvq->buf + start, size);
	    start += size;
	    if (!ucl->user_running)
	    	break;
	    c += size - 1;
	}
        else if (ucl->user_recvq->buf[c] == '|') {
            /* Got a complete command. */
	    if (c - start > 0)
	    	dump_command(_("<--"), ucl->user_recvq->buf + start, c - start + 1);
            ucl->user_recvq->buf[c] = '\0'; /* Just to be on the safe side... */
            user_handle_command(ucl, ucl->user_recvq->buf + start, c - start);
            start = c+1;
	    if (!ucl->user_running)
	    	break;
        }
    }

    if (start != 0)
        byteq_remove(ucl->user_recvq, start);

    ucl->user_recvq_last = ucl->user_recvq->cur;
    alarm(USER_CONN_IDLE_TIMEOUT); /* cannot fail */
}

static void
user_now_writable(DCUserConnLocal *ucl)
{
    alarm(0); /* cannot fail */
    if (ucl->user_state == DC_USER_CONNECT) {
        int error;
        socklen_t size = sizeof(error);

        if (getsockopt(ucl->user_socket, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            warn(_("Cannot get error status - %s\n"), errstr);
            terminate_process(ucl); /* MSG: local error */
            return;
        }
        if (error != 0) { /* The connect call on the socket failed. */
            warn(_("Cannot connect - %s\n"), strerror(error) /* not errno! */);
            terminate_process(ucl); /* MSG: local error */
            return;
        }

        FD_CLR(ucl->user_socket, &ucl->user_write_fds);
        FD_SET(ucl->user_socket, &ucl->user_read_fds);
        ucl->user_state = DC_USER_MYNICK;

    	if (ucl->we_connected) {
            char *our_nick;
            char *hub_my_nick;
    	    
            if (!get_our_nick(ucl, &our_nick))
                return;
            hub_my_nick = main_to_hub_string(our_nick);
            flag_putf(DC_DF_CONNECTIONS, _("Connected to user.\n"));/*XXX: move where!?*/
            if (!user_putf(ucl, "$MyNick %s|", hub_my_nick)) {
                free(our_nick);
                free(hub_my_nick);
                return;
            }
            free(our_nick);
            free(hub_my_nick);
    	    if (!user_putf(ucl, "$Lock %s Pk=%s|", LOCK_STRING, LOCK_PK_STRING))
        		return;
        }
    }
    else if (ucl->user_state == DC_USER_DATA_SEND) {
        size_t block;
    	ssize_t res;

    	assert(ucl->file_size != 0);
    	block = MIN(DEFAULT_SENDQ_SIZE, ucl->final_pos - ucl->file_pos);
        if (block > 0 && ucl->user_sendq->cur == 0) { //if (ucl->user_sendq->cur < ucl->user_sendq->max) {
            res = byteq_full_read_upto(ucl->user_sendq, ucl->transfer_fd/*UL*/, block);
            if (res < block) {
                warn_file_error(res, false, ucl->local_file/*UL*/);
                end_upload(ucl, false, _("local error"));
                terminate_process(ucl); /* MSG: local error */
                return;
            }
            ucl->file_pos += res;
        }

        assert(ucl->user_sendq->cur != 0);
        res = byteq_write(ucl->user_sendq, ucl->user_socket);
        if (res == 0 || (res < 0 && errno != EAGAIN && errno != EINTR)) {
            warn_socket_error(res, true, _("user"));
            end_upload(ucl, false, _("communication error"));
            terminate_process(ucl); /* MSG: communication error */
            return;
        }
        ucl->transfer_pos += res;
        send_user_status(ucl, ucl->file_pos - ucl->user_sendq->cur);

        if (ucl->file_pos == ucl->final_pos && ucl->user_sendq->cur == 0) {
            FD_CLR(ucl->user_socket, &ucl->user_write_fds);
            end_upload(ucl, true, _("transfer complete"));
            ucl->user_state = DC_USER_GET;
        }
    }
    else {
    	int res;

        if (ucl->user_sendq->cur > 0) {
            res = byteq_write(ucl->user_sendq, ucl->user_socket);
            if (res == 0 || (res < 0 && errno != EAGAIN && errno != EINTR)) {
                warn_socket_error(res, true, _("user"));
                terminate_process(ucl); /* MSG: socket error above */
                return;
            }
        }
        if (ucl->user_sendq->cur == 0)
            FD_CLR(ucl->user_socket, &ucl->user_write_fds);
    }
    alarm(USER_CONN_IDLE_TIMEOUT); /* cannot fail */
}

static void
signal_received(int signal)
{
    DCUserConnLocal *ucl = cur_ucl; /* XXX */
    uint8_t signal_char;

    signal_char = signal;
    /* We don't care if this blocks - since we can't postpone this. */
    if (write(ucl->signal_pipe[1], &signal_char, sizeof(uint8_t)) < sizeof(uint8_t)) {
	/* Die only if the signal is fatal.
	 * SIGALRM is sent when the connection has been idle too long.
	 * It will result in the connection shutting down, so if we
	 * can't deliver it we might just shut down here.
	 */
	if (signal == SIGTERM || signal == SIGALRM)
	    die(_("Cannot write to signal pipe - %s\n"), errstr); /* die OK */
        warn(_("Cannot write to signal pipe - %s\n"), errstr);
    }
}

static void
read_signal_input(DCUserConnLocal *ucl)
{
    uint8_t signal;

    /* This read is atomic since sizeof(int) < PIPE_BUF!
     * It also doesn't block since all data is already
     * available (otherwise select wouldn't tell us there
     * was data).
     */
    if (read(ucl->signal_pipe[0], &signal, sizeof(uint8_t)) < 0) {
	warn(_("Cannot read from signal pipe - %s\n"), errstr);
	terminate_process(ucl); /* MSG: local error */
	return;
    }

    if (signal == SIGTERM) {
        warn(_("Received TERM signal, shutting down.\n"));
        terminate_process(ucl); /* MSG: terminating by signal */
    } else if (signal == SIGALRM) {
        warn(_("Idle timeout (%d seconds)\n"), USER_CONN_IDLE_TIMEOUT);
	terminate_process(ucl); /* MSG: idle timeout msg above */
    } else if (signal == SIGUSR1) {
        /* Not implemented yet, do nothing for now. */
    }
}

static void
main_request_fd_writable(DCUserConnLocal *ucl)
{
    int res;

    res = msgq_write(ucl->put_mq);
    if (res <= 0) {
        fatal_error(ucl, res, true);
        return;
    }
    if (!msgq_has_partial_msg(ucl->put_mq))
        FD_CLR(ucl->put_mq->fd, &ucl->user_write_fds);
}

static void
main_result_fd_readable(DCUserConnLocal *ucl)
{
    int res;

    res = msgq_read(ucl->get_mq);
    if (res <= 0) {
        fatal_error(ucl, res, false);
        return;
    }
    warn(_("Received unknown message from main process, shutting down process.\n"));
    ucl->user_running = false;
}

void
__attribute__((noreturn))
user_main(int get_fd[2], int put_fd[2], struct sockaddr_in *addr, int sock)
{
    struct sigaction sigact;
    DCUserConnLocal *ucl;

    filelist_free(our_filelist);

    ucl = xmalloc(sizeof(DCUserConnLocal));
    cur_ucl = ucl; /* one per process anyway */
    ucl->user_nick = NULL;
    ucl->share_file = NULL;
    ucl->local_file = NULL;
    ucl->user_recvq_last = 0;
    ucl->user_socket = -1;
    ucl->transfer_fd = -1;
    ucl->data_size = 0;     /* only useful when receiving files */
    ucl->file_pos = 0;
    ucl->final_pos = 0;
    ucl->transfer_pos = 0;
    ucl->file_size = 0;
    ucl->user_state = DC_USER_CONNECT;
    ucl->user_running = true;
    ucl->get_mq = msgq_new(get_fd[0]);
    ucl->put_mq = msgq_new(put_fd[1]);
    ucl->supports = ptrv_new();
    
    screen_writer = user_screen_writer;

    if (close(get_fd[1]) != 0 || close(put_fd[0]) != 0)
        warn(_("Cannot close pipe - %s\n"), errstr);

    if (pipe(ucl->signal_pipe) < 0) {
    	warn(_("Cannot create pipe pair - %s\n"), errstr);
	goto cleanup;
    }
    sigact.sa_handler = signal_received;
    if (sigemptyset(&sigact.sa_mask) < 0) {
        warn(_("Cannot empty signal set - %s\n"), errstr);
	goto cleanup;
    }
    sigact.sa_flags = SA_RESTART;
#ifdef HAVE_STRUCT_SIGACTION_SA_RESTORER
    sigact.sa_restorer = NULL;
#endif
    /* Note: every signal registered with a non-ignore action in main.c
     * must also be registered here, either with an action or as ignored.
     */
    if (sigaction(SIGTERM, &sigact, NULL) < 0
            || sigaction(SIGUSR1, &sigact, NULL) < 0
            || sigaction(SIGALRM, &sigact, NULL) < 0) {
        warn(_("Cannot register signal handler - %s\n"), errstr);
	goto cleanup;
    }
    sigact.sa_handler = SIG_IGN;
    if (sigaction(SIGINT, &sigact, NULL) < 0
          || sigaction(SIGCHLD, &sigact, NULL) < 0
          || sigaction(SIGPIPE, &sigact, NULL) < 0) {
        warn(_("Cannot register signal handler - %s\n"), errstr);
	goto cleanup;
    }

    ucl->user_recvq = byteq_new(DEFAULT_RECVQ_SIZE);
    ucl->user_sendq = byteq_new(DEFAULT_SENDQ_SIZE);
    ucl->dir_rand = rand() % 0x8000;
    if (sock < 0) {
	ucl->user_socket = socket(PF_INET, SOCK_STREAM, 0);
	if (ucl->user_socket < 0) {
            warn(_("Cannot create socket - %s\n"), errstr);
	    goto cleanup;
	}
    	ucl->we_connected = true;
    } else {
    	ucl->user_socket = sock;
    	ucl->we_connected = false;
    }

    /* Set non-blocking I/O on socket. */
    if (!fd_set_nonblock_flag(ucl->user_socket, true)) {
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        goto cleanup;
    }

    if (sock < 0) {
	/* Connect to host, non-blocking manner. Even if connect would return
	 * success, we would catch the successful connection after select -
	 * when socket_fd is writable.
	 */
	if (connect(ucl->user_socket, (struct sockaddr *) addr, sizeof(struct sockaddr_in)) < 0
	    	&& errno != EINPROGRESS) {
            warn(_("Cannot connect - %s\n"), errstr);
	    goto cleanup;
	}
    }

    FD_ZERO(&ucl->user_read_fds);
    FD_ZERO(&ucl->user_write_fds);
    FD_SET(ucl->signal_pipe[0], &ucl->user_read_fds);
    FD_SET(ucl->user_socket, &ucl->user_write_fds); /* will set read after connect() finishes */
    FD_SET(ucl->get_mq->fd, &ucl->user_read_fds);

    while (ucl->user_running) {
    	fd_set res_read_fds;
	fd_set res_write_fds;

	res_read_fds = ucl->user_read_fds;
	res_write_fds = ucl->user_write_fds;
	if (TEMP_FAILURE_RETRY(select(FD_SETSIZE, &res_read_fds, &res_write_fds, NULL, NULL)) < 0) {
	    warn(_("Cannot select - %s\n"), errstr);
	    break;
	}

    	/* We must check for user writable prior to readable, to detect
	 * connect() completion before any data is received and handled.
	 */

    	if (ucl->user_running && FD_ISSET(ucl->signal_pipe[0], &res_read_fds))
	    read_signal_input(ucl);
        if (ucl->user_running && FD_ISSET(ucl->put_mq->fd, &res_write_fds))
            main_request_fd_writable(ucl);
        if (ucl->user_running && FD_ISSET(ucl->get_mq->fd, &res_read_fds))
            main_result_fd_readable(ucl);
	if (ucl->user_running && FD_ISSET(ucl->user_socket, &res_write_fds))
	    user_now_writable(ucl);
	if (ucl->user_running && FD_ISSET(ucl->user_socket, &res_read_fds))
	    user_input_available(ucl);
    }

cleanup:

    free(ucl->local_file);
    free(ucl->share_file);
    free(ucl->user_nick);
    byteq_free(ucl->user_recvq);
    byteq_free(ucl->user_sendq);

    if (ucl->transfer_fd >= 0 && close(ucl->transfer_fd) < 0)
    	warn(_("Cannot close transfer file - %s\n"), errstr); /* XXX: should print WHAT file - then remove " transfer" */
    if (ucl->signal_pipe[0] >= 0 && close(ucl->signal_pipe[0]) < 0)
    	warn(_("Cannot close signal pipe - %s\n"), errstr);
    if (ucl->signal_pipe[1] >= 0 && close(ucl->signal_pipe[1]) < 0)
    	warn(_("Cannot close signal pipe - %s\n"), errstr);
    if (ucl->get_mq != NULL && close(ucl->get_mq->fd) != 0)
    	warn(_("Cannot close pipe - %s\n"), errstr);
    if (ucl->put_mq != NULL && close(ucl->put_mq->fd) != 0)
    	warn(_("Cannot close pipe - %s\n"), errstr);
    if (ucl->user_socket >= 0 && close(ucl->user_socket) < 0)
    	warn(_("Cannot close user connection - %s\n"), errstr);

    msgq_free(ucl->get_mq);
    msgq_free(ucl->put_mq);

    ptrv_foreach(ucl->supports, free);
    ptrv_free(ucl->supports);

    exit(EXIT_SUCCESS);
}
