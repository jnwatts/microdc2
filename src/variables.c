/* variables.c - Setting variables
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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <iconv.h>
#include <langinfo.h>		/* POSIX (XSI) */
#include "xalloc.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "strnlen.h"		/* Gnulib */
#include "iconvme.h"
#include "xstrndup.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/strleftcmp.h"
#include "common/intutil.h"
#include "common/strbuf.h"
#include "common/quoting.h"
#include "common/comparison.h"
#include "common/bksearch.h"
#include "microdc.h"

/* TRANSLATORS: This comma-separated list specifies
 * all strings which are considered negative boolean
 * values for variables such as `active'.
 * Retain the default English strings and add the
 * localized ones.
 */
#define NEGATIVE_BOOL_STRINGS _("0,off,no,false")
/* TRANSLATORS: This comma-separated list specifies
 * all strings which are considered positive boolean
 * values for variables such as `active'.
 * Retain the default English strings and add the
 * localized ones.
 */
#define POSITIVE_BOOL_STRINGS _("1,on,yes,true")

static void var_set_hub_charset(DCVariable *var, int argc, char **argv);
static void var_set_fs_charset(DCVariable *var, int argc, char **argv);
static void var_set_log_charset(DCVariable *var, int argc, char **argv);
static void var_set_nick(DCVariable *var, int argc, char **argv);
static void var_set_password(DCVariable *var, int argc, char **argv);
static void var_set_description(DCVariable *var, int argc, char **argv);
static void var_set_tag(DCVariable *var, int argc, char **argv);
static void var_set_speed(DCVariable *var, int argc, char **argv);
static void var_set_email(DCVariable *var, int argc, char **argv);
static void var_set_download_dir(DCVariable *var, int argc, char **argv);
static void var_set_listing_dir(DCVariable *var, int argc, char **argv);
static void var_set_slots(DCVariable *var, int argc, char **argv);
static char *var_get_slots(DCVariable *var);
static char *var_get_string(DCVariable *var);
static char *var_get_bool(DCVariable *var);
static void var_set_active(DCVariable *var, int argc, char **argv);
static void var_set_auto_reconnect(DCVariable *var, int argc, char **argv);
static char *var_get_listen_addr(DCVariable *var);
static void var_set_listen_addr(DCVariable *var, int argc, char **argv);
static char *var_get_listen_port(DCVariable *var);
static void var_set_listen_port(DCVariable *var, int argc, char **argv);
static char *var_get_display_flags(DCVariable *var);
static void var_set_display_flags(DCVariable *var, int argc, char **argv);
static void var_set_log_file(DCVariable *var, int argc, char **argv);
static char *var_get_time(DCVariable *var);
static void var_set_filelist_refresh_interval(DCVariable *var, int argc, char **argv);

static void speed_completion_generator(DCCompletionInfo *ci);
static void bool_completion_generator(DCCompletionInfo *ci);
static void variable_completion_generator(DCCompletionInfo *ci);
static void display_completion_generator(DCCompletionInfo *ci);
static void charset_completion_generator(DCCompletionInfo *ci);

typedef struct {
    DCDisplayFlag flag;
    const char *name;
} DCDisplayFlagDetails;

/* settings/variables */
uint32_t minislot_count = 3;
uint32_t minislot_size = (1 << 16);
int used_mini_slots = 0;
int used_ul_slots = 0;
int used_dl_slots = 0;
uint32_t my_ul_slots;
char *my_nick;
char *my_tag;
char *my_description;
char *my_speed;
char *my_email;
char *share_dir = NULL;
char *download_dir;
char *listing_dir;
char *my_password;
bool is_active;
bool auto_reconnect = 0;
uint64_t my_share_size = 0;
uint32_t display_flags = ~(DC_DF_DEBUG); /* All flags except debug set */
uint32_t log_flags = ~(DC_DF_DEBUG);
struct in_addr force_listen_addr = { INADDR_NONE };

/* This list must be sorted according to strcmp. */
DCDisplayFlagDetails display_flag_details[] =  {
    { DC_DF_CONNECTIONS,    "connections" },
    { DC_DF_DEBUG,          "debug" },
    { DC_DF_DOWNLOAD,       "download" },
    { DC_DF_JOIN_PART,      "joinpart" },
    { DC_DF_PUBLIC_CHAT,    "publicchat" },
    { DC_DF_SEARCH_RESULTS, "searchresults" },
    { DC_DF_UPLOAD,         "upload" },
};
static const int display_flag_count = sizeof(display_flag_details)/sizeof(*display_flag_details);

static char *speeds[] = {
    "28.8Kbps",
    "33.6Kbps",
    "56Kbps",
    "Cable",
    "DSL",
    "ISDN",
    "LAN(T1)",
    "LAN(T3)",
    "Modem",
    "Satellite",
};
static const int speeds_count = sizeof(speeds)/sizeof(*speeds);

/* This structure must be sorted by variable name, according to strcmp. */
static DCVariable variables[] = {
    {
      "active",
      var_get_bool, var_set_active, &is_active,
      bool_completion_generator,
      NULL,
      "Enable if listening for remote connections"
    },
    {
      "auto_reconnect",
      var_get_bool, var_set_auto_reconnect, &auto_reconnect,
      bool_completion_generator,
      NULL,
      "Enable automatic reconnect to the last connected hub"
    },
    {
      "description",
      var_get_string, var_set_description, &my_description,
      NULL,
      NULL,
      "This is the description which is visible to other users of the hub."
    },
    {
      "display",
      var_get_display_flags, var_set_display_flags, &display_flags,
      display_completion_generator,
      NULL,
      "Types of messages to display on screen"
    },
    {
      "downloaddir",
      var_get_string, var_set_download_dir, &download_dir,
      local_dir_completion_generator,
      NULL,
      "Directory which files are downloaded to"
    },
    {
      "email",
      var_get_string, var_set_email, &my_email,
      NULL,
      NULL,
      "The e-mail visible to other users of the hub"
    },
    {
      "filelist_refresh_interval",
      var_get_time, var_set_filelist_refresh_interval, &filelist_refresh_timeout,
      NULL,
      NULL,
      "Local filelist refresh interval (in seconds)"
    },
    {
      "filesystem_charset",
      var_get_string, var_set_fs_charset, &fs_charset,
      charset_completion_generator,
      NULL,
      "Local filesystem charset (if it differs from local charset)"
    },
    {
      "hub_charset",
      var_get_string, var_set_hub_charset, &hub_charset,
      charset_completion_generator,
      NULL,
      "Character set used for chat on the hub"
    },
    {
      "listenaddr",
      var_get_listen_addr, var_set_listen_addr, &force_listen_addr,
      NULL,
      NULL,
      "Address to send to clients"
    },
    {
      "listenport",
      var_get_listen_port, var_set_listen_port, &listen_port,
      NULL,
      NULL,
      "Port to listen on for connections"
    },
    {
      "listingdir",
      var_get_string, var_set_listing_dir, &listing_dir,
      local_dir_completion_generator,
      NULL,
      "Directory where file listings are kept"
    },
    {
      "log",
      var_get_display_flags, var_set_display_flags, &log_flags,
      display_completion_generator,
      NULL,
      "Types of messages to log (if logfile set)"
    },
    {
      "log_charset",
      var_get_string, var_set_log_charset, &log_charset,
      charset_completion_generator,
      NULL,
      "Log charset (if it differs from local charset)"
    },
    {
      "logfile",
      var_get_string, var_set_log_file, &log_filename,
      local_path_completion_generator,
      NULL,
      "File to log screen messages to (will be appeneded)"
    },
    {
      "nick",
      var_get_string, var_set_nick, &my_nick,
      NULL,
      NULL,
      "This is the desired (but not necessarily the current) nick name."
    },
    {
      "password",
      var_get_string, var_set_password, &my_password,
      NULL,
      NULL,
      "The optional password to pass to the hub."
    },
/*
    {
      "sharedir",
      var_get_string, var_set_share_dir, &share_dir,
      local_path_completion_generator,
      NULL,
      "Directory containing files to share (or a single file to share)"
    },
*/
    {
      "slots",
      var_get_slots, var_set_slots, &my_ul_slots,
      NULL,
      NULL,
      "Number of open upload slots"
    },
    {
      "speed",
      var_get_string, var_set_speed, &my_speed,
      speed_completion_generator,
      NULL,
      "The speed visible to other users of the hub"
    },
    {
      "tag",
      var_get_string, var_set_tag, &my_tag,
      NULL,
      NULL,
      "The user agent tag the hub uses to detect features"
    },
};
static const int variables_count = sizeof(variables)/sizeof(*variables);

static DCVariable *
find_variable(const char *name)
{
    return (DCVariable *) bksearch(name, variables, variables_count,
                                   sizeof(DCVariable),
	                           offsetof(DCVariable, name),
		                   (comparison_fn_t) strcmp);
}

static void
charset_completion_generator(DCCompletionInfo *ci)
{
    /* FIXME: NYI */
    /*run the command  'iconv --list' to get a list of completion alternatives
    fork();
    exec('iconv', 'iconv', '--list');*/
}


static void
var_set_hub_charset(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }
    set_hub_charset(argv[1]);

    /* rebuild filelist (filenames should be in hub charset ) */
    update_request_set_hub_charset(argv[1]);
    if (hub_state == DC_HUB_LOGGED_IN)
        hub_reconnect();
}

static void
var_set_fs_charset(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }
    set_fs_charset(argv[1]);
    /* rebuild filelist (read filenames in new charset */
    update_request_set_fs_charset(argv[1]);
}

static void
var_set_log_charset(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }
    set_log_charset(argv[1]);
}

static void
var_set_nick(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
    } else if (argv[1][0] == '\0') {
	warn(_("Nick cannot be empty.\n"));
    } else if (strnlen(argv[1], 36) >= 36) {
	warn(_("Nick is too long - max length is 35 characters.\n"));
    } else if (strpbrk(argv[1], "$| ") != NULL) {
	warn(_("Nick may not contain `$', `|' or space characters.\n"));
    } else {
	free(my_nick);
	my_nick = xstrdup(argv[1]);
    }
}

static void
var_set_description(DCVariable *var, int argc, char **argv)
{
    char *new_value = join_strings(argv+1, argc-1, ' ');

    if (strpbrk(new_value, "$|") != NULL) {
        warn(_("Description may not contain `$' or `|' characters.\n"));
	free(new_value);
    } else if (strnlen(new_value, 36) >= 36) {
        warn(_("Description is too long - max length is 35 characters.\n"));
	free(new_value);
    } else {
	free(my_description);
	my_description = new_value;
    }
    
}

static void
var_set_email(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
    } else if (strpbrk(argv[1], "$|") != NULL) {
        warn(_("E-mail may not contain `$' or `|' characters.\n"));
    } else if (strnlen(argv[1], 36) >= 36) {
        warn(_("E-mail is too long - max length is 35 characters.\n"));
    } else {
        free(my_email);
        my_email = xstrdup(argv[1]);
    }
}

static void
var_set_tag(DCVariable *var, int argc, char **argv)
{
    char *new_value = join_strings(argv+1, argc-1, ' ');

    if (strpbrk(new_value, "$|") != NULL) {
        warn(_("Tag may not contain `$' or `|' characters.\n"));
	free(new_value);
    } else {
        free(my_tag);
        my_tag = new_value;
    }
}

static void
var_set_speed(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
    } else if (strpbrk(argv[1], "$|") != NULL) {
        warn(_("Speed may not contain `$' or `|' characters.\n"));
    } else {
        free(my_speed);
        my_speed = xstrdup(argv[1]);
    }
}

static void
var_set_download_dir(DCVariable *var, int argc, char **argv)
{
    struct stat st;

    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (stat(argv[1], &st) < 0) {
    	screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(argv[1]), errstr);
	return;
    }
    if (!S_ISDIR(st.st_mode)) {
    	screen_putf(_("%s: Not a directory\n"), quotearg(argv[1]));
	return;
    }
    free(download_dir);
    download_dir = xstrdup(argv[1]);
}

static void
var_set_listing_dir(DCVariable *var, int argc, char **argv)
{
    struct stat st;

    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (stat(argv[1], &st) < 0) {
    	screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(argv[1]), errstr);
	return;
    }
    if (!S_ISDIR(st.st_mode)) {
    	screen_putf(_("%s: Not a directory\n"), quotearg(argv[1]));
	return;
    }
    free(listing_dir);
    listing_dir = xstrdup(argv[1]);
    update_request_set_listing_dir(listing_dir);
}

static void
var_set_slots(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (!parse_uint32(argv[1], &my_ul_slots))
    	screen_putf(_("Invalid slot number `%s'\n"), quotearg(argv[1]));

    if (hub_state >= DC_HUB_LOGGED_IN)
        send_my_info();
}

static char *
var_get_slots(DCVariable *var)
{
    return xstrdup(uint32_str(my_ul_slots));
}

/* Improve: return string position or word index in csv where value was found
 * XXX: move to strutil.c or something. Also on gmediaserver!
 */

bool
string_in_csv(const char *csv, char sep, const char *value)
{
    const char *p0;
    const char *p1;
    size_t len;

    len = strlen(value);
    for (p0 = csv; (p1 = strchr(p0, sep)) != NULL; p0 = p1+1) {
        if (p1-p0 == len && strncmp(p0, value, len) == 0)
            return true;                                 
    }
    if (strcmp(p0, value) == 0)
        return true;

    return false;
}

static void
bool_completion_generator(DCCompletionInfo *ci)
{
    const char *p0;
    const char *p1;
    size_t wlen;

    wlen = strlen(ci->word);

    p0 = POSITIVE_BOOL_STRINGS;
    for (; (p1 = strchr(p0, ',')) != NULL; p0 = p1+1) {
        if (p1-p0 >= wlen && memcmp(ci->word, p0, wlen) == 0)
            ptrv_append(ci->results, new_completion_entry(xstrndup(p0, p1-p0), NULL));
    }
    if (strleftcmp(ci->word, p0) == 0)
        ptrv_append(ci->results, new_completion_entry(xstrdup(p0), NULL));

    p0 = NEGATIVE_BOOL_STRINGS;
    for (; (p1 = strchr(p0, ',')) != NULL; p0 = p1+1) {
        if (p1-p0 >= wlen && memcmp(ci->word, p0, wlen) == 0)
            ptrv_append(ci->results, new_completion_entry(xstrndup(p0, p1-p0), NULL));
    }
    if (strleftcmp(ci->word, p0) == 0)
        ptrv_append(ci->results, new_completion_entry(xstrdup(p0), NULL));
    ptrv_sort(ci->results, completion_entry_display_compare);
}

static bool
parse_bool(const char *s, bool *value)
{
    if (string_in_csv(POSITIVE_BOOL_STRINGS, ',', s)) {
	*value = true;
	return true;
    }
    if (string_in_csv(NEGATIVE_BOOL_STRINGS, ',', s)) {
        *value = false;
        return true;
    }
    return false;
}

static void
var_set_active(DCVariable *var, int argc, char **argv)
{
    bool state;
    
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (!parse_bool(argv[1], &state)) {
	screen_putf(_("Specify active as `0', `no', `off', `1', `yes', or `on'.\n"));
	return;/*false*/
    }
    if (!set_active(state, listen_port))
    	screen_putf(_("Active setting not changed.\n"));/*return false;*/
}

static void
var_set_auto_reconnect(DCVariable *var, int argc, char **argv)
{
    bool state;
    
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (!parse_bool(argv[1], &state)) {
	screen_putf(_("Specify value as `0', `no', `off', `1', `yes', or `on'.\n"));
	return;/*false*/
    }
    auto_reconnect = state;
}

static char *
var_get_listen_addr(DCVariable *var)
{
    if (force_listen_addr.s_addr == INADDR_NONE)
        return NULL;
    return xstrdup(in_addr_str(force_listen_addr));
}

static void
var_set_listen_addr(DCVariable *var, int argc, char **argv)
{
    struct sockaddr_in addr;

    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }
    if (argv[1][0] == '\0') {
        force_listen_addr.s_addr = INADDR_NONE;
        screen_putf(_("Removing listening address.\n"));
        return;
    }

    if (!inet_aton(argv[1], &addr.sin_addr)) {
        screen_putf(_("%s: Specify listen address as an IP address\n"), quotearg(argv[1]));
        /* XXX: fix this in the future... */
	/*struct hostent *he;

	screen_putf(_("Looking up IP address for %s\n"), quotearg(argv[1]));
	he = gethostbyname(argv[1]);
	if (he == NULL) {
	    screen_putf(_("%s: Cannot look up address - %s\n"), quotearg(argv[1]), hstrerror(h_errno));
	    return;
	}

	addr.sin_addr = *(struct in_addr *) he->h_addr;*/
    }

    force_listen_addr = addr.sin_addr;
    screen_putf(_("Listening address set to %s.\n"), inet_ntoa(force_listen_addr));
}

static char *
var_get_display_flags(DCVariable *var)
{
    StrBuf *out;
    uint32_t c;
    uint32_t flags;

    flags = *(uint32_t *) var->value;
    out = strbuf_new();
    for (c = 0; c < display_flag_count; c++) {
	if (flags & display_flag_details[c].flag) {
	    if (!strbuf_is_empty(out))
		strbuf_append_char(out, ' ');
	    strbuf_append(out, display_flag_details[c].name);
	}
    }

    return strbuf_free_to_string(out);
}

static uint32_t
find_display_flag_value(const char *name)
{
    DCDisplayFlagDetails *details;
    details = (DCDisplayFlagDetails *) bksearch(name,
            display_flag_details, display_flag_count,
            sizeof(DCDisplayFlagDetails),
            offsetof(DCDisplayFlagDetails, name),
            (comparison_fn_t) strcmp);
    return (details == NULL ? 0 : details->flag);
}

static void
var_set_display_flags(DCVariable *var, int argc, char **argv)
{
    uint32_t add_values = 0;
    uint32_t set_values = 0;
    uint32_t del_values = 0;
    uint32_t c;

    for (c = 1; c < argc; c++) {
	char *arg = argv[c];
	uint32_t *values;
	uint32_t value;

	if (arg[0] == '+') {
	    values = &add_values;
	    arg++;
	} else if (arg[0] == '-') {
	    values = &del_values;
	    arg++;
	} else {
	    values = &set_values;
	}
	if (strcmp(arg, "all") == 0) {
	    value = ~0;
	} else if (strcmp(arg, "default") == 0) {
	    value = ~0;
	} else {
	    value = find_display_flag_value(arg);
	}
	if (value == 0) {
	    screen_putf(_("No flag by the name %s, display flags not changed.\n"), quotearg(arg));
	    return;
	}
	*values |= value;
    }

    if (set_values != 0 && (add_values != 0 || del_values != 0))
	screen_putf(_("Cannot set and add or delete flags at the same time.\n"));

    if (set_values != 0)
	*((uint32_t *) var->value) = set_values;
    *((uint32_t *) var->value) |= add_values;
    *((uint32_t *) var->value) &= ~del_values;
    *((uint32_t *) var->value) |= DC_DF_COMMON;	/* XXX: must always be available, but for logging? */
}

static char *
var_get_listen_port(DCVariable *var)
{
    if (listen_port == 0)
	return NULL; /* random */
    return xstrdup(uint16_str(listen_port));
}

static void
var_set_listen_port(DCVariable *var, int argc, char **argv)
{
    uint16_t port;
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (argv[1][0] == '\0') {
	port = 0;
    } else {
	if (!parse_uint16(argv[1], &port)) {
	    screen_putf(_("Invalid value `%s' for port number.\n"), quotearg(argv[1]));
	    return;
	}
    }
    if (!set_active(is_active, port))
    	screen_putf(_("Active setting not changed.\n"));
}

static void
var_set_log_file(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    set_log_file(argv[1], true);
}

static char *
var_get_string(DCVariable *var)
{
    char *value = *((char **) var->value);
    return value == NULL ? NULL : xstrdup(value);
}

static char *
var_get_bool(DCVariable *var)
{
    bool value = *((bool *) var->value);
    return xstrdup(value ? _("on") : _("off"));    
}

static void
var_set_password(DCVariable *var, int argc, char **argv)
{
    if (argc > 2) {
	warn(_("too many arguments\n"));
	return;
    }
    if (argv[1][0] == '\0') {
	screen_putf(_("Removing current password.\n"));
	free(my_password);
	my_password = NULL;
    } else if (strchr(argv[1], '|') != NULL) {
    	warn(_("Password may not contain `|' characters.\n"));
    } else {
	free(my_password);
	my_password = xstrdup(argv[1]);
    }
}

static char*
var_get_time(DCVariable *var)
{
    time_t value = *((time_t *) var->value);
    return xasprintf("%ld", value);
}

static void
var_set_filelist_refresh_interval(DCVariable *var, int argc, char **argv)
{
    unsigned int interval;
    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }
    if (argv[1][0] == '\0') {
        interval = 0;
    } else {
        if (!parse_uint32(argv[1], &interval)) {
            screen_putf(_("Invalid value `%s' for interval.\n"), quotearg(argv[1]));
            return;
        }
    }
    filelist_refresh_timeout = interval;

    update_request_set_filelist_refresh_timeout(filelist_refresh_timeout);
}

/* The following completers could be improved by doing a modified
 * binsearch for the first match, then stopping immediately when a
 * non-match is found.
 */
void
variable_completion_generator(DCCompletionInfo *ci)
{
    sorted_list_completion_generator(ci->word,ci->results,variables,variables_count,sizeof(DCVariable),offsetof(DCVariable, name));
}

static void
speed_completion_generator(DCCompletionInfo *ci)
{
    int c;

    /* There's not enough speed types to justify a binary search... */
    for (c = 0; c < speeds_count; c++) {
	if (strleftcmp(ci->word, speeds[c]) == 0)
	    ptrv_append(ci->results, new_completion_entry(speeds[c], NULL));
    }
}

static void
display_completion_generator(DCCompletionInfo *ci)
{
    char *word;
    char base_char;
    uint32_t c;
    uint32_t flags;
    char *cmd;

    cmd = get_word_dequoted(ci->line, 1);
    if (strcmp(cmd, "log") == 0) { /* this is safe even when using aliases */
	flags = log_flags;
    } else {
	flags = display_flags;
    }
    free(cmd);

    word = ci->word;
    base_char = (word[0] == '+' || word[0] == '-' ? *word++ : '\0');
    /* XXX: complete `all', `default' */
    for (c = 0; c < display_flag_count; c++) {
        if (strleftcmp(word, display_flag_details[c].name) == 0) {
            DCCompletionEntry *entry;
        
            if (base_char == '+' && (flags & display_flag_details[c].flag) != 0)
                continue;
            if (base_char == '-' && (flags & display_flag_details[c].flag) == 0)
                continue;

            entry = new_completion_entry(NULL, NULL);
            entry->input = xasprintf("%c%s", base_char, display_flag_details[c].name);
            entry->display = xstrdup(display_flag_details[c].name);
	    ptrv_append(ci->results, entry);
        }
    }
}

void
cmd_set(int argc, char **argv)
{
    uint32_t c;
    DCVariable *var;

    if (argc == 1) {
	int max_len = 0;
	int cols;
	char *fmt;
	
	for (c = 0; c < variables_count; c++)
	    max_len = MAX(max_len, strlen(variables[c].name));
	fmt = xasprintf("%%-%ds  %%s\n", max_len);
	screen_get_size(NULL, &cols);
	for (c = 0; c < variables_count; c++) {
	    DCVariable *var = &variables[c];
	    char *value = var->getter(var);

	    screen_putf(fmt, var->name, value == NULL ? "(unset)" : quotearg(value));
	    free(value);
	}
	free(fmt);
	return;
    }

    var = find_variable(argv[1]);
    if (var == NULL) {
	warn("No variable by the name `%s'.\n", quotearg(argv[1]));
	return;
    }

    if (argc <= 2) {
	char *value;
	value = var->getter(var);
	if (value == NULL) {
	    screen_putf(_("No value is set for `%s'.\n"), var->name);
	} else {
	    screen_putf(_("Current value for `%s':\n%s\n"), var->name, quotearg(value));
	}
	return;
    }

    var->setter(var, argc-1, argv+1);
}

void
set_command_completion_selector(DCCompletionInfo *ci)
{
    char *name;
    DCVariable *var;

    /* ci->word_index > 0 is assured by get_completor. */
    if (ci->word_index <= 1) {
	variable_completion_generator(ci);
    } else {
	name = get_word_dequoted(ci->line, 1);
	var = find_variable(name);
	if (var != NULL && var->completor != NULL)
	    var->completor(ci);
	free(name);
    }
}
