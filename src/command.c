/* command.c - Command input and basic commands
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
#include <ctype.h>		/* C89 */
#include <string.h>		/* C89 */
#include <sys/stat.h>		/* POSIX.1: stat */
#include <unistd.h>		/* POSIX.1: fork */
#include <netdb.h>		/* POSIX.1: gethostbyname, h_errno, ?hstrerror? */
#include <arpa/inet.h>		/* BSD 4.3: inet_aton */
#include <dirent.h>		/* ? */
#include <inttypes.h>		/* POSIX.1 (CX): PRI* */
#include "iconvme.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#include "quotearg.h"		/* Gnulib */
#include "human.h"		/* Gnulib */
#include "fnmatch.h"		/* Gnulib/GNU Libc */
#include "quote.h"		/* Gnulib */
#include "stpcpy.h"		/* Gnulib/GNU Libc */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/tmap.h"
#include "common/error.h"
#include "common/strleftcmp.h"
#include "common/intutil.h"
#include "common/minmaxonce.h"
#include "common/strbuf.h"
#include "common/range.h"
#include "common/quoting.h"
#include "common/comparison.h"
#include "common/bksearch.h"
#include "common/swap.h"
#include "common/optparser.h"
#include "microdc.h"

//#define _TRACE
#if defined(_TRACE)
#include <stdio.h>
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

static void cmd_retry(int argc, char **argv);
static void cmd_help(int argc, char **argv);
static void cmd_exit(int argc, char **argv);
static void cmd_say(int argc, char **argv);
static void cmd_msg(int argc, char **argv);
static void cmd_raw(int argc, char **argv);
static void cmd_disconnect(int argc, char **argv);
static void cmd_connect(int argc, char **argv);
static void cmd_grantslot(int argc, char **argv);
static void cmd_browse(int argc, char **argv);
static void cmd_pwd(int argc, char **argv);
static void cmd_find(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cd(int argc, char **argv);
static void cmd_get(int argc, char **argv);
static void cmd_queue(int argc, char **argv);
static void cmd_unqueue(int argc, char **argv);
static void cmd_who(int argc, char **argv);
static void cmd_transfers(int argc, char **argv);
static void cmd_cancel(int argc, char **argv);
static void cmd_search(int argc, char **argv);
static void cmd_status(int argc, char **argv);
static void cmd_results(int argc, char **argv);
static void cmd_unsearch(int argc, char **argv);
static void cmd_alias(int argc, char **argv);
static void cmd_unalias(int argc, char **argv);
static void cmd_shell(int argc, char **argv);
static void cmd_lookup(int argc, char **argv);
static void cmd_share(int argc, char **argv);
static void cmd_unshare(int argc, char **argv);
static void shell_command_completion_selector(DCCompletionInfo *ci);
static void executable_completion_generator(DCCompletionInfo *ci);
static void alias_completion_generator(DCCompletionInfo *ci);
static void command_completion_generator(DCCompletionInfo *ci);
static void alias_command_completion_selector(DCCompletionInfo *ci);

typedef enum {
    DC_CMD_BUILTIN,
    DC_CMD_ALIAS,
} DCCommandType;

typedef struct _DCGetData DCGetData;
typedef struct _DCCommand DCCommand;
typedef struct _DCAliasCommand DCAliasCommand;
typedef struct _DCBuiltinCommand DCBuiltinCommand;

struct _DCGetData {
    DCUserInfo *ui;
    char *base_path;
    uint64_t byte_count;
    uint32_t file_count;
};

struct _DCCommand {
    char *name;
    DCCommandType type;
};

struct _DCAliasCommand {
    DCCommand cmd;
    char *alias_spec;
};

struct _DCBuiltinCommand {
    DCCommand cmd;
    DCBuiltinCommandHandler handler;
    DCCompletorFunction completor;
    const char *usage_msg;
    const char *help_msg;
};

static TMap *commands; /* initialized in command_init */

static void
add_builtin_command(const char *name, DCBuiltinCommandHandler handler, DCCompletorFunction completor, const char *usage_msg, const char *help_msg)
{
    DCBuiltinCommand *builtin = xmalloc(sizeof(DCBuiltinCommand));
    builtin->cmd.name = xstrdup(name); /* XXX: this should be avoidable (pass char *name instead) */
    builtin->cmd.type = DC_CMD_BUILTIN;
    builtin->handler = handler;
    builtin->completor = completor;
    builtin->usage_msg = usage_msg;
    builtin->help_msg = help_msg;
    tmap_put(commands, builtin->cmd.name, builtin);
}

static void
add_alias_command(const char *name, const char *alias_spec)
{
    DCAliasCommand *alias = xmalloc(sizeof(DCAliasCommand));
    alias->cmd.name = xstrdup(name);
    alias->cmd.type = DC_CMD_ALIAS;
    alias->alias_spec = xstrdup(alias_spec);
    tmap_put(commands, alias->cmd.name, alias);
}

static void
free_command(DCCommand *cmd)
{
    if (cmd->type == DC_CMD_ALIAS) {
	DCAliasCommand *alias = (DCAliasCommand *) cmd;
	free(alias->alias_spec);
    }
    free(cmd->name);
    free(cmd);
}

void
command_finish(void)
{
    tmap_foreach_value(commands, free_command);
    tmap_free(commands);
}

void
command_init(void)
{
    commands = tmap_new();
    tmap_set_compare_fn(commands, (comparison_fn_t) strcmp);
    /* Note: The algorithm in cmd_help that displays the help message
     * requires that the help message is terminated by a newline.
     * Otherwise the last line will not be shown.
     */
    add_builtin_command("browse", cmd_browse, user_or_myself_completion_generator,
        _("browse [USER]"),
        _("If USER is specified, queue the file list for that user for download and "
          "start browsing the user's files as soon as the list is downloaded. With no "
          "arguments, stop browsing.\n"));
    add_builtin_command("cancel", cmd_cancel, transfer_completion_generator,
        _("cancel CONNECTION ..."),
        _("Close a user connection. Use the `transfers' command to get a list of "
          "connections.\n"));
    add_builtin_command("cd", cmd_cd, remote_dir_completion_generator,
        _("cd [DIRECTORY]"),
        _("Change directory when browsing another user's files. If DIRECTORY is not "
          "specified, change to the root directory (`/').\n"));
    add_builtin_command("connect", cmd_connect, NULL,
        _("connect HOST[:PORT]"),
        _("Connect to a hub. If PORT is not specified, assume port 411.\n"));
    add_builtin_command("disconnect", cmd_disconnect, NULL,
        _("disconnect"),
        _("Disconnect from the hub.\n"));
    add_builtin_command("exit", cmd_exit, NULL,
        _("exit"),
        _("Quit the program.\n"));
    add_builtin_command("find", cmd_find, remote_path_completion_generator,
        _("find [FILE ...]"),
        _("List files and directories recursively. Assume current directory if FILE is "
          "not specified. Must be browsing a user's files to use this command.\n"));
    add_builtin_command("get", cmd_get, remote_path_completion_generator,
        _("get FILE ..."),
        _("Queue file for download. Must be browsing a user's files to use this "
          "command.\n"));
    add_builtin_command("grantslot", cmd_grantslot, user_completion_generator,
        _("grantslot [USER ...]"),
        _("Grant a download slot for the specified users, or remove granted slot if the "
          "user was already granted one. Without arguments, display a list of users with "
          "granted slots.\n"));
    add_builtin_command("help", cmd_help, command_completion_generator,
        _("help [COMMAND ...]"),
        _("If COMMAND is specified, display help for that command. Otherwise list all "
          "available commands.\n"));
    add_builtin_command("ls", cmd_ls, remote_path_completion_generator,
        _("ls [OPTION...] [FILE...]"),
        _("List files and directories. Assume current directory if FILE is not\n"
          "specified.\n"
          "\n"
          "Options:\n"
          "  -l, --long    use a long listing format\n"));
    add_builtin_command("retry", cmd_retry, user_with_queue_completion_generator,
        _("retry USER ..."),
        _("Try to connect and download files from the specified users.\n"));
    add_builtin_command("msg", cmd_msg, user_completion_generator,
        _("msg USER MESSAGE..."),
        _("Send a private message to USER. Note that characters such as semicolon "
          "(`;'), double quote (`\"') and number sign (`#') in MESSAGE need to be "
          "escaped or quoted. Therefore it is recommended to put MESSAGE in double "
          "quotes.\n"
          "\n"
          "Example:\n"
          "  msg some_user \"hello, how are you?\"\n"));
    add_builtin_command("pwd", cmd_pwd, NULL,
        _("pwd"),
        _("Display user being browsed and current directory.\n"));
    add_builtin_command("queue", cmd_queue, user_with_queue_completion_generator,
        _("queue [USER ...]"),
        _("Display files queued for download from the specified users. Without "
          "arguments, display a list of users we have queued files for.\n"));
    add_builtin_command("raw", cmd_raw, NULL,
        _("raw DATA..."),
        _("Send some raw data to the hub. Note that characters such as semicolon "
          "(`;'), double quote (`\"') and number sign (`#') in DATA need to be "
          "escaped or quoted. Therefore it is recommended to put DATA in double "
          "quotes.\n"));
    add_builtin_command("results", cmd_results, NULL,
        _("results [INDEX ...]"),
        _("If INDEX is specified, display results for the search by that index. "
          "Otherwise, display a list of searches and statistics over those searches.\n"));
    add_builtin_command("say", cmd_say, say_user_completion_generator,
        _("say MESSAGE..."),
        _("Send a public message to users on the hub. Note that characters such as "
          "semicolon (`;'), double quote (`\"') and number sign (`#') in MESSAGE need "
          "to be escaped or quoted. Therefore it is recommended to put MESSAGE in "
          "double quotes.\n"
          "\n"
          "Example:\n"
          "  say \"hi everyone!\"\n"));
    add_builtin_command("search", cmd_search, NULL,
        _("search WORD..."),
        _("Issue a search for the specified search words.\n"));
    add_builtin_command("set", cmd_set, set_command_completion_selector,
        _("set [NAME [VALUE...]]"),
        _("Without arguments, display a list of variables and their current values. With "
          "only NAME argument, display the value of that variable. With NAME and VALUE "
          "arguments, change the value of a variable.\n"));
    add_builtin_command("status", cmd_status, NULL,
        _("status"),
        _("Display status information and some statistics.\n"));
    add_builtin_command("transfers", cmd_transfers, NULL,
        _("transfers"),
        _("Display a list of user connections.\n"));
    add_builtin_command("unqueue", cmd_unqueue, user_with_queue_completion_generator,
        _("unqueue USER [RANGE]"),
        _("Remove all or a range of queued files for USER. If RANGE is not specified, "
          "remove all files from the queue. Use dash (`-') and comma (`,') in RANGE. "
          "Open ranges are accepted (e.g. `1-' or `-2').\n"));
    add_builtin_command("unsearch", cmd_unsearch, NULL,
        _("unsearch INDEX ..."),
        _("Remove a previously issued search and all results for that search.\n"));
    add_builtin_command("who", cmd_who, user_completion_generator,
        _("who [USER ...]"),
        _("If USER is specified, display information on that user. Otherwise, "
          "display a table of users with some user details.\n"));
    add_builtin_command("alias", cmd_alias, alias_command_completion_selector,
        _("alias [NAME[=VALUE] ...]"),
        _("Without arguments, display the list of aliases. With NAME argument, "
          "display what value (command) that alias is set to. With both NAME and "
          "VALUE argument, change alias. Note that VALUE is a single argument - "
          "you need to use quotes for more complex commands.\n"
          "\n"
          "Example:\n"
          "  alias ll \"ls -l\"\n"));
    add_builtin_command("unalias", cmd_unalias, alias_completion_generator,
        _("unalias NAME ..."),
        _("Remove aliases.\n"));
    add_builtin_command("shell", cmd_shell, shell_command_completion_selector,
        _("shell [COMMAND [ARGUMENTS...]]"),
        _("Execute a system command. If no arguments are specified, the current "
          "shell will be started (SHELL environment variable or `/bin/sh' if that "
          "is not set). microdc will continue in the background while the command "
          "is executing.\n"));
    add_builtin_command("lookup", cmd_lookup, NULL,
        _("lookup HOST ..."),
        _("Lookup the IP address of specified hosts.\n"));
    add_builtin_command("share", cmd_share, local_path_completion_generator,
        _("share DIR"),
        _("Add share directory to the processing list\n"));
    add_builtin_command("unshare", cmd_unshare, local_path_completion_generator,
        _("unshare DIR"),
        _("Remove share directory from the processing list\n"));

    add_alias_command("ll", "ls -l");
}

void
default_completion_selector(DCCompletionInfo *ci)
{
    DCCompletionInfo ci2;
    const char *newline;

    if (find_unquoted_leading_char(ci->line, ci->line + ci->we, '#') != NULL)
	return; /* completing in commented out text */

    newline = find_last_unquoted_char(ci->line, ci->line + ci->we, ';');
    if (newline != NULL) {
	ci2.line = find_word_start(newline+1, ci->line + ci->we);
	ci2.ws = ci->ws - (ci2.line - ci->line);
	ci2.we = ci->we - (ci2.line - ci->line);
	ci2.results = ci->results;
	fill_completion_info(&ci2);
    } else {
        ci2 = *ci;
    }

    if (ci2.word_index == 0) {
        command_completion_generator(&ci2);
    } else {
        DCCommand *cmd;
        char *name;

        name = get_word_dequoted(ci2.line, 0);
        cmd = tmap_get(commands, name);
        free(name);
        if (cmd != NULL) {
            if (cmd->type == DC_CMD_BUILTIN) {
                DCBuiltinCommand *builtin = (DCBuiltinCommand *) cmd;
		if (builtin->completor != NULL)
		    builtin->completor(&ci2);
            } else if (cmd->type == DC_CMD_ALIAS) {
                DCAliasCommand *alias = (DCAliasCommand *) cmd;
                int sizemod;
                char *newline;

                newline = xasprintf("%s%s", alias->alias_spec, find_word_end(ci2.line, NULL));
                ci2.line = newline;
                sizemod = strlen(alias->alias_spec) - strlen(cmd->name);
                ci2.ws += sizemod;
                ci2.we += sizemod;
                default_completion_selector(&ci2);
                free(newline);
            }
        }
    }

    if (newline != NULL) {
        free(ci2.word_full);
        free(ci2.word);
    }
}

static void
alias_command_completion_selector(DCCompletionInfo *ci)
{
/* XXX not implemented yet */
#if 0
    char *newline;
    char *newword;
    int newwe;
    int newws;
    bool quoted;

    /*printf("\nOLD: %2d:%2d [%s:%s]\n", ws, we, word, line);*/
    newline = word;
    newwe = strlen(word);
    newws = find_completion_word_start(newline, newwe);
    /*newword = dequote_word(newline+newws, false, NULL);*/
    newword = get_completion_word_dequoted(newline, newwe);
//    printf("NEW: %2d:%2d [%s:%s]\n", newws, newwe, newword, newline);

    quoted = newline[newws] == '"';
    default_completion_selector(newline, newws, newwe, newword, results);
    if (results->cur == 1) {
        DCCompletionEntry *entry = results->buf[0];
        char *match;
        char *match2;

        if (entry->input_single_full_format != NULL && strcmp(newword, entry->input) == 0) {
            match = xasprintf(entry->input_single_full_format, entry->input);
        } else if (entry->input_single_format != NULL) {
            match = xasprintf(entry->input_single_format, entry->input);
        } else {
            match = xasprintf(entry->input_format, entry->input);
        }
        if (entry->input != entry->display)
            free(entry->input);
        match2 = quote_word_full(match, quoted, true, ";", "#", false, true, true, true);
        free(match);
        entry->input = replace_substr(newline, newws, newwe, match2);
//        printf(">>[%s]\n", entry->input);
        free(match2);
        entry->input_format = "%s";
        entry->input_single_format = NULL;
        entry->input_single_full_format = NULL;
        entry->inhibit_quote = true;
    } else if (results->cur > 1) {
        uint32_t c;

        for (c = 0; c < results->cur; c++) {
            DCCompletionEntry *entry = results->buf[c];
            char *match;

            match = quote_word_full(entry->input, quoted, true, ";", "#", false, true, true, true);
            if (entry->input != entry->display)
                free(entry->input);
            entry->input = replace_substr(newline, newws, newwe, match);
            free(match);
        }
    }

    free(newword);
#endif
}

static void
alias_completion_generator(DCCompletionInfo *ci)
{
    TMapIterator it;

    tmap_iterator_partial(commands, &it, ci->word, (comparison_fn_t) strleftcmp);
    while (it.has_next(&it)) {
        DCCommand *cmd = it.next(&it);
        if (cmd->type == DC_CMD_ALIAS)
            ptrv_append(ci->results, new_completion_entry(cmd->name, NULL));
    }
}

static void
command_completion_generator(DCCompletionInfo *ci)
{
    TMapIterator it;

    tmap_iterator_partial(commands, &it, ci->word, (comparison_fn_t) strleftcmp);
    while (it.has_next(&it)) {
        DCCommand *cmd = it.next(&it);
        ptrv_append(ci->results, new_completion_entry(cmd->name, NULL));
    }
}

void
command_execute(const char *line)
{
    for (;;) {
        PtrV *args;
	DCCommand *cmd;
	char *name;

	line = find_word_start(line, NULL);
	if (*line == '\0' || *line == '#')
	    break;
	if (*line == ';') {
	    line++;
	    continue;
	}

	name = get_word_dequoted_termchar(line, 0, ';'); /* won't return NULL */
	cmd = tmap_get(commands, name);
	if (cmd != NULL && cmd->type == DC_CMD_ALIAS) {
	    DCAliasCommand *alias = (DCAliasCommand *) cmd;
	    char *newline;

	    newline = xasprintf("%s%s", alias->alias_spec, find_word_end(line, NULL));
	    free(name);
	    command_execute(newline);
	    free(newline);
	    return;
	}

	/* Process arguments until ";" or "#" or end of line. */
        args = ptrv_new();
	ptrv_append(args, name);
	for (;;) {
	    line = find_word_start(find_word_end_termchar(line, NULL, ';'), NULL);
	    if (*line == '\0' || *line == '#' || *line == ';')
		break;
	    ptrv_append(args, get_word_dequoted_termchar(line, 0, ';'));
	}
	ptrv_append(args, NULL);

	if (cmd == NULL) {
	    warn(_("%s: Unknown command.\n"), quotearg(name));
	} else {
	    DCBuiltinCommand *builtin = (DCBuiltinCommand *) cmd;
	    /*int c;
	    screen_putf("Execute: <%s>", cmd->name);
	    for (c = 0; c < args->cur-1; c++)
		screen_putf(" [%s]", (char *) args->buf[c]);
	    screen_putf("\n");*/
	    builtin->handler(args->cur-1, (char **) args->buf);
	}

	ptrv_foreach(args, free); /* will free(name) as that is args->buf[0] */
	ptrv_free(args);
    }
}

/* This is not exactly the same as bash, I consider it better:
 *
 *  [1] Allow completion of dirs relative to CWD always (bash: only in
 *      last resort, when there are no exefile matches).
 *  [2] When a dir has been completed (dirpath!=NULL, line contains at
 *      least one /), then allow completion of any dirs+exefiles relative
 *      to that dir. 
 *  [3] Only allow completion of exefiles relative to CWD if PATH
 *      contains `.'.
 *
 * Bash completes like [2] and [3]. But bash deals with things differently
 * when it comes to [1]: When PATH does not contain ., then completion of
 * directories in CWD is only used as a last resort if there are no
 * executable matches in other dirs in PATH.
 */
static void
executable_completion_generator(DCCompletionInfo *ci)
{
    char *path;
    char *p1;
    char *p;
    const char *file_part;
    char *dir_part;
    char *conv_word;
    bool path_has_cwd = false;

    conv_word = main_to_fs_string(ci->word);

    get_file_dir_part(conv_word, &dir_part, &file_part);

    path = getenv("PATH");
    if (path == NULL) {
        /*path = xconfstr(_CS_PATH);
        if (path == NULL || *path == '\0')*/
        free(conv_word);
        return;
    } else {
        path = xstrdup(path);
    }

    for (p1 = path; (p = strsep(&p1, ":")) != NULL; ) {
        DIR *dh;

        if (strcmp(p, ".") == 0)
            path_has_cwd = true;
        dh = opendir(p); /* ignore errors */
        if (dh != NULL) {
            struct dirent *ent;
            while ((ent = readdir(dh)) != NULL) { /* ignore errors */
                if (strleftcmp(conv_word, ent->d_name) == 0) {
                    char *full;
                    full = catfiles(p, ent->d_name);
                    if (access(full, X_OK) == 0) { /* ignore errors */
                        struct stat sb;
                        if (stat(full, &sb) >= 0 && S_ISREG(sb.st_mode)) {
                            DCCompletionEntry *ce;
                            char *conv_fname;
                            
                            conv_fname = fs_to_main_string(ent->d_name);
                            ce = new_completion_entry(conv_fname, conv_fname);
			                free(conv_fname);
                            ce->sorting.file_type = DC_TYPE_REG;
                            ptrv_append(ci->results, ce);
                        }
                    }
                    /* XXX: use is_directory instead of that stat above */
                    free(full);
                }
            }
            closedir(dh);
        }
    }

    free(conv_word);

    /* local_fs_completion_generator will sort the results. */
    if (*dir_part != '\0' || path_has_cwd) {
        local_fs_completion_generator(ci,DC_CPL_DIR|DC_CPL_EXE|DC_CPL_DOT);
    } else if (ci->results->cur == 0) {
        local_fs_completion_generator(ci,DC_CPL_DIR|DC_CPL_DOT);
    }

    free(dir_part);
    free(path);
}

static void
shell_command_completion_selector(DCCompletionInfo *ci)
{
    if (ci->word_index == 1) {
        executable_completion_generator(ci);
    } else {
        local_path_completion_generator(ci);
    }
}

static void
cmd_help(int argc, char **argv)
{
    uint32_t c;
    int width;

    screen_get_size(NULL, &width);

    if (argc == 1) {
        TMapIterator it;
        StrBuf *buf;

        buf = strbuf_new();
        for (tmap_iterator(commands, &it); it.has_next(&it); ) {
            DCCommand *cmd = it.next(&it);
            if (cmd->type == DC_CMD_BUILTIN) {
                DCBuiltinCommand *builtin = (DCBuiltinCommand *) cmd;
                if (strlen(builtin->usage_msg) < width/2) {
                    if (strbuf_is_empty(buf)) {
                        strbuf_append(buf, builtin->usage_msg);
                        strbuf_append_char_n(buf, width/2-strbuf_length(buf), ' ');
                    } else {
                        screen_putf("%s%s\n", strbuf_buffer(buf), builtin->usage_msg);
                        strbuf_clear(buf);
                    }
                } else {
                    if (strbuf_is_empty(buf)) {
                        screen_putf("%s\n", builtin->usage_msg);
                    } else {
                        screen_putf("%s\n%s\n", strbuf_buffer(buf), builtin->usage_msg);
                        strbuf_clear(buf);
                    }
                }
            }
        }
        if (!strbuf_is_empty(buf))
            screen_putf("%s\n", strbuf_buffer(buf));
        strbuf_free(buf);
        return;
    }


    for (c = 1; c < argc; c++) {
        DCCommand *cmd;
        cmd = tmap_get(commands, argv[c]);
        if (cmd == NULL) {
            warn("%s: Unknown command.\n", quotearg(argv[c]));
        } else {
            if (cmd->type == DC_CMD_ALIAS) {
                DCAliasCommand *alias = (DCAliasCommand *) cmd;
                screen_putf("%s: aliased to `%s'.\n", quotearg_n(0, cmd->name), quotearg_n(1, alias->alias_spec));
            } else {
                DCBuiltinCommand *builtin = (DCBuiltinCommand *) cmd;
                const char *p0;
                const char *p1;

                screen_putf("%s: %s\n", cmd->name, builtin->usage_msg);
                for (p0 = builtin->help_msg; (p1 = strchr(p0, '\n')) != NULL; p0 = p1+1) {
                    PtrV *msgs;
                    uint32_t d;

                    msgs = wordwrap(p0, p1-p0, width-5-4, width-5-4);
                    for (d = 0; d < msgs->cur; d++)
                        screen_putf("    %s\n", (char *) msgs->buf[d]);
                    if (d == 0)
                        screen_putf("\n");
                    ptrv_free(msgs);
                }
            }
        }
    }
}

static void
cmd_shell(int argc, char **argv)
{
    screen_suspend();
    shell_child = fork();
    if (shell_child < 0) {
	warn(_("Cannot create child process - %s\n"), errstr);
	return;
    }
    if (shell_child == 0) {
        warn_writer = default_warn_writer;
        if (argc <= 1) {
            char *args[2] = { NULL, NULL };
            char *shell;

            shell = getenv("SHELL");
            if (shell != NULL)
                args[0] = shell;
            else
                args[0] = "/bin/sh";
            if (execvp(args[0], args) < 0)
	        die(_("%s: cannot execute - %s\n"), quotearg(args[0]), errstr); /* die OK */
        } else {
            if (execvp(argv[1], argv+1) < 0)
	        die(_("%s: cannot execute - %s\n"), quotearg(argv[1]), errstr); /* die OK */
        }
        exit(EXIT_FAILURE); /* shouldn't get here */
    }
}

static void
cmd_status(int argc, char **argv)
{
    HMapIterator it;
    uint32_t c;
    char sizebuf[LONGEST_HUMAN_READABLE+1];

    switch (hub_state) {
    case DC_HUB_DISCONNECTED:
	screen_putf(_("Hub state: %s\n"), _("Not connected"));
	break;
    case DC_HUB_LOOKUP:
        screen_putf(_("Hub state: %s\n"), _("Looking up IP address"));
        break;
    case DC_HUB_CONNECT:
	screen_putf(_("Hub state: %s\n"), _("Waiting for complete connection"));
	break;
    case DC_HUB_LOCK:
	screen_putf(_("Hub state: %s\n"), _("Waiting for $Lock"));
	break;
    case DC_HUB_HELLO:
	screen_putf(_("Hub state: %s\n"), _("Waiting for $Hello"));
	break;
    case DC_HUB_LOGGED_IN:
	screen_putf(_("Hub state: %s\n"), _("Logged in"));
	break;
    }

    if (hub_state >= DC_HUB_LOGGED_IN) {
        screen_putf(_("Hub users: %s\n"), uint32_str(hmap_size(hub_users)));
    } else {
        screen_putf(_("Hub users: %s\n"), _("(not logged in)"));
    }

    /*
    screen_putf(_("Pending user connections:\n"));
    hmap_iterator(pending_userinfo, &it);
    for (c = 0; it.has_next(&it); c++) {
        DCUserInfo* ui = it.next(&it);
        if (ui != NULL && ui->nick != NULL) {
            screen_putf("  %s\n", quotearg(ui->nick));
        } else {
            screen_putf("  (null)\n");
        }
    }
    if (c == 0)
        screen_putf(_("  (none)\n"));
    */

    c = 0;
    screen_putf(_("Shared directories:\n"));
    if (our_filelist != NULL) {
        hmap_iterator(our_filelist->dir.children, &it);
        while (it.has_next(&it)) {
            DCFileList *node = it.next(&it);
            char* screen_path = fs_to_main_string(node->dir.real_path);
            screen_putf(_("  %s - %" PRIu64" %s (%s)\n"),
                        screen_path,
                        node->size,
                        ngettext("byte", "bytes", node->size),
                        human_readable(node->size, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1));
            free(screen_path);
            c++;
        }
    }
    if (c == 0) {
        screen_putf(_("  (none)\n"));
    }

    screen_putf(_("Total share size: %" PRIu64" %s (%s)\n"),
                my_share_size,
                ngettext("byte", "bytes", my_share_size),
                human_readable(my_share_size, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1));
    screen_putf(_("FileList was updated last time on %s\n"), ctime(&our_filelist_last_update));

    screen_putf(_("Bytes received: %" PRIu64 " %s (%s)\n"),
                bytes_received,
                ngettext("byte", "bytes", bytes_received),
                human_readable(bytes_received, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1));
    screen_putf(_("Bytes sent: %" PRIu64 " %s (%s)\n"),
                bytes_sent,
                ngettext("byte", "bytes", bytes_sent),
                human_readable(bytes_sent, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1));

    if (update_status != NULL) {
        screen_putf(_("%s\n"), update_status);
    }
}

static void
cmd_exit(int argc, char **argv)
{
    /*if (argc > 1) {
	warn(_("%s: too many arguments\n"), argv[0]);
	return;
    }*/
    running = false;
}

static void
cmd_say(int argc, char **argv)
{
    char *t1;
    char *t2;
    char *hub_my_nick;
    char *hub_t2;
    bool utf8 = false;

    if (argc <= 1) {
    	screen_putf(_("Usage: %s MESSAGE..\n"), argv[0]);
	return;
    }
    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    t1 = join_strings(argv+1, argc-1, ' ');

    if (t1 == NULL)
	return;

    t2 = escape_message(t1);
    free(t1);

    if (t2 == NULL)
	return;

    /* Don't print on screen - hub will send us the message back. */
    /*
    hub_my_nick = main_to_hub_string(my_nick);
    hub_t2 = main_to_hub_string(t2);
    */
    hub_my_nick = try_main_to_hub_string(my_nick);
    if (hub_my_nick == NULL) {
        hub_my_nick = main_to_utf8_string(my_nick);
        utf8 = true;
    }
    hub_t2 = try_main_to_hub_string(t2);
    if (hub_t2 == NULL) {
        hub_t2 = main_to_utf8_string(t2);
        utf8 = true;
    }
    hub_putf("<%s>%c%s|", hub_my_nick, (utf8 ? '\xA0' : ' '), hub_t2); /* Ignore error */
    free(hub_my_nick);
    free(hub_t2);
    free(t2);
}

static void
cmd_msg(int argc, char **argv)
{
    char *t1;
    char *t2;
    DCUserInfo *ui;

    if (argc <= 2) {
    	screen_putf(_("Usage: %s USER MESSAGE..\n"), argv[0]);
	return;
    }
    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    ui = hmap_get(hub_users, argv[1]);
    if (ui == NULL) {
    	screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[1]));
	return;
    }


    t1 = join_strings(argv+2, argc-2, ' ');

    if ( t1 == NULL)
	return;

    screen_putf("Private to %s: <%s> %s\n", quotearg_n(0, ui->nick), quotearg_n(1, my_nick), t1); /* XXX: quotearg_n(2, t1)? */

    t2 = escape_message(t1);
    free(t1);

    if (t2 == NULL)
	return;

    char *hub_my_nick = main_to_hub_string(my_nick);
    char *hub_to_nick = main_to_hub_string(ui->nick);
    char *hub_t2 = main_to_hub_string(t2);
    free(t2);
    hub_putf("$To: %s From: %s $<%s> %s|", hub_to_nick, hub_my_nick, hub_my_nick, hub_t2); /* Ignore error */
    free(hub_to_nick);
    free(hub_my_nick);
    free (hub_t2);
}

static void
cmd_raw(int argc, char **argv)
{
    char *msg;
    
    if (argc <= 1) {
    	screen_putf(_("Usage: %s DATA...\n"), argv[0]);
	return;
    }
    if (hub_state < DC_HUB_LOCK) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    msg = join_strings(argv+1, argc-1, ' ');
    screen_putf("Raw to hub: %s\n", msg);
    hub_putf("%s", msg); /* Ignore error */
    free(msg);
}

static void
cmd_connect(int argc, char **argv)
{
    char *portstr;
    uint16_t port;

    if (argc == 1) {
        screen_putf(_("Usage: %s HOST[:PORT]\n"), argv[0]);
        return;
    }
    if (hub_state != DC_HUB_DISCONNECTED) {
        screen_putf(_("Connection in progress, disconnect first.\n"));
        return;
    }

    portstr = strchr(argv[1], ':');
    if (portstr != NULL) {
    	*portstr = '\0';
    	if (!parse_uint16(portstr+1, &port)) {
	    screen_putf(_("Invalid port number %s\n"), quote(portstr+1));
	    return;
	}
    } else {
        port = DC_HUB_TCP_PORT;
    }

    hub_new(argv[1], port);
}

static void
cmd_disconnect(int argc, char **argv)
{
    if (hub_state == DC_HUB_DISCONNECTED) {
        warn(_("Not connected.\n"));
    } else {
        warn(_("Disconnecting from hub.\n"));
        hub_disconnect();
        hub_set_connected(false);
    }
}

static void
cmd_grantslot(int argc, char **argv)
{
    uint32_t c;

    if (hub_state != DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    if (argc == 1) {
        HMapIterator it;

        hmap_iterator(hub_users, &it);
        while (it.has_next(&it)) {
            DCUserInfo *ui = it.next(&it);
            if (ui->slot_granted)
                screen_putf("%s\n", ui->nick);
        }
        return;
    }
    for (c = 1; c < argc; c++) {
        DCUserInfo *ui;

        ui = hmap_get(hub_users, argv[c]);
        if (ui == NULL) {
    	    screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[c]));
    	    return;
        }
        if (ui->slot_granted) {
            screen_putf(_("%s has been granted a slot.\n"), ui->nick);
        } else {
            screen_putf(_("%s is no longer granted a slot.\n"), ui->nick);
        }
        ui->slot_granted = !ui->slot_granted;
    }
}

/* XXX: move queue.c/browse.c? */
void
browse_none(void)
{
    /* Clean up previous browse. */
    if (browse_list != NULL) {
        if (!browsing_myself)
	    filelist_free(browse_list);
        browse_list = NULL;
	free(browse_path);
	browse_path = NULL;
	free(browse_path_previous);
	browse_path_previous = NULL;
    }
    if (browse_user != NULL) {
	user_info_free(browse_user);
	browse_user = NULL;
    }
    browsing_myself = false;
}

/* XXX: move to queue.c */
static int
queued_file_cmp(const char *filename, DCQueuedFile *qf)
{
    return strcmp(filename, qf->filename);
}

void
browse_list_parsed(DCFileList *node, void *data)
{
    char *nick = data;

    /* These checks are to protect from the case when
     * the user is already browsing the user the file
     * list was parsed for.
     */
    if (browse_list == NULL && browse_user != NULL
            && strcmp(nick, browse_user->nick) == 0) {
	browse_list = node;
	browse_path = xstrdup("/");
	browse_path_previous = NULL;
	update_prompt();
	/* browse_list_parsed will never be called when browsing ourselves,
	 * because our filelist is already parsed and always available.
	 */
	screen_putf(_("Now browsing %s.\n"), quotearg(browse_user->nick));
    } else {
	filelist_free(node);
    }

    free(nick);
}

/* XXX: move queue.c/browse.c? */
static void
cmd_browse(int argc, char **argv)
{
    DCUserInfo *ui;
    char *filename = NULL, *xml_filename = NULL, *bzxml_filename = NULL;
    struct stat st;

    if (argc == 1) {
        if (!browsing_myself && browse_user == NULL) {
            screen_putf(_("Not browsing any user.\n"));
            return;
        }
        browse_none();
        update_prompt();
        return;
    }

    if (strcmp(my_nick, argv[1]) == 0) {
        browse_none();
        browse_list = our_filelist;
        browse_path = xstrdup("/");
        browse_path_previous = NULL;
        browse_user = NULL;
        browsing_myself = true;
        update_prompt();
        return;
    }

    ui = hmap_get(hub_users, argv[1]);
    if (ui == NULL) {
        screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[1]));
        return;
    }

    filename = xasprintf("%s/%s.MyList.DcLst", listing_dir, ui->nick);
    xml_filename = xasprintf("%s/%s.files.xml", listing_dir, ui->nick);
    bzxml_filename = xasprintf("%s/%s.files.xml", listing_dir, ui->nick);
    if (stat(filename, &st) >= 0) {
        unlink(filename);
    }
    if (stat(filename, &st) >= 0) {
        unlink(filename);
    }
    if (stat(filename, &st) >= 0) {
        unlink(filename);
    }
    if (stat(filename, &st) < 0 &&
        stat(xml_filename, &st) < 0 &&
        stat(bzxml_filename, &st) < 0) {
        if (errno != ENOENT) {
            screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
            free(filename);
            free(xml_filename);
            free(bzxml_filename);
            return;
        }

        free(filename);
        free(xml_filename);
        free(bzxml_filename);
        if (ptrv_find(ui->download_queue, "/MyList.DcLst", (comparison_fn_t) queued_file_cmp) < 0 &&
            ptrv_find(ui->download_queue, "/files.xml", (comparison_fn_t) queued_file_cmp) < 0 &&
            ptrv_find(ui->download_queue, "/files.xml.bz2", (comparison_fn_t) queued_file_cmp) < 0) {
            DCQueuedFile *queued = xmalloc(sizeof(DCQueuedFile));

            TRACE(("%s:%d: enqueue a new file list to download\n", __FUNCTION__, __LINE__));

            queued->filename = xstrdup("/MyList.DcLst"); /* just to have something here */
            queued->base_path = xstrdup("/");
            queued->flag = DC_TF_LIST;
            queued->status = DC_QS_QUEUED;
            queued->length = UINT64_MAX; /* UINT64_MAX means that the size is unknown */
            ptrv_prepend(ui->download_queue, queued);
        } else {
            TRACE(("%s:%d: there is already filelist in download queue\n", __FUNCTION__, __LINE__));
        }
        if (!has_user_conn(ui, DC_DIR_RECEIVE) && ui->conn_count < DC_USER_MAX_CONN)
            hub_connect_user(ui); /* Ignore errors */
        else
            screen_putf(_("No free connections. Queued file for download.\n"));

        browse_none();
        browse_user = ui;
        browsing_myself = false;
        ui->refcount++;
        update_prompt();
        return;
    }

    browse_none();
    browse_user = ui;
    browsing_myself = false;
    ui->refcount++;
    update_prompt();

    add_parse_request(browse_list_parsed, filename, xstrdup(ui->nick));
    free(filename);
    free(xml_filename);
    free(bzxml_filename);
}

/* XXX: move queue.c/browse.c? */
static void
cmd_pwd(int argc, char **argv)
{
    if (browse_list == NULL) {
	if (browse_user == NULL) {
	    screen_putf(_("Not browsing any user.\n"));
	} else {
	    screen_putf(_("(%s) Waiting for file list.\n"), quotearg(browse_user->nick));
	}
    } else {
	screen_putf(_("(%s) %s\n"),
		quotearg_n(0, browsing_myself ? my_nick : browse_user->nick),
		quotearg_n(1, browse_path)); /* XXX: print as %s:%s instead */
    }
}

/* XXX: move queue.c/browse.c? */
static void
cmd_cd(int argc, char **argv)
{
    if (browse_list == NULL) {
	screen_putf(_("Not browsing any user.\n"));
	return;
    }

    if (argc == 1) {
        free(browse_path_previous);
        browse_path_previous = browse_path;
        browse_path = filelist_get_path(browse_list);
        update_prompt();
    } else if (strcmp(argv[1], "-") == 0) {
	if (browse_path_previous == NULL) {
	    warn(_("No previous path.\n"));
	} else {
	    swap(browse_path, browse_path_previous);
	    update_prompt();
	}
    } else {
        bool quoted = false;
        PtrV *results;
        DCFileList *basenode;
        char *basedir;

        results = ptrv_new();
        if (has_leading_slash(argv[1])) {
            basenode = browse_list;
            basedir = "/";
        } else {
            basenode = filelist_lookup(browse_list, browse_path);
            basedir = "";
        }
        remote_wildcard_expand(argv[1], &quoted, basedir, basenode, results);
        if (results->cur >= 1) {
            char *name = results->buf[0];
            char *fullname;  
            DCFileList *node;

            fullname = apply_cwd(name);
            node = filelist_lookup(browse_list, fullname);
            if (node != NULL) { /* Technically, this shouldn't fail */
                if (node->type != DC_TYPE_DIR) {
                    warn(_("%s: not a directory\n"), quotearg(name));
                } else {
                    free(browse_path_previous);
                    browse_path_previous = browse_path;
                    browse_path = filelist_get_path(node);
                    update_prompt();
                }
            }
            free(fullname);
            ptrv_foreach(results, free);
        }
        ptrv_free(results);
    }

}

/* XXX: move queue.c/browse.c? */
static void
cmd_find(int argc, char **argv)
{
    uint32_t c;

    if (browse_list == NULL) {
	screen_putf(_("Not browsing any user.\n"));
	return;
    }

    if (argc == 1) {
        DCFileList *node;

        node = filelist_lookup(browse_list, browse_path);
        /* At the moment, node cannot be NULL here. */
        if (node != NULL)
            filelist_list_recursively(node, "");
    }
    for (c = 1; c < argc; c++) {
	bool quoted = false;
	PtrV *results;
	DCFileList *basenode;
	char *basedir;
	uint32_t d;

        results = ptrv_new();
        if (has_leading_slash(argv[c])) {
            basenode = browse_list;
            basedir = "/";
        } else {
            basenode = filelist_lookup(browse_list, browse_path);
            basedir = "";
        }
        remote_wildcard_expand(argv[c], &quoted, basedir, basenode, results);
        for (d = 0; d < results->cur; d++) {
            char *name = results->buf[d];
            char *fullname;
            DCFileList *node;

            fullname = apply_cwd(name);
            node = filelist_lookup(browse_list, fullname);
            if (node != NULL) /* Technically, this shouldn't fail */
                filelist_list_recursively(node, name);
            free(fullname);
            free(name);
        }
        if (d == 0)
            screen_putf(_("%s: No such file or directory\n"), quotearg(argv[c]));
        ptrv_free(results);
    }
}

/* XXX: move queue.c/browse.c? */
static void
cmd_ls(int argc, char **argv)
{
    OptDetail long_opts[] = {
	{ "l", OPTP_NO_ARG, 'l' },
	{ "t", OPTP_NO_ARG, 'l' },
	{ NULL },
    };
    OptParser *p;
    int mode = 0;

    p = optparser_new(long_opts, -1, 0);
    optparser_parse(p, argc, argv);
    while (optparser_has_next(p)) {
        switch (optparser_next(p)) {
        case 'l':
            mode |= DC_LS_LONG_MODE;
            break;
        case 't':
            mode |= DC_LS_TTH_MODE;
            break;
        }
    }
    if (optparser_error(p) != NULL) {
        screen_putf("%s: %s\n", argv[0], optparser_error(p));
        optparser_free(p);
        return;
    }
    if (browse_list == NULL) {
        screen_putf(_("Not browsing any user.\n"));
        optparser_free(p);
        return;
    }
    if (!optparser_has_next_arg(p)) {
        DCFileList *node;

        node = filelist_lookup(browse_list, browse_path);
        /* At the moment, node cannot be NULL here. */
        if (node != NULL)
            filelist_list(node, mode);
    }
    while (optparser_has_next_arg(p)) {
        char *arg = optparser_next_arg(p);
        bool quoted = false;
        char *basedir;
        PtrV *results;
        DCFileList *basenode;
        uint32_t d;

        results = ptrv_new();
        if (has_leading_slash(arg)) {
            basenode = browse_list;
            basedir = "/";
        } else {
            basenode = filelist_lookup(browse_list, browse_path);
            basedir = "";
        }
        remote_wildcard_expand(arg, &quoted, basedir, basenode, results);
        for (d = 0; d < results->cur; d++) {
            char *name = results->buf[d];
            char *fullname;  
            DCFileList *node;
                                       
            fullname = apply_cwd(name);
            node = filelist_lookup(browse_list, fullname);
            if (node != NULL) /* Technically, this shouldn't fail */
                filelist_list(node, mode);
            free(fullname);                       
            free(name);
        }
        if (d == 0)
            screen_putf(_("%s: No such file or directory\n"), quotearg(arg));
        ptrv_free(results);
    }
    optparser_free(p);
}

static void
cmd_retry(int argc, char **argv)
{
    uint32_t c;

    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }

    for (c = 1; c < argc; c++) {
        DCUserInfo *ui;

        ui = hmap_get(hub_users, argv[c]);
        if (ui == NULL) {
            screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[c]));
            continue;
        }
        if (!has_user_conn(ui, DC_DIR_RECEIVE) && ui->conn_count < DC_USER_MAX_CONN) {
            hub_connect_user(ui);
        } else {
            screen_putf(_("%s: Already connected to user.\n"), quotearg(ui->nick));
        }
    }
}

/* XXX: move to download.c/queue.c/browse.c? */
static void
cmd_queue(int argc, char **argv)
{
    uint32_t d;

    /*if (argc == 1) {
    	screen_putf(_("Usage: %s USER\n"), argv[0]);
	return;
    }*/
    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    if (argc == 1) {
        HMapIterator it;

        hmap_iterator(hub_users, &it);
        while (it.has_next(&it)) {
            DCUserInfo *ui = it.next(&it);
            if (ui->download_queue->cur > 0)
                screen_putf("%3d %s\n", ui->download_queue->cur, ui->nick);
        }
        return;
    }

    for (d = 1; d < argc; d++) {
        uint32_t c;
        DCUserInfo *ui;

        ui = hmap_get(hub_users, argv[d]);
        if (ui == NULL) {
            screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[d]));
            continue;
        }
        screen_putf("%s:\n", quotearg(ui->nick));
        for (c = 0; c < ui->download_queue->cur; c++) {
            DCQueuedFile *queued = ui->download_queue->buf[c];
            char *status;

            switch (queued->status) {
            case DC_QS_QUEUED:
                status = "queued";
                break;
            case DC_QS_PROCESSING:
                status = "processing";
                break;
            case DC_QS_DONE:
                status = "done";
                break;
            case DC_QS_ERROR:
            default:
                status = "error";
                break;
            }

            screen_putf("%d. (%s) [%s] %s\n",
                    c+1,
                    status,
                    quotearg_n(0, queued->base_path),
                    quotearg_n(1, queued->filename + strlen(queued->base_path)));
        }
    }
}

/* XXX: move to download.c/queue.c/browse.c? */
static void
removed_queued_by_range(uint32_t sp, uint32_t ep, void *userdata)
{
    DCUserInfo *user = userdata;
    uint32_t c;

    for (c = sp-1; c < ep; c++) {
	if (user->download_queue->buf[c] != NULL) {
	    free_queued_file(user->download_queue->buf[c]);
	    user->download_queue->buf[c] = NULL;
	}
    }
}

/* XXX: move to download.c/queue.c/browse.c? */
static void
compact_queue_ptrv(DCUserInfo *user)
{
    int32_t first_free = -1;
    uint32_t c;

    for (c = 0; c < user->download_queue->cur; c++) {
	if (user->download_queue->buf[c] == NULL) {
	    if (first_free == -1)
		first_free = c;
	} else {
	    if (first_free != -1) {
		user->download_queue->buf[first_free] = user->download_queue->buf[c];
		user->download_queue->buf[c] = NULL;
		for (first_free++; first_free < c; first_free++) {
		    if (user->download_queue->buf[first_free] == NULL)
			break;
		}
	    }
	}
    }

    if (first_free != -1)
	user->download_queue->cur = first_free;
}    

/* XXX: move to download.c/queue.c/browse.c? */
static void
cmd_unqueue(int argc, char **argv)
{
    DCUserInfo *user;
    const char *range;

    if (argc == 1) {
    	screen_putf(_("Usage: %s USER [RANGE]\n"), argv[0]);
	return;
    }

    /* XXX: parse each argument, allow RANGE RANGE2 .. */
    if (argc > 2) {
        range = argv[2];
    } else {
	range = "1-";
    }

    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    user = hmap_get(hub_users, argv[1]);
    if (user == NULL) {
        screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[1]));
        return;
    }
    if (!foreach_in_range(range, 1, user->download_queue->cur, NULL, NULL)) {
	screen_putf(_("%s: Invalid range, or index out of range (1-%d)\n"), quotearg(range), user->download_queue->cur);
	return;
    }
    foreach_in_range(range, 1, user->download_queue->cur, removed_queued_by_range, user);

    if (has_user_conn(user, DC_DIR_RECEIVE)) {
        DCUserConn *uc = NULL;
        uint32_t c;

        for (c = 0; c < user->conn_count; c++) {
            if (user->conn[c]->dir == DC_DIR_RECEIVE) {
                uc = user->conn[c];
                break;
            }
        }
        if (uc->queue_pos < user->download_queue->cur) {
            uint32_t rank = 0;

            if (user->download_queue->buf[uc->queue_pos] == NULL) {
                /* Possible actions to take on unqueue of file currently
                 * being downloaded:
                 * - abort download, discard file (keep partial)
                 * - wait until download finishes, and leave as .part
                 * - wait until download finishes, rename and report
                 * - not possible to remove such queued file
                 * The current implementation does (2), which probably
                 * isn't optimal, but works.
                 */
                uc->queued_valid = false;
            }
            for (c = 0; c < uc->queue_pos; c++) {
                if (user->download_queue->buf[c] != NULL)
                    rank++;
            }
            uc->queue_pos = rank;
            /* uc->queue_pos may be uc->download_queue->cur after
             * compact_queue_ptrv, but this is OK. */
        }
    }

    compact_queue_ptrv(user);
}

static int
user_info_compare(const void *i1, const void *i2)
{
    const DCUserInfo *f1 = *(const DCUserInfo **) i1;
    const DCUserInfo *f2 = *(const DCUserInfo **) i2;

    return strcoll(f1->nick, f2->nick);
}

#define IFNULL(a,x) ((a) == NULL ? (x) : (a))

static void
cmd_who(int argc, char **argv)
{
    uint32_t maxlen;
    HMapIterator it;
    uint32_t c;
    DCUserInfo **items;
    uint32_t count;
    int cols;
    StrBuf *out;

    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }

    if (argc > 1) {
        for (c = 1; c < argc; c++) {
            DCUserInfo *ui;

            ui = hmap_get(hub_users, argv[c]);
            if (ui == NULL) {
                screen_putf(_("%s: No such user on this hub\n"), quotearg(argv[c]));
            } else {
                char *fmt1 = ngettext("byte", "bytes", ui->share_size);
                screen_putf(_("Nick: %s\n"), quotearg(ui->nick));
                screen_putf(_("Description: %s\n"), quotearg(IFNULL(ui->description, "")));
                screen_putf(_("Speed: %s\n"), quotearg(IFNULL(ui->speed, "")));
                screen_putf(_("Level: %d\n"), ui->level);
                screen_putf(_("E-mail: %s\n"), quotearg(IFNULL(ui->email, "")));
                screen_putf(_("Operator: %d\n"), ui->is_operator);
                screen_putf(_("Share Size: %" PRIu64 " %s (%" PRIu64 " MB)\n"), /* " */
                    ui->share_size, fmt1, ui->share_size/(1024*1024));
            }
        }
    	return;
    }

    maxlen = 0;
    for (hmap_iterator(hub_users, &it); it.has_next(&it); ) {
    	DCUserInfo *ui = it.next(&it);
	maxlen = max(maxlen, strlen(quotearg(ui->nick)));
    }

    count = hmap_size(hub_users);
    items = xmalloc(count * sizeof(DCUserInfo *));
    hmap_iterator(hub_users, &it);
    for (c = 0; c < count; c++)
	items[c] = it.next(&it);
    qsort(items, count, sizeof(DCUserInfo *), user_info_compare);

    screen_get_size(NULL, &cols);

    out = strbuf_new();
    for (c = 0; c < count; c++) {
    	DCUserInfo *ui = items[c];
	char *nick = quotearg(ui->nick);

	strbuf_clear(out);
	strbuf_append(out, nick);
	strbuf_append_char_n(out, maxlen+1-strlen(nick), ' ');
	strbuf_appendf(out, "  %7" PRIu64 "M", ui->share_size / (1024*1024)); /* " */
	strbuf_append(out, ui->is_operator ? " op" : "   ");
	if (ui->download_queue->cur > 0)
	    strbuf_appendf(out, " (%3d)", ui->download_queue->cur);
	else
	    strbuf_append(out, "      ");
	strbuf_appendf(out, " %s", quotearg(ui->description ? ui->description : ""));
	if (strbuf_length(out) > cols)
	    strbuf_set_length(out, cols);
	screen_putf("%s\n", strbuf_buffer(out));
    }
    free(items);
    strbuf_free(out);
}

static void
cmd_transfers(int argc, char **argv)
{
    char *format;
    HMapIterator it;
    uint32_t maxlen = 0;
    time_t now;

    hmap_iterator(user_conns, &it);
    while (it.has_next(&it)) {
    	DCUserConn *uc = it.next(&it);
	maxlen = max(maxlen, strlen(uc->name));
    }
    format = xasprintf("%%-%ds  %%s\n", maxlen);

    now = time(NULL);
    if (now == (time_t) -1)
    	warn(_("Cannot get current time - %s\n"), errstr);

    hmap_iterator(user_conns, &it);
    while (it.has_next(&it)) {
    	DCUserConn *uc = it.next(&it);
	char *status;

	status = user_conn_status_to_string(uc, now);
	screen_putf(format, quotearg(uc->name), status);
	free(status);
    }

    screen_putf(_("Upload slots: %d/%d  Download slots: %d/unlimited\n"), used_ul_slots, my_ul_slots, used_dl_slots);
    free(format);
}

static void
cmd_cancel(int argc, char **argv)
{
    uint32_t c;

    if (argc == 1) {
    	screen_putf(_("Usage: %s CONNECTION ...\n"), argv[0]);
	return;
    }

    for (c = 1; c < argc; c++) {
        DCUserConn *uc;

        uc = hmap_get(user_conns, argv[c]);
        if (uc == NULL) {
    	    screen_putf(_("%s: No such user connection.\n"), quotearg(argv[c]));
        } else {
            user_conn_cancel(uc);
        } 
    }

}

static void
cmd_search(int argc, char **argv)
{
    char *tmp;
    
    if (argc == 1) {
    	screen_putf(_("Usage: %s STRING...\n"), argv[0]);
	return;
    }
    if (hub_state < DC_HUB_LOGGED_IN) {
    	screen_putf(_("Not connected.\n"));
	return;
    }
    
    tmp = join_strings(argv+1, argc-1, ' ');
    add_search_request(tmp); /* Ignore errors */
    free(tmp);
}

static void
cmd_results(int argc, char **argv)
{
    uint32_t d;

    if (argc == 1) {
        time_t now;
    
	if (time(&now) == (time_t) -1) {
            warn(_("Cannot get current time - %s\n"), errstr);
            return;
        }

    	for (d = 0; d < our_searches->cur; d++) {
    	    DCSearchRequest *sd = our_searches->buf[d];
    	    char *status;
    	    char *spec;

            spec = search_selection_to_string(&sd->selection);
            status = sd->issue_time + SEARCH_TIME_THRESHOLD <= now
                   ? _("Closed") : _("Open");
	    screen_putf(_("%d. %s (%s) Results: %d\n"), d+1, quotearg(spec),
                   status, sd->responses->cur);
	}
	return;
    }

    for (d = 1; d < argc; d++) {
        DCSearchRequest *sd;
        uint32_t c;

        if (!parse_uint32(argv[d], &c) || c == 0 || c-1 >= our_searches->cur) {
            screen_putf(_("%s: Invalid search index.\n"), quotearg(argv[d]));
            continue;
        }
        sd = our_searches->buf[c-1];
        screen_putf(_("Search %d:\n"), c);
        for (c = 0; c < sd->responses->cur; c++) {
            DCSearchResponse *sr = sd->responses->buf[c];
            char *n;
            char *t;

            n = translate_remote_to_local(sr->filename);
            if (sr->filetype == DC_TYPE_DIR) /* XXX: put into some function */
                t = "/";
            else
                t = "";
            screen_putf("%d. %s %s%s\n", c+1, quotearg(sr->userinfo->nick), n, t);
            free(n);
        }
    }
}

static void
cmd_unsearch(int argc, char **argv)
{
    uint32_t c;

    if (argc == 1) {
    	screen_putf(_("Usage: %s INDEX\n"), argv[0]);
	return;
    }
    for (c = 1; c < argc; c++) {
        DCSearchRequest *sd;
        uint32_t index;

        if (!parse_uint32(argv[c], &index) || index == 0 || index-1 >= our_searches->cur) {
            screen_putf(_("%s: Invalid search index.\n"), quotearg(argv[c]));
            return;
        }

        sd = our_searches->buf[index-1];
        ptrv_remove_range(our_searches, index-1, index);
        free_search_request(sd);    
    }
}

static void
cmd_alias(int argc, char **argv)
{
    uint32_t c;

    if (argc == 1) {
	TMapIterator it;

	for (tmap_iterator(commands, &it); it.has_next(&it); ) {
	    DCCommand *cmd = it.next(&it);
	    if (cmd->type == DC_CMD_ALIAS) {
		DCAliasCommand *alias = (DCAliasCommand *) cmd;
		screen_putf("alias %s \"%s\"\n" /*no translation */, cmd->name, quotearg(alias->alias_spec));
	    }
	}
	return;
    }

    for (c = 1; c < argc; c++) {
        char *name = argv[c];
        char *value;
        DCCommand *cmd;

        value = strchr(name, '=');
        if (value == NULL) {
            cmd = tmap_get(commands, name);
            if (cmd != NULL && cmd->type == DC_CMD_ALIAS) {
                DCAliasCommand *alias = (DCAliasCommand *) cmd;
                screen_putf("alias %s=\"%s\"\n" /*no translation */, name, quotearg(alias->alias_spec));
            } else {
                warn(_("%s: No such alias.\n"), quotearg(name));
            }
        } else {
            *value = '\0';
            value++;
            if (strpbrk(name, " \"#;") != NULL) {
                warn(_("%s: Invalid alias name\n"), quotearg(name));
                continue;
            }
            cmd = tmap_get(commands, name);
            if (cmd == NULL) {
                DCAliasCommand *alias = xmalloc(sizeof(DCAliasCommand));
                alias->cmd.name = xstrdup(name);
                alias->cmd.type = DC_CMD_ALIAS;
                alias->alias_spec = xstrdup(value);
                tmap_put(commands, alias->cmd.name, alias);
            } else if (cmd->type == DC_CMD_ALIAS) {
                DCAliasCommand *alias = (DCAliasCommand *) cmd;
                free(alias->alias_spec);
                alias->alias_spec = xstrdup(value);
            } else {
                warn(_("%s: Cannot override built-in command.\n"), quotearg(cmd->name));
            }
        }
    }
}

static void
cmd_unalias(int argc, char **argv)
{
    uint32_t c;

    if (argc == 1) {
    	screen_putf(_("Usage: %s NAME ...\n"), argv[0]);
    	return;
    }

    for (c = 0; c < argc; c++) {
	DCCommand *cmd = tmap_get(commands, argv[c]);
	if (cmd == NULL || cmd->type != DC_CMD_ALIAS) {
	    warn(_("%s: No such alias.\n"), quotearg(argv[c]));
	} else {
	    DCAliasCommand *alias = (DCAliasCommand *) cmd;
	    tmap_remove(commands, cmd->name);
	    free(alias->cmd.name);
	    free(alias->alias_spec);
	    free(alias);
	}
    }
}

void
update_prompt(void)
{
    if (browsing_myself || browse_user != NULL) {
	char *nick = browsing_myself ? my_nick : browse_user->nick;
	
	if (browse_list == NULL) {
	    set_screen_prompt("%s:(%s)> ", PACKAGE, quotearg(nick));
	} else {
	    set_screen_prompt("%s:%s:%s> ", PACKAGE, quotearg_n(0, nick), quotearg_n(1, browse_path));
	}
    } else {
	set_screen_prompt("%s> ", PACKAGE);
    }
}

static void
append_download_file(DCUserInfo *ui, DCFileList *node, DCFileList *basenode, uint32_t *file_count, uint64_t *byte_count)
{
    if (node->type == DC_TYPE_REG) {
        DCQueuedFile *queued;
        char *path;

        if (ui != NULL) { /* if NULL then we're browsing ourselves */
            path = filelist_get_path(node);
            if (ptrv_find(ui->download_queue, path, (comparison_fn_t) queued_file_cmp) >= 0) {
                screen_putf(_("Queue already contains this file, ignoring\n"));
                free(path);
                return;
            }

            queued = xmalloc(sizeof(DCQueuedFile));
            queued->filename = path;
            queued->base_path = filelist_get_path_with_trailing_slash(basenode);
            queued->flag = DC_TF_NORMAL;
            queued->status = DC_QS_QUEUED;
            queued->length = node->size;
            ptrv_append(ui->download_queue, queued);
        }

        (*byte_count) += node->size;
        (*file_count)++;
    }
    else if (node->type == DC_TYPE_DIR) {
        HMapIterator it;

        hmap_iterator(node->dir.children, &it);
        while (it.has_next(&it))
            append_download_file(ui, it.next(&it), basenode, file_count, byte_count);
    }
}

/* Note: This command assumes that we are browsing some user.
 * If we in the future would allow browse-less getting,
 * i.e. get NICK FILE, then we must here add additional checks
 * found in cmd_browse, such as strcmp(my_nick, nick)==0 etc.
 */
static void
cmd_get(int argc, char **argv)
{
    uint32_t c;
    bool dl_some = false;

    if (argc == 1) {
        screen_putf(_("Usage: %s FILE ...\n"), argv[0]);
        return;
    }
    if (browse_list == NULL) {
        screen_putf(_("Not browsing any user.\n"));
        return;
    }
    /*if (browsing_myself) {
        screen_putf(_("Cannot download files from myself.\n"));
        return;
    }*/

    for (c = 1; c < argc; c++) {
        DCFileList *basenode;
        bool quoted = false;
        char *basedir;
        uint64_t byte_count = 0;
        uint32_t file_count = 0;
        PtrV *results;
        int d;

        results = ptrv_new();
        if (has_leading_slash(argv[c])) {
            basenode = browse_list;
            basedir = "/";
        } else {
            basenode = filelist_lookup(browse_list, browse_path);
            basedir = "";
        }
        remote_wildcard_expand(argv[c], &quoted, basedir, basenode, results);
        for (d = 0; d < results->cur; d++) {
            char *name = results->buf[d];
            char *fullname;  
            DCFileList *node;

            fullname = apply_cwd(name);
            node = filelist_lookup(browse_list, fullname);
            if (node != NULL) { /* Technically, this shouldn't fail */
                screen_putf(_("Matched %s\n"), quotearg(name));
                append_download_file(browse_user, node, basenode, &file_count, &byte_count);
            }
            free(fullname);                       
            free(name);
        }
        ptrv_free(results);

        if (file_count > 0) {
            screen_putf(_("Downloading %" PRIu64 " %s in %" PRIu32 " %s\n"),
                byte_count, ngettext("byte", "bytes", byte_count),
                file_count, ngettext("file", "files", file_count));
            dl_some = true;
        } else {
            screen_putf(_("%s: No files to download.\n"), quotearg(argv[c]));
        }
    }

    if (dl_some && !browsing_myself) {
        if (!has_user_conn(browse_user, DC_DIR_RECEIVE) && browse_user->conn_count < DC_USER_MAX_CONN) {
            hub_connect_user(browse_user); /* Ignore errors */
        } else {
            screen_putf(_("No free connections. Queued files for download.\n"));
        }
    }
}

static void
lookup_address_looked_up(int rc, struct addrinfo *ai, void *data)
{
    char *host = data;

    screen_putf("%s:", quotearg(host));
    if (rc == 0) {
        for (; ai != NULL; ai = ai->ai_next) {
            struct sockaddr_in *addr = (struct sockaddr_in *) ai->ai_addr;
            screen_putf(" %s", inet_ntoa(addr->sin_addr));
        }
    } else {
        screen_putf(" %s", gai_strerror(rc));
    }
    screen_putf("\n");

    free(host);
}

static void
cmd_lookup(int argc, char **argv)
{
    int c;

    if (argc == 1) {
        screen_putf(_("missing host argument\n"));
        return;
    }

    for (c = 1; c < argc; c++) {
        struct addrinfo hints = { 0, PF_INET, SOCK_STREAM, 0, };
        add_lookup_request(argv[c], NULL, &hints, lookup_address_looked_up, xstrdup(argv[c]));
    }
}

static void
cmd_share(int argc, char **argv)
{
    struct stat st;
    char *dir_fs;

    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }

    if (argc == 1) {
        screen_putf(_("missing directory argument\n"));
        return;
    }

    dir_fs = main_to_fs_string(argv[1]);
    if (stat(dir_fs, &st) < 0) {
    	screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(argv[1]), errstr);
        free(dir_fs);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
    	screen_putf(_("%s: Not a directory\n"), quotearg(argv[1]));
        free(dir_fs);
        return;
    }
    if (update_request_mq != NULL) {
        if (!update_request_add_shared_dir(dir_fs)) {
            screen_putf(_("%s: Cannot process directory - %s\n"), quotearg(argv[1]), errstr);
        }
    }
    free(dir_fs);
}

static void
cmd_unshare(int argc, char **argv)
{
    struct stat st;
    char *dir_fs;

    if (argc > 2) {
        warn(_("too many arguments\n"));
        return;
    }

    if (argc == 1) {
        screen_putf(_("missing directory argument\n"));
        return;
    }

    dir_fs = main_to_fs_string(argv[1]);
    if (stat(dir_fs, &st) < 0) {
    	screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(argv[1]), errstr);
        free(dir_fs);
        return;
    }
    if (!S_ISDIR(st.st_mode)) {
    	screen_putf(_("%s: Not a directory\n"), quotearg(argv[1]));
        free(dir_fs);
        return;
    }
    if (update_request_mq != NULL) {
        if (!update_request_del_shared_dir(dir_fs)) {
            screen_putf(_("%s: Cannot process directory - %s\n"), quotearg(argv[1]), errstr);
        }
    }
    free(dir_fs);
}
