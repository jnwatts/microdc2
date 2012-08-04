/* hub.c - Hub communication
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
#include <string.h>		/* C89 */
#include <stdlib.h>		/* C89 */
#include <sys/socket.h>		/* ? */
#include <netinet/in.h>		/* ? */
#include <arpa/inet.h>		/* ? */
#include <unistd.h>		/* POSIX */
#include <time.h>
#include <inttypes.h>		/* POSIX */
#include "iconvme.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "memmem.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/error.h"
#include "common/intutil.h"
#include "common/strleftcmp.h"
#include "microdc.h"

#define DEFAULT_HUB_RECVQ_SIZE 128
#define DEFAULT_HUB_SENDQ_SIZE 128

typedef enum {
    HUB_EXT_NOGETINFO	= 1 << 0,
    HUB_EXT_NOHELLO	= 1 << 1, 
} DCHubExtension;

ByteQ *hub_recvq = NULL;
ByteQ *hub_sendq = NULL;
int hub_socket = -1;
DCHubState hub_state = DC_HUB_DISCONNECTED;
HMap *hub_users = NULL;  /* all users on the current hub (UserInfo->nick => UserInfo) */
DCLookup *hub_lookup = NULL;
struct sockaddr_in hub_addr;
static uint32_t hub_recvq_last = 0;
char *hub_name = NULL;
static DCHubExtension hub_extensions = 0;

bool   hub_connected = false;

time_t hub_activity_check_interval = 150;
time_t hub_reconnect_interval = 10;
time_t hub_last_activity = 0;

void hub_set_connected(bool state)
{
    hub_connected = state;
}

void update_hub_activity()
{
    hub_last_activity = time(NULL);
}

void check_hub_activity()
{
    if (hub_connected) {
        time_t now = time(NULL);
        if (hub_state == DC_HUB_LOGGED_IN && hub_last_activity + hub_activity_check_interval <= now) {
            hub_putf("|");
        } else if (hub_state == DC_HUB_DISCONNECTED &&
                   (hub_last_activity + hub_reconnect_interval) <= now &&
                   running && auto_reconnect) {
	        warn(_("Automatically reconnecting to hub\n"));
            hub_connect(&hub_addr);
        }
    }
}

void hub_reconnect()
{
    hub_disconnect();
    hub_connect(&hub_addr);
}

bool
send_my_info(void)
{
    char *conv_nick = main_to_hub_string(my_nick);
    char *conv_desc = main_to_hub_string(my_description);
    char *conv_email = main_to_hub_string(my_email);
    bool res;

    /* XXX: hm, H:1/0/0 should be Normal/Registered/Op, calculate this value. */
    res = hub_putf("$MyINFO $ALL %s %s<%s,M:%c,H:1/0/0,S:%d>$ $%s%c$%s$%" PRIu64 "$|", /* " */
	  conv_nick,
	  conv_desc,
	  my_tag, is_active ? 'A':'P', my_ul_slots,
	  my_speed,
	  1, /* level, '1' means normal, see DCTC Documentation/Documentation/VAR */
	  conv_email,
	  my_share_size);
    free(conv_nick);
    free(conv_desc);
    free(conv_email);
    return res;
}

void
say_user_completion_generator(DCCompletionInfo *ci)
{
    HMapIterator it;

    /* XXX: perhaps hub_users should be made a tmap? to speed up things */
    hmap_iterator(hub_users, &it);
    while (it.has_next(&it)) {
    	DCUserInfo *ui = it.next(&it);

	if (strleftcmp(ci->word, ui->nick) == 0) {
	    DCCompletionEntry *entry;
	    entry = new_completion_entry_full(
	        quote_string(ui->nick, ci->word_full[0] == '"', true),
	        xstrdup(ui->nick),
	        "%s", "%s",
	        false,
	        true); /* entry->input and _single_fmt are already quoted */
	    entry->input_single_fmt = "%s: ";
	    ptrv_append(ci->results, entry);
	}
    }
    ptrv_sort(ci->results, completion_entry_display_compare);
}

void
user_or_myself_completion_generator(DCCompletionInfo *ci)
{
    if (strleftcmp(ci->word, my_nick) == 0)
        ptrv_append(ci->results, new_completion_entry(my_nick, NULL));
    user_completion_generator(ci);
}

void
user_completion_generator(DCCompletionInfo *ci)
{
    HMapIterator it;

    /* XXX: what if we are self found in this list? conflict with user_or_myself_completion_generator */
    hmap_iterator(hub_users, &it);
    while (it.has_next(&it)) {
    	DCUserInfo *ui = it.next(&it);
	if (strleftcmp(ci->word, ui->nick) == 0)
	    ptrv_append(ci->results, new_completion_entry(ui->nick, NULL));
    }
    ptrv_sort(ci->results, completion_entry_display_compare);
}

void
user_with_queue_completion_generator(DCCompletionInfo *ci)
{
    HMapIterator it;

    /* XXX: for completion speed, maintain a separate TMap for users with queue? */
    hmap_iterator(hub_users, &it);
    while (it.has_next(&it)) {
    	DCUserInfo *ui = it.next(&it);
	if (ui->download_queue->cur > 0 && strleftcmp(ci->word, ui->nick) == 0)
	    ptrv_append(ci->results, new_completion_entry(ui->nick, NULL));
    }
    ptrv_sort(ci->results, completion_entry_display_compare);
}

/* Create a new user info structure, representing a user on the hub.
 */
DCUserInfo *
user_info_new(const char *nick)
{
    DCUserInfo *info;
    char *ucname;

    info = xmalloc(sizeof(DCUserInfo));
    info->nick = xstrdup(nick);
    info->description = NULL;
    info->speed = NULL;
    info->level = 0;
    info->email = NULL;
    info->share_size = 0;
    info->active_state = DC_ACTIVE_UNKNOWN;
    info->download_queue = ptrv_new();
    info->slot_granted = false;
    info->refcount = 1;
    info->is_operator = false;
    info->info_quered = false;
    info->conn_count = 0;

    /* XXX Find existing connections to this user... */
    ucname = xasprintf("%s|%s", nick, _("UL"));
    info->conn[info->conn_count] = hmap_get(user_conns, ucname);
    if (info->conn[info->conn_count] != NULL)
    	info->conn_count++;
    free(ucname);
    ucname = xasprintf("%s|%s", nick, _("DL"));
    info->conn[info->conn_count] = hmap_get(user_conns, ucname);
    if (info->conn[info->conn_count] != NULL)
    	info->conn_count++;
    free(ucname);
    ucname = xasprintf("%s|", nick);
    info->conn[info->conn_count] = hmap_get(user_conns, ucname);
    if (info->conn[info->conn_count] != NULL)
        info->conn_count++;
    free(ucname);

    return info;
}

void
free_queued_file(DCQueuedFile *qf)
{
    free(qf->filename);
    free(qf->base_path);
    free(qf);
}

void
user_info_free(DCUserInfo *ui)
{
    ui->refcount--;
    if (ui->refcount == 0) {
	free(ui->nick);
	free(ui->description);
	free(ui->speed);
	free(ui->email);
	ptrv_foreach(ui->download_queue, (PtrVForeachCallback) free_queued_file);
	ptrv_free(ui->download_queue);
	free(ui);
    }
}

/* Put some data onto the connection, printf style.
 */
bool
hub_putf(const char *format, ...)
{
    va_list args;
    size_t oldcur;
    int res;

    oldcur = hub_sendq->cur;
    va_start(args, format);
    res = byteq_vappendf(hub_sendq, format, args);
    va_end(args);

    if (res < 0) {
    	warn(_("Cannot append to hub send queue - %s\n"), errstr);
        hub_disconnect();
        return false;
    }

    dump_command(_("-->"), hub_sendq->buf+oldcur, hub_sendq->cur-oldcur);

    res = byteq_write(hub_sendq, hub_socket);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
    	warn_socket_error(res, true, _("hub"));
        hub_disconnect();
        return false;
    }

    if (oldcur == 0 && hub_sendq->cur > 0)
    	FD_SET(hub_socket, &write_fds);

    update_hub_activity();

    return true;
}

static void
hub_address_looked_up(int rc, struct addrinfo *ai, void *data)
{
    char *hostname = data;

    hub_lookup = NULL;
    if (rc != 0) {
        screen_putf(_("%s: Cannot look up address - %s\n"), quotearg(hostname), gai_strerror(rc));
        free(data);
        return;
    }

    hub_set_connected(true);
    hub_connect((struct sockaddr_in *) ai->ai_addr);
    free(data);
}

/* port must be valid */
void
hub_new(const char *hostname, uint16_t port)
{
    struct sockaddr_in addr;

    if (inet_aton(hostname, &addr.sin_addr)) {
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        hub_set_connected(true);
        hub_connect(&addr); /* Ignore errors */
    } else {
        char portstr[6];

        sprintf(portstr, "%" PRIu16, port);
        screen_putf(_("Looking up IP address for %s\n"), quotearg(hostname));
        hub_lookup = add_lookup_request(hostname, portstr, NULL, hub_address_looked_up, xstrdup(hostname));
        hub_state = DC_HUB_LOOKUP;
    }
}

void
hub_connect(struct sockaddr_in *addr)
{
    hub_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (hub_socket < 0) {
        warn(_("Cannot create socket - %s\n"), errstr);
        hub_disconnect();
        return;
    }

    /* Set non-blocking I/O on socket. */
    if (!fd_set_nonblock_flag(hub_socket, true)) {
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        hub_disconnect();
        return;
    }

    screen_putf(_("Connecting to hub on %s.\n"), sockaddr_in_str(addr));
    if (connect(hub_socket, (struct sockaddr *) addr, sizeof(struct sockaddr_in)) < 0
    	    && errno != EINPROGRESS) {
        warn(_("%s: Cannot connect - %s\n"), sockaddr_in_str(addr), errstr);
        hub_disconnect();
        return;
    }

    if (&hub_addr != addr)
        hub_addr = *addr;
    FD_SET(hub_socket, &write_fds);
    hub_state = DC_HUB_CONNECT;
}

void
hub_disconnect(void)
{
    if (hub_state > DC_HUB_DISCONNECTED)
        screen_putf(_("Shutting down hub connection.\n"));
    if (hub_lookup != NULL) {
        cancel_lookup_request(hub_lookup);
        hub_lookup = NULL;
    }
    if (hub_socket >= 0) {
    	FD_CLR(hub_socket, &read_fds);
        FD_CLR(hub_socket, &write_fds);
    	if (close(hub_socket) < 0)
	    warn(_("Cannot close socket - %s\n"), errstr);
    	hub_socket = -1;
    }
    if (hub_users != NULL) {
        hmap_foreach_value(hub_users, user_info_free);
        hmap_clear(hub_users);
    }
    if (hub_sendq != NULL)
    	byteq_clear(hub_sendq);
    if (hub_recvq != NULL)
    	byteq_clear(hub_recvq);
    hub_recvq_last = 0;
    if (pending_userinfo != NULL) {
    	hmap_foreach_value(pending_userinfo, user_info_free);
    	hmap_clear(pending_userinfo);
    }
    free(hub_name);
    hub_name = NULL;
    hub_extensions = 0;
    hub_state = DC_HUB_DISCONNECTED;
    update_hub_activity();
}

static bool
check_state(char *buf, DCUserState state)
{
    if (hub_state != state) {
	warn(_("Received %s message in wrong state.\n"), strtok(buf, " "));
	hub_disconnect();
	return false;
    }
    return true;
}

static void
parse_hub_extension(const char *ext)
{
    if (strcmp(ext, "NoGetINFO") == 0) {
        hub_extensions |= HUB_EXT_NOGETINFO;
    } else if (strcmp(ext, "NoHello") == 0) {
        hub_extensions |= HUB_EXT_NOHELLO;
    }
}

static char *
prepare_chat_string_for_display(const char *str)
{
    char *t1, *t2;
    if (str[0] == '<' && 0 != (t1 = strstr(str, ">\0xA0"))) { // flag for unicode utf8 support
        *(t1+1) = ' ';
    }
    t1 = try_utf8_to_main_string(str);
    if (t1 == 0) {
        t1 = hub_to_main_string(str);
    }
    t2 = unescape_message(t1);
    free(t1);

    /*
    t1 = unescape_message(str);
    t2 = hub_to_main_string(t1);
    free(t1);
    */
    return t2;
}

static void
hub_handle_command(char *buf, uint32_t len)
{
    char *hub_my_nick; /* XXX */

    hub_my_nick = main_to_hub_string(my_nick);

    if (len >= 6 && strncmp(buf, "$Lock ", 6) == 0) {
        char *key;

        if (!check_state(buf, DC_HUB_LOCK))
	        goto hub_handle_command_cleanup;

        key = memmem(buf+6, len-6, " Pk=", 4);
        if (key == NULL) {
            warn(_("Invalid $Lock message: Missing Pk value\n"));
            key = buf+len;
	    }
        key = decode_lock(buf+6, key-buf-6, DC_CLIENT_BASE_KEY);
        if (strleftcmp("EXTENDEDPROTOCOL", buf+6) == 0) {
            if (!hub_putf("$Supports TTHSearch NoGetINFO NoHello|")) {
	            free(key);
	            goto hub_handle_command_cleanup;
            }
        }
        if (!hub_putf("$Key %s|", key)) {
            free(key);
	        goto hub_handle_command_cleanup;
        }
        free(key);
        if (!hub_putf("$ValidateNick %s|", hub_my_nick))
	        goto hub_handle_command_cleanup;
        hub_state = DC_HUB_HELLO;
    }
    else if (len >= 10 && strncmp(buf, "$Supports ", 10) == 0) {
        char *p0, *p1;

        hub_extensions = 0;
        for (p0 = buf+10; (p1 = strchr(p0, ' ')) != NULL; p0 = p1+1) {
            *p1 = '\0';
            parse_hub_extension(p0);
        }
        if (*p0 != '\0')
            parse_hub_extension(p0);
    }
    else if (strcmp(buf, "$GetPass") == 0) {
        if (my_password == NULL) {
            screen_putf(_("Hub requires password.\n"));
            hub_disconnect();
            goto hub_handle_command_cleanup;
        }
        screen_putf(_("Sending password to hub.\n"));
        if (!hub_putf("$MyPass %s|", my_password))
            goto hub_handle_command_cleanup;
    }
    else if (strcmp(buf, "$BadPass") == 0) {
        warn(_("Password not accepted.\n"));
        hub_disconnect();
    }
    else if (strcmp(buf, "$LogedIn") == 0) {
        screen_putf(_("You have received operator status.\n"));
    }
    else if (len >= 9 && strncmp(buf, "$HubName ", 9) == 0) {
        free(hub_name);
        hub_name = hub_to_main_string(buf + 9);
        screen_putf(_("Hub name is %s.\n"), quotearg(hub_name));
    }
    else if (strcmp(buf, "$GetNetInfo") == 0) {
        hub_putf("$NetInfo %d$1$%c|", my_ul_slots, is_active ? 'A' : 'P');
    }
    else if (strcmp(buf, "$ValidateDenide") == 0) {
        if (!check_state(buf, DC_HUB_HELLO))
            goto hub_handle_command_cleanup;
        /* DC++ disconnects immediately if this is received.
         * But shouldn't we give the client a chance to change the nick?
         * Also what happens if we receive this when correctly logged in?
         */
        warn(_("Hub did not accept nick. Nick may be in use.\n"));
        hub_disconnect();
    }
    else if (len >= 7 && strncmp(buf, "$Hello ", 7) == 0) {
        DCUserInfo *ui;
        char *conv_nick;

        conv_nick = hub_to_main_string(buf + 7);

        if (hub_state == DC_HUB_HELLO) {
            if (strcmp(buf+7, hub_my_nick) == 0) {
                screen_putf(_("Nick accepted. You are now logged in.\n"));
            } else {
	    	    /* This probably won't happen, but better safe... */
                free(my_nick);
                my_nick = xstrdup(conv_nick);
                free(hub_my_nick);
                hub_my_nick = xstrdup(buf + 7);
    	    	screen_putf(_("Nick accepted but modified to %s. You are now logged in.\n"), quotearg(my_nick));
            }

    	    ui = user_info_new(conv_nick);
            ui->info_quered = true; /* Hub is sending this automaticly */
            hmap_put(hub_users, ui->nick, ui);

            free (conv_nick);

            if (!hub_putf("$Version 1,0091|"))
                goto hub_handle_command_cleanup;
            if (!hub_putf("$GetNickList|"))
                goto hub_handle_command_cleanup;
            if (!send_my_info())
                goto hub_handle_command_cleanup;

            hub_state = DC_HUB_LOGGED_IN;
        } else {
            flag_putf(DC_DF_JOIN_PART, _("User %s logged in.\n"), quotearg(conv_nick));
            ui = user_info_new(conv_nick);
            hmap_put(hub_users, ui->nick, ui);
            free (conv_nick);
            if ((hub_extensions & HUB_EXT_NOGETINFO) == 0) {
                if (!hub_putf("$GetINFO %s %s|", buf+7, hub_my_nick))
                    goto hub_handle_command_cleanup;
                ui->info_quered = true;
            }
        }
    }
    else if (len >= 8 && strncmp(buf, "$MyINFO ", 8) == 0) {
        DCUserInfo *ui;
        char *token;
        uint32_t len;
        char* conv_buf;
        char *work_buf;

        buf += 8;
        work_buf = conv_buf = hub_to_main_string(buf);
        token = strsep(&work_buf, " ");
        if (strcmp(token, "$ALL") != 0) {
            warn(_("Invalid $MyINFO message: Missing $ALL parameter, ignoring\n"));
	        free(conv_buf);
	        goto hub_handle_command_cleanup;
        }

        token = strsep(&work_buf, " ");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing nick parameter, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }
        ui = hmap_get(hub_users, token);
        if (ui == NULL) {
            /*
             * if the full buf has not been converted from hub to local charset,
             * we should try to convert nick only
             */
            /*
            char *conv_nick = hub_to_main_string(token);
            if ((ui = hmap_get(hub_users, conv_nick)) == NULL) {
            */
    	        ui = user_info_new(token);
                ui->info_quered = true;
                hmap_put(hub_users, ui->nick, ui);
            /*
            }
            free(conv_nick);
            */
        }

    	token = strsep(&work_buf, "$");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing description parameter, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }
        free(ui->description);
        ui->description = xstrdup(token);

    	token = strsep(&work_buf, "$");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing description separator, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }

    	token = strsep(&work_buf, "$");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing connection speed, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }
        len = strlen(token);
        free(ui->speed);
        if (len == 0) {
            ui->speed = xstrdup("");
            ui->level = 0; /* XXX: or 1? acceptable level? */
        } else {
            ui->speed = xstrndup(token, len-1);
            ui->level = token[len-1];
        }

    	token = strsep(&work_buf, "$");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing e-mail address, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }
        free(ui->email);
        ui->email = xstrdup(token);

    	token = strsep(&work_buf, "$");
        if (token == NULL) {
            warn(_("Invalid $MyINFO message: Missing share size, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }
        if (!parse_uint64(token, &ui->share_size)) {
            warn(_("Invalid $MyINFO message: Invalid share size, ignoring\n"));
            free(conv_buf);
            goto hub_handle_command_cleanup;
        }

        if (ui->active_state == DC_ACTIVE_RECEIVED_PASSIVE
            || ui->active_state == DC_ACTIVE_KNOWN_ACTIVE)
            ui->active_state = DC_ACTIVE_UNKNOWN;

        /* XXX: Now that user's active_state may have changed, try queue again? */
        free(conv_buf);
    }
    else if (strcmp(buf, "$HubIsFull") == 0) {
        warn(_("Hub is full.\n"));
        /* DC++ does not disconnect immediately. So I guess we won't either. */
        /* Maybe we should be expecting an "hub follow" message? */
        /* hub_disconnect(); */
    }
    else if (len >= 3 && (buf[0] == '<' || strncmp(buf, " * ", 3) == 0)) {
        char *head;
        char *tail;
        char *msg;
        bool first = true;
        /*
        int scrwidth;
        size_t firstlen;
        size_t otherlen;

        screen_get_size(NULL, &scrwidth);
        firstlen = scrwidth - strlen(_("Public:"));
        otherlen = scrwidth - strlen(_(" | "));
        */

        msg = prepare_chat_string_for_display(buf);

        for (head = msg; (tail = strchr(head, '\n')) != NULL; head = tail+1) {
            /*PtrV *wrapped;*/

        if (tail[-1] == '\r') /* end > buf here, buf[0] == '<' or ' ' */
            tail[-1] = '\0';
        else
            tail[0] = '\0';

            /*wrapped = wordwrap(quotearg(buf), first ? firstlen : otherlen, otherlen);
            for (c = 0; c < wrapped->cur; c++)
            flag_putf(DC_DF_PUBLIC_CHAT, first ? _("Public: %s\n") : _(" | %s\n"), );
            ptrv_foreach(wrapped, free);
            ptrv_free(wrapped);*/
            flag_putf(DC_DF_PUBLIC_CHAT, first ? _("Public: %s\n") : _(" | %s\n"), quotearg(head));
            first = false;
        }
        flag_putf(DC_DF_PUBLIC_CHAT, first ? _("Public: %s\n") : _(" | %s\n"), quotearg(head));
        free(msg);
    }
    else if (len >= 5 && strncmp(buf, "$To: ", 5) == 0) {
        char *msg;
        char *tail;
        char *frm;
        char *head;
        bool first = true;

        msg = strchr(buf+5, '$');
        if (msg == NULL) {
            warn(_("Invalid $To message: Missing text separator, ignoring\n"));
            goto hub_handle_command_cleanup;
        }
        *msg = '\0';
        msg++;

        /* FIXME: WTF is this? Remove multiple "From: "? Why!? */
        frm = buf+5;
        while ((tail = strstr(msg, "From: ")) != NULL && tail < msg)
	        frm = tail+6;

        msg = prepare_chat_string_for_display(msg);
        frm = prepare_chat_string_for_display(frm);
        for (head = msg; (tail = strchr(head, '\n')) != NULL; head = tail+1) {
            if (tail[-1] == '\r') /* tail > buf here because head[0] == '<' or ' ' */
                tail[-1] = '\0';
            else
                tail[0] = '\0';
            if (first) {
                screen_putf(_("Private: [%s] %s\n"), quotearg_n(0, frm), quotearg_n(1, head));
                first = false;
            } else {
                screen_putf(_(" | %s\n"), quotearg(head));
            }
        }
        if (first) {
            screen_putf(_("Private: [%s] %s\n"), quotearg_n(0, frm), quotearg_n(1, head));
        } else {
            screen_putf(_(" | %s\n"), quotearg(head));
        }
        free(msg);
        free(frm);
    }
    else if (len >= 13 && strncmp(buf, "$ConnectToMe ", 13) == 0) {
        struct sockaddr_in addr;

        buf += 13;
        if (strsep(&buf, " ") == NULL) {
            warn(_("Invalid $ConnectToMe message: Missing or invalid nick\n"));
            goto hub_handle_command_cleanup;
        }
        if (!parse_ip_and_port(buf, &addr, 0)) {
            warn(_("Invalid $ConnectToMe message: Invalid address specification.\n"));
            goto hub_handle_command_cleanup;
        }

        flag_putf(DC_DF_CONNECTIONS, _("Connecting to user on %s\n"), sockaddr_in_str(&addr));
        user_connection_new(&addr, -1);
    }
    else if (len >= 16 && strncmp(buf, "$RevConnectToMe ", 16) == 0) {
        char *nick;
        char *local_nick;
        DCUserInfo *ui;

        nick = strtok(buf+16, " ");
        if (nick == NULL) {
            warn(_("Invalid $RevConnectToMe message: Missing nick parameter\n"));
            goto hub_handle_command_cleanup;
        }
    	if (strcmp(nick, hub_my_nick) == 0) {
            warn(_("Invalid $RevConnectToMe message: Remote nick is our nick\n"));
            goto hub_handle_command_cleanup;
        }
        local_nick = hub_to_main_string(nick);
        ui = hmap_get(hub_users, local_nick);
        if (ui == NULL) {
            warn(_("Invalid $RevConnectToMe message: Unknown user %s, ignoring\n"), quotearg(local_nick));
            free(local_nick);
            goto hub_handle_command_cleanup;
        }
        free(local_nick);

        if (ui->conn_count >= DC_USER_MAX_CONN) {
            warn(_("No more connections to user %s allowed.\n"), quotearg(ui->nick));
            goto hub_handle_command_cleanup;
        }

        if (!is_active) {
            if (ui->active_state == DC_ACTIVE_SENT_PASSIVE) {
                warn(_("User %s is also passive. Cannot establish connection.\n"), quotearg(ui->nick));
            /* We could set this to DC_ACTIVE_UNKNOWN too. This would mean
             * we would keep sending RevConnectToMe next time the download
             * queue is modified or some timer expired and told us to retry
             * download. The remote would then send back RevConnectToMe
             * and this would happen again. This way we would try again -
             * maybe remote has become active since last time we checked?
             * But no - DC++ only replies with RevConnectToMe to our first
             * RevConnectToMe. After that it ignores them all.
             */
                ui->active_state = DC_ACTIVE_RECEIVED_PASSIVE;
                if (hmap_remove(pending_userinfo, ui->nick) != NULL)
                    ui->refcount--;
                goto hub_handle_command_cleanup;
            }
            if (ui->active_state != DC_ACTIVE_RECEIVED_PASSIVE) {
                /* Inform remote that we are also passive. */
                if (!hub_putf("$RevConnectToMe %s %s|", hub_my_nick, nick))
                    goto hub_handle_command_cleanup;
                }
        }
        ui->active_state = DC_ACTIVE_RECEIVED_PASSIVE;

        if (!hub_connect_user(ui))
            goto hub_handle_command_cleanup;
    }
    else if ( (len >= 10 && strncmp(buf, "$NickList ", 10) == 0)
	    || (len >= 8 && strncmp(buf, "$OpList ", 8) == 0) ) {
        char *nick;
        char *end;
        int oplist;
        char *conv_nick;

        if ( strncmp(buf, "$NickList ", 10) == 0) {
	        nick = buf + 10;
	        oplist = 0;
        }else {
	        nick = buf + 8;
	        oplist = 1;
        }

        for (; (end = strstr(nick, "$$")) != NULL; nick = end+2) {
            DCUserInfo *ui;
            *end = '\0';

            conv_nick = hub_to_main_string(nick);

            ui = hmap_get(hub_users, conv_nick);
            if (ui == NULL) {
                ui = user_info_new(conv_nick);
                hmap_put(hub_users, ui->nick, ui);
            }

            free(conv_nick);

            if (!ui->info_quered && (hub_extensions & HUB_EXT_NOGETINFO) == 0) {
                if (!hub_putf("$GetINFO %s %s|", nick, hub_my_nick))  {
	                goto hub_handle_command_cleanup;
            }
                ui->info_quered = true;
            }

            if (oplist)
                ui->is_operator = true;
        }
    }
    else if (len >= 6 && strncmp(buf, "$Quit ", 6) == 0) {
        DCUserInfo *ui;
        char *conv_nick = hub_to_main_string(buf+6);

    	flag_putf(DC_DF_JOIN_PART, "User %s quits.\n", quotearg(conv_nick));
        ui = hmap_remove(hub_users, conv_nick);
        if (ui == NULL) {
            /* Some hubs print quit messages for users that never joined,
             * so print this as debug only.
             */
            flag_putf(DC_DF_DEBUG, _("Invalid $Quit message: Unknown user %s.\n"), quotearg(conv_nick));
        } else
            user_info_free(ui);
        free(conv_nick);
    }
    else if (len >= 8 && strncmp(buf, "$Search ", 8) == 0) {
    	char *source;
        int parse_result = 0;
	    DCSearchSelection sel;
        sel.patterns = NULL;

    	buf += 8;
	    source = strsep(&buf, " ");
	    if (source == NULL) {
	        warn(_("Invalid $Search message: Missing source specification.\n"));
            goto hub_handle_command_cleanup;
	    }
	    /* charset handling is in parse_search_selection */
        parse_result = parse_search_selection(buf, &sel);
	    if (parse_result != 1) {
            if (sel.patterns != NULL) {
                int i = 0;
                for (i = 0; i < sel.patterncount; i++) {
                    search_string_free(sel.patterns+i);
                }
                free(sel.patterns);
            }
            if (parse_result == 0)
	            warn(_("Invalid $Search message: %s: Invalid search specification.\n"), buf);
            goto hub_handle_command_cleanup;
	    }
	    if (strncmp(source, "Hub:", 4) == 0) {
            DCUserInfo *ui;
            char *conv_nick = hub_to_main_string(source+4);
            ui = hmap_get(hub_users, conv_nick);

	        if (ui == NULL) {
                warn(_("Invalid $Search message: Unknown user %s.\n"), quotearg(conv_nick));
                if (sel.patterns != NULL) {
                    int i = 0;
                    for (i = 0; i < sel.patterncount; i++) {
                        search_string_free(sel.patterns+i);
                    }
                    free(sel.patterns);
                }
                free(conv_nick);
                goto hub_handle_command_cleanup;
	        }
            free(conv_nick);

            if (strcmp(ui->nick, my_nick) == 0) {
                if (sel.patterns != NULL) {
                    int i = 0;
                    for (i = 0; i < sel.patterncount; i++) {
                        search_string_free(sel.patterns+i);
                    }
                    free(sel.patterns);
                }
                goto hub_handle_command_cleanup;
            }
	        perform_inbound_search(&sel, ui, NULL);
            if (sel.patterns != NULL) {
                int i = 0;
                for (i = 0; i < sel.patterncount; i++) {
                    search_string_free(sel.patterns+i);
                }
                free(sel.patterns);
            }
	    } else {
            struct sockaddr_in addr;
    	    if (!parse_ip_and_port(source, &addr, DC_CLIENT_UDP_PORT)) {
		        warn(_("Invalid $Search message: Invalid address specification.\n"));
                if (sel.patterns != NULL) {
                    int i = 0;
                    for (i = 0; i < sel.patterncount; i++) {
                        search_string_free(sel.patterns+i);
                    }
                    free(sel.patterns);
                }
                goto hub_handle_command_cleanup;
	        }
            if (local_addr.sin_addr.s_addr == addr.sin_addr.s_addr && listen_port == ntohs(addr.sin_port)) {
                if (sel.patterns != NULL) {
                    int i = 0;
                    for (i = 0; i < sel.patterncount; i++) {
                        search_string_free(sel.patterns+i);
                    }
                    free(sel.patterns);
                }
                goto hub_handle_command_cleanup;
            }
	        perform_inbound_search(&sel, NULL, &addr);
            if (sel.patterns != NULL) {
                int i = 0;
                for (i = 0; i < sel.patterncount; i++) {
                    search_string_free(sel.patterns+i);
                }
                free(sel.patterns);
            }
	    }
    } else if (len >= 4 && strncmp(buf, "$SR ", 4) == 0) {
    	handle_search_result(buf, len);
    } else if (len == 0) {
    	/* Ignore empty commands. */
    }
hub_handle_command_cleanup:
    free(hub_my_nick);
}

/* This function is called by handle_connection_fd_event when there is input
 * available on the connection socket.
 */
void
hub_input_available(void)
{
    int start = 0;
    int c;
    int res;

    res = byteq_read(hub_recvq, hub_socket);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
    	warn_socket_error(res, false, _("hub"));
        hub_disconnect();
        return;
    }

    for (c = hub_recvq_last; c < hub_recvq->cur; c++) {
        if (hub_recvq->buf[c] == '|') {
            /* Got a complete command. */
	    if (c - start > 0)
	    	dump_command(_("<--"), hub_recvq->buf + start, c - start + 1);
            hub_recvq->buf[c] = '\0'; /* Just to be on the safe side... */
            hub_handle_command(hub_recvq->buf + start, c - start);
            start = c+1;
	    if (hub_socket < 0)
    	    	return;
        }
    }

    if (start != 0)
        byteq_remove(hub_recvq, start);

    hub_recvq_last = hub_recvq->cur;

    update_hub_activity();
}

void
hub_now_writable(void)
{
    if (hub_state == DC_HUB_CONNECT) {
        int error;
        socklen_t size = sizeof(error);
        socklen_t addr_len;

        if (getsockopt(hub_socket, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            warn(_("Cannot get error status - %s\n"), errstr);
            hub_disconnect();
            return;
        }
        if (error != 0) { /* The connect call on the socket failed. */
            warn(_("Cannot connect - %s\n"), strerror(error) /* not errno! */);
            /* XXX: need to know hub address to put in error message */
            hub_disconnect();
            return;
        }

        addr_len = sizeof(local_addr);
        if (getsockname(hub_socket, (struct sockaddr *) &local_addr, &addr_len) < 0) {
            warn(_("Cannot get socket address - %s\n"), errstr);
            hub_disconnect();
            return;
        }
        if (force_listen_addr.s_addr != INADDR_NONE)
            local_addr.sin_addr = force_listen_addr;

    	screen_putf(_("Connected to hub from %s.\n"), sockaddr_in_str(&local_addr));
        update_hub_activity();

    	FD_CLR(hub_socket, &write_fds);
        FD_SET(hub_socket, &read_fds);
        hub_state = DC_HUB_LOCK;
    } else {
    	int res;
    
        if (hub_sendq->cur > 0) {
            res = byteq_write(hub_sendq, hub_socket);
            if (res == 0 || (res < 0 && errno != EAGAIN)) {
    	    	warn_socket_error(res, true, _("hub"));
                hub_disconnect();
                return;
            }
        }
        if (hub_sendq->cur == 0)
            FD_CLR(hub_socket, &write_fds);
    }
}

/* This function tries to make a connection to a user, or ask them to
 * connect to us.
 * 
 * Before calling this function, make sure we are not connected to the
 * user already.
 *
 * This function makes sure an unanswerred (Rev)ConnectToMe hasn't been
 * sent previously.
 *
 * This function is the only place that is allowed to send $ConnectToMe.
 * $RevConnectToMe may be set by one other place (when $RevConnectToMe
 * was received and we did not previously send $RevConnectToMe).
 */
bool
hub_connect_user(DCUserInfo *ui)
{
    char *hub_my_nick;
    char *hub_ui_nick;
    bool connect = false;

    hub_my_nick = main_to_hub_string(my_nick);
    hub_ui_nick = main_to_hub_string(ui->nick);

    if (is_active) {
    	if (ui->active_state == DC_ACTIVE_SENT_ACTIVE) {
    	    warn(_("ConnectToMe already sent to user %s. Waiting.\n"), ui->nick);
	        connect =  true;
	        goto cleanup;
        }
    	if (!hub_putf("$ConnectToMe %s %s:%u|", hub_ui_nick, inet_ntoa(local_addr.sin_addr), listen_port))
	        goto cleanup;
        ui->active_state = DC_ACTIVE_SENT_ACTIVE;
    } else {
    	if (ui->active_state == DC_ACTIVE_SENT_PASSIVE) {
    	    warn(_("RevConnectToMe already sent to user %s. Waiting.\n"), quotearg(ui->nick));
            connect =  true;
            goto cleanup;
        }
    	if (ui->active_state == DC_ACTIVE_RECEIVED_PASSIVE) {
    	    warn(_("User %s is also passive. Cannot communicate.\n"), quotearg(ui->nick));
	        connect =  true;
	        goto cleanup;
        }
    	if (!hub_putf("$RevConnectToMe %s %s|", hub_my_nick, hub_ui_nick)) {
	        goto cleanup;
        }
   	    ui->active_state = DC_ACTIVE_SENT_PASSIVE;
    }
    /* hmap_put returns the old value */
    if (hmap_put(pending_userinfo, ui->nick, ui) == NULL)
    	ui->refcount++;
    connect = true;
cleanup:
    free(hub_ui_nick);
    free(hub_my_nick);
    return connect;
}

void hub_reload_users()
{
    if (hub_putf("$GetNickList|")) {
        if (hub_users != NULL) {
            hmap_foreach_value(hub_users, user_info_free);
            hmap_clear(hub_users);
        }
    }
}
