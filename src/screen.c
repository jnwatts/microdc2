/* screen.c - User interface management (Readline)
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
#include <unistd.h>		/* POSIX: STDIN_FILENO */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#if defined(HAVE_READLINE_READLINE_H)
# include <readline/readline.h>
#elif defined(HAVE_READLINE_H)
# include <readline.h>
#endif
#if defined(HAVE_READLINE_HISTORY_H)
# include <readline/history.h>
#elif defined(HAVE_HISTORY_H)
# include <history.h>
#endif
#include "xalloc.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "iconvme.h"
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "microdc.h"
#include "common/strbuf.h"
#include "common/error.h"
#include "common/strleftcmp.h"
#include "common/bksearch.h"
#include "common/quoting.h"

/* Flow of messages in microdc:
 *
 *  main process:
 *    warn(error.c) -> warn_writer(error.c)  -> screen_warn_writer(screen.c) -> screen_writer(screen.c) -> flag_vputf(screen.c) -> log/screen
 *    die(error.c)  -> warn_writer(error.c)  -> screen_warn_writer(screen.c) -> screen_writer(screen.c) -> flag_vputf(screen.c) -> log/screen
 *                     screen_putf(screen.c) -> flag_putf(screen.c)          -> screen_writer(screen.c) -> flag_vputf(screen.c) -> log/screen
 *  user process:
 *    warn(error.c) -> warn_writer(error.c)  -> screen_warn_writer(screen.c) -> screen_writer(screen.c) -> user_screen_writer(user.c) -> ipc
 *    die(error.c)  -> warn_writer(error.c)  -> screen_warn_writer(screen.c) -> screen_writer(screen.c) -> user_screen_writer(user.c) -> ipc
 *                     screen_putf(screen.c) -> flag_putf(screen.c)          -> screen_writer(screen.c) -> user_screen_writer(user.c) -> ipc
 *
 */

typedef enum {
    SCREEN_UNINITIALIZED,	/* History not initialized */
    SCREEN_NO_HANDLER,		/* rl_callback_handler_install not called. */
    SCREEN_RL_CLEARED,		/* Readline has been cleared. Need to redisplay after. */
    SCREEN_RL_DISPLAYED,	/* Readline will be displayed. Need to clear first. */
    SCREEN_SUSPENDED,		/* A command using the screen is running. */
} ScreenState;

static void clear_rl();
static void user_input(char *line);
static int screen_warn_writer(const char *format, va_list args);
static void flag_vputf(DCDisplayFlag flag, const char *format, va_list args);

char *log_filename = NULL;
ScreenWriter screen_writer = flag_vputf;
static char *screen_prompt = NULL;
static FILE *log_fh = NULL;
static PtrV *suspend_msgs = NULL;
static ScreenState screen_state = SCREEN_UNINITIALIZED;

/* This piece of code was snatched from lftp, and modified somewhat by me.
 * I suggest a function called rl_clear() be added to readline. The
 * function clears the prompt and everything the user has written so far on
 * the line. The cursor is positioned at the beginning of the line that
 * contained the prompt. Note: This function doesn't modify the screen_state
 * variable.
 */
static void
clear_rl()
{
    extern char *rl_display_prompt;
#if HAVE__RL_MARK_MODIFIED_LINES
    extern int _rl_mark_modified_lines;
    int old_mark = _rl_mark_modified_lines;
#endif
    int old_end = rl_end;
    char *old_prompt = rl_display_prompt;

    rl_end = 0;
    rl_display_prompt = "";
    rl_expand_prompt("");
#if HAVE__RL_MARK_MODIFIED_LINES
    _rl_mark_modified_lines = 0;
#endif

    rl_redisplay();

    rl_end = old_end;
    rl_display_prompt = old_prompt;
#if HAVE__RL_MARK_MODIFIED_LINES
    _rl_mark_modified_lines = old_mark;
#endif
    if (rl_display_prompt == rl_prompt)
        rl_expand_prompt(rl_prompt);
}

bool
set_log_file(const char *new_filename, bool verbose)
{
    if (log_fh != NULL) {
	if (fclose(log_fh) != 0)
	    warn(_("%s: Cannot close file - %s\n"), quotearg(log_filename), errstr);
	log_fh = NULL;
	free(log_filename);
	log_filename = NULL;
    }
    if (new_filename == NULL) {
	if (verbose)
	    screen_putf(_("No longer logging to file.\n"));
	return true;
    }
    log_fh = fopen(new_filename, "a");
    if (log_fh == NULL) {
	screen_putf(_("%s: Cannot open file for appending - %s\n"), quotearg(new_filename), errstr);
	return false;
    }
    log_filename = xstrdup(new_filename);
    if (verbose)
	screen_putf(_("Logging to `%s'.\n"), quotearg(new_filename));
    return true;
}

/* Readline < 5.0 disables SA_RESTART on SIGWINCH for some reason.
 * This turns it back on.
 * This was partially copied from guile.
 */
static int
fix_winch(void)
{
    struct sigaction action;

    if (sigaction(SIGWINCH, NULL, &action) >= 0) {
    	action.sa_flags |= SA_RESTART;
    	sigaction(SIGWINCH, &action, NULL); /* Ignore errors */
    }
    return 0;
}

/* XXX: move this to strutil.c or something? */
void
get_file_dir_part(const char *word, char **dir_part, const char **file_part)
{
    const char *fp;

    fp = strrchr(word, '/');
    if (fp == NULL) {
	*dir_part = xstrdup("");
	*file_part = word;
    } else {
	for (; fp > word && fp[-1] == '/'; fp--);
	*dir_part = xstrndup(word, fp - word + 1);
	for (fp++; *fp == '/'; fp++);
	*file_part = fp;
    }
}

DCCompletionEntry *
new_completion_entry_full(char *input, char *display, const char *input_fmt, const char *display_fmt, bool finalize, bool quoted)
{
    DCCompletionEntry *entry = xmalloc(sizeof(DCCompletionEntry));
    entry->input = input;
    entry->display = display;
    entry->input_fmt = input_fmt;
    entry->input_single_fmt = NULL;
    entry->display_fmt = display_fmt;
    entry->finalize = finalize;
    entry->quoted = quoted;
    return entry;
}

DCCompletionEntry *
new_completion_entry(const char *input, const char *display)
{
    DCCompletionEntry *entry = xmalloc(sizeof(DCCompletionEntry));
    entry->input = input == NULL ? NULL : xstrdup(input);
    entry->display = display == NULL ? entry->input : xstrdup(display);
    entry->display_fmt = "%s";
    entry->input_fmt = "%s";
    entry->input_single_fmt = NULL;
    entry->finalize = true;
    entry->quoted = false;
    return entry;
}

void
free_completion_entry(DCCompletionEntry *entry)
{
    if (entry->display != entry->input)
        free(entry->display);
    free(entry->input);
    free(entry);
}

/* This function is called via the warn_writer variable by warn() and
 * die() in error.c to print messages on screen properly.
 */
static int
screen_warn_writer(const char *format, va_list args)
{
    screen_writer(DC_DF_COMMON, format, args);
    return 0;
}

static void
flag_vputf(DCDisplayFlag flag, const char *format, va_list args)
{
    //va_list args2;

    //va_copy(args2, args);

    if (display_flags & flag) {
        if (screen_state == SCREEN_SUSPENDED) {
            ptrv_append(suspend_msgs, xvasprintf(format, args));
        } else {
            if (screen_state == SCREEN_RL_DISPLAYED) {
                clear_rl();
                screen_state = SCREEN_RL_CLEARED;
            }
            vprintf(format, args);
            fflush(stdout);
        }
    }
    if (log_fh != NULL && log_flags & flag) {
        char c_time[1024];
        time_t now = time(NULL);
        struct tm _tm = {0};
        if (NULL != localtime_r(&now, &_tm) && 0 != strftime(c_time, 1023, "%d.%m.%Y %H:%M:%S", &_tm)) {
            fprintf(log_fh, "%s ", c_time);
        }
        char* msg = xvasprintf(format, args);
        //va_end(args2);
        char* log_msg = main_to_log_string(msg);
        free(msg);
        fprintf(log_fh, log_msg);
        free(log_msg);
        fflush(log_fh);
    }
}

void
flag_putf(DCDisplayFlag flag, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    screen_writer(flag, format, args);
    va_end(args);
}

/* This function is called by readline whenever the user has
 * entered a full line (usually by pressing enter).
 */
static void
user_input(char *line)
{
    /* Readline has already made way for us. */
    screen_state = SCREEN_RL_CLEARED;

    if (log_fh != NULL) {
        char c_time[1024];
        time_t now = time(NULL);
        struct tm _tm = {0};
        if (NULL != localtime_r(&now, &_tm) && 0 != strftime(c_time, 1023, "%d.%m.%Y %H:%M:%S", &_tm)) {
            fprintf(log_fh, "%s ", c_time);
        }
	    fprintf(log_fh, "> %s\n", line == NULL ? "(null)" : line);
        fflush(log_fh);
    }
    
    if (line == NULL) {
        /* Ctrl+D was pressed on an empty line. */
        screen_putf("exit\n");
        running = false;
    } else if (line[0] != '\0') {
        add_history(line);
	/* XXX: handle input differently
	 * !shell_cmd
	 * /cmd
	 * depending on context: msg, say, cmd
	 */
        command_execute(line);
    }

    /* As soon as we exit this function, a new prompt will be displayed.
     */
    if (running) {
        if (screen_state != SCREEN_SUSPENDED)
            screen_state = SCREEN_RL_DISPLAYED;
    } else {
        rl_callback_handler_remove();
        FD_CLR(STDIN_FILENO, &read_fds);
        screen_state = SCREEN_NO_HANDLER;
    }
}

/* Move down one line (rl_on_new_line+redisplay), print a new prompt and
 * empty the input buffer. Unlike rl_clear this doesn't erase anything on
 * screen.
 *
 * This is usually called when a user presses Ctrl+C.
 * XXX: Should find a better way to do this (see lftp or bash).
 */
void
screen_erase_and_new_line(void)
{
    if (screen_state == SCREEN_RL_DISPLAYED) {
        rl_callback_handler_remove();
        putchar('\n');
        rl_callback_handler_install(screen_prompt, user_input);
    }
}

/* Suspend screen operations so that nothing in here
 * uses standard in or standard out. This is necessary when
 * running a system command in the terminal.
 */
void
screen_suspend(void)
{
    if (screen_state == SCREEN_RL_DISPLAYED || screen_state == SCREEN_RL_CLEARED) {
        rl_callback_handler_remove();
	FD_CLR(STDIN_FILENO, &read_fds);
        if (screen_state == SCREEN_RL_DISPLAYED)
            putchar('\n');
	suspend_msgs = ptrv_new();
	screen_state = SCREEN_SUSPENDED;
    }
}

/* Wake up screen after suspension.
 */
void
screen_wakeup(bool print_newline_first)
{
    if (screen_state == SCREEN_SUSPENDED) {
        int c;
        if (print_newline_first)
            putchar('\n');
        for (c = 0; c < suspend_msgs->cur; c++)
            fputs((char *) suspend_msgs->buf[c], stdout);
	ptrv_foreach(suspend_msgs, free);
	ptrv_free(suspend_msgs);
        screen_state = SCREEN_NO_HANDLER;
    }
}

/* Finish screen management. Usually called from main_finish.
 */
void
screen_finish(void)
{
    if (screen_state == SCREEN_SUSPENDED) {
        int c;
        for (c = 0; c < suspend_msgs->cur; c++)
            fputs((char *) suspend_msgs->buf[c], stdout);
	ptrv_foreach(suspend_msgs, free);
	ptrv_free(suspend_msgs);
    } else if (screen_state > SCREEN_NO_HANDLER) {
        rl_callback_handler_remove();
        if (screen_state == SCREEN_RL_DISPLAYED)
            putchar('\n');
        FD_CLR(STDIN_FILENO, &read_fds);
    }

    if (screen_state >= SCREEN_NO_HANDLER) {
    	char *path;

	/* Save history */
    	get_package_file("history", &path);
    	if (mkdirs_for_file(path) >= 0) {
            if (write_history(path) != 0)
                warn(_("%s: Cannot write history - %s\n"), quotearg(path), errstr);
        }
        free(path);

        rl_pre_input_hook = NULL;

        warn_writer = default_warn_writer;
        screen_state = SCREEN_UNINITIALIZED;
        free(screen_prompt);

        set_log_file(NULL, false);
    }
}

/* XXX: move somewhere else. */
static char *
dequote_string(const char *str)
{
    return dequote_words_full(str, false, false, true, false/*true*/, NULL);
}

char *
filename_quote_string(const char *str, bool dquotes, bool finalize)
{
    return quote_word_full(str, dquotes, finalize, ";*?", "#", false, true, false/*true*/, true);
}

char *
quote_string(const char *str, bool dquotes, bool finalize)
{
    return quote_word_full(str, dquotes, finalize, ";", "#", false, true, false/*true*/, true);
}

void
fill_completion_info(DCCompletionInfo *ci)
{
    ci->word_full = xstrndup(ci->line + ci->ws, ci->we - ci->ws);
    ci->word = dequote_string(ci->word_full);
    ci->word_index = get_word_index(ci->line, ci->ws);
}

/* Return escaped text to display for the specified completion entry.
 */
static char *
get_escaped_display(DCCompletionEntry *ce)
{
    char *d1;
    char *d2;

    d1 = xasprintf(ce->display_fmt, ce->display);
    d2 = quote_word_full(d1, false, false, ";", "#", false, true, false/*true*/, false);
    free(d1);

    return d2;
}

/* Return quoted text to put in the command line buffer for the completion entry CE.
 * If SINGLE is true, then the input_single_fmt may be used.
 * If FINALIZE is true, then the text will have a trailing closing quote (if quoted)
 * and a space after that. The returned value should be freed when no longer needed.
 */
static char *
get_quoted_input(DCCompletionInfo *ci, DCCompletionEntry *ce, bool single, bool finalize)
{
    char *str1;
    char *str2;

    /* If quoted already, get rid of trailing quotes (at least temporarily). */
    if (ce->quoted && ce->input[0] == '"')
        ce->input[strlen(ce->input)-1] = '\0';

    /* Pick the right input format, and print using it. */
    if (single && ce->input_single_fmt != NULL) {
        str1 = xasprintf(ce->input_single_fmt, ce->input);
    } else {
        str1 = xasprintf(ce->input_fmt, ce->input);
    }

    /* Add quotes if necessarily. If already quoted, add trailing quote if it should be there. */
    if (ce->quoted) {
        if (ce->input[0] == '"' && ce->finalize && finalize) {
            str2 = xasprintf("%s\"", str1);
            free(str1);
        } else {
            str2 = str1;
        }
    } else {
        str2 = quote_word_full(str1, ci->line[ci->ws] == '"', finalize && ce->finalize, ";", "#", false, true, true, true);
        free(str1);
    }

    if (finalize && ce->finalize) {
        str1 = xasprintf("%s ", str2);
        free(str2);
        return str1;
    }
    return str2;
}

static size_t
leading_same(const char *c1, const char *c2)
{
    int c;
    for (c = 0; c1[c] != '\0' && c2[c] != '\0'; c++) {
        if (c1[c] != c2[c])
            break;
    }
    return c;
}

static int
completion_readline(int key, int count)
{
    char *input;
    DCCompletionInfo ci;
    int c;

    ci.line = rl_line_buffer;/*RL*/
    ci.we = rl_point;/*RL*/
    ci.ws = find_completion_word_start(ci.line, ci.we);
    ci.results = ptrv_new();
    fill_completion_info(&ci);

    default_completion_selector(&ci);
    if (ci.results->cur == 0) {
        rl_ding();/*RL*/
        return -1;
    }

    if (ci.results->cur == 1) {
        input = get_quoted_input(&ci, ci.results->buf[0], true, true);
    } else {
        int minlen;
        int maxlen;
        char *matches[ci.results->cur + 1];
        DCCompletionEntry *ce;

        ce = ci.results->buf[0];
        input = get_quoted_input(&ci, ce, false, false);
        minlen = strlen(input);
        matches[1] = get_escaped_display(ce);
        maxlen = strlen(matches[1]);

        for (c = 1; c < ci.results->cur; c++) {
            char *input2;

            ce = ci.results->buf[c];
            input2 = get_quoted_input(&ci, ce, false, false);
            minlen = MIN(minlen, leading_same(input, input2));
            free(input2);
            matches[c+1] = get_escaped_display(ce);
            maxlen = MAX(maxlen, strlen(matches[c+1]));
        }

        rl_display_match_list(matches, ci.results->cur, maxlen);/*RL*/
        for (c = 0; c < ci.results->cur; c++)
            free(matches[c+1]);
        rl_on_new_line();/*RL*/
        input[minlen] = '\0';  /* XXX: get rid of trailing escape on input somehow! */
    }

    rl_begin_undo_group();/*RL*/
    if (ci.ws != ci.we)
        rl_point/*RL*/ -= rl_delete_text(ci.ws, ci.we);/*RL*/
    rl_insert_text(input);/*RL*/ /* automaticly updates rl_point */
    rl_end_undo_group();/*RL*/
    free(input);

    free(ci.word_full);
    free(ci.word);
    ptrv_foreach(ci.results, (PtrVForeachCallback) free_completion_entry);
    ptrv_free(ci.results);

    return 0;
}

/* Prepare the screen prior to waiting for events with select/poll/epoll.
 * Redisplay the prompt if it was cleared by a call to screen_(v)put(f).
 */
void
screen_prepare(void)
{
    if (screen_state == SCREEN_SUSPENDED)
        return;

    if (screen_state == SCREEN_UNINITIALIZED) {
    	char *path;

        screen_state = SCREEN_NO_HANDLER;
        warn_writer = screen_warn_writer;
        if (screen_prompt == NULL)
            screen_prompt = xasprintf("%s> ", PACKAGE);

        rl_readline_name = PACKAGE;
        rl_add_defun(PACKAGE "-complete", completion_readline, '\t');
        rl_pre_input_hook = fix_winch;

        using_history();
        get_package_file("history", &path);
        if (path != NULL) {
            if (read_history(path) != 0 && errno != ENOENT) {
                warn(_("%s: Cannot read history - %s\n"), quotearg(path), errstr);
            }
            free(path);
        }
    }
    if (screen_state == SCREEN_NO_HANDLER) {
        rl_callback_handler_install(screen_prompt, user_input);
        FD_SET(STDIN_FILENO, &read_fds);
    } else if (screen_state == SCREEN_RL_CLEARED) {
        rl_set_prompt(screen_prompt);
        rl_redisplay();
    }
    screen_state = SCREEN_RL_DISPLAYED;
}

void screen_redisplay_prompt()
{
    rl_set_prompt(screen_prompt);
    rl_redisplay();
    screen_state = SCREEN_RL_DISPLAYED;
}

/* This function is called from the main loop when there's input for us to
 * read on stdin.
 */
void
screen_read_input(void)
{
    rl_callback_read_char();
}

/* Return the size of the screen.
 */
void
screen_get_size(int *rows, int *cols)
{
    int dummy;
    rl_get_screen_size(rows ? rows : &dummy, cols ? cols : &dummy);
}

void
set_screen_prompt(const char *prompt, ...)
{
    va_list args;

    if (screen_prompt != NULL)
	free(screen_prompt);
    va_start(args, prompt);
    screen_prompt = xvasprintf(prompt, args);
    va_end(args);
    if (screen_state != SCREEN_SUSPENDED)
        rl_set_prompt(screen_prompt);
}

/* Look up completion alternatives from a sorted list using binary search.
 */
void
sorted_list_completion_generator(const char *base, PtrV *results, void *items,
                                 size_t item_count, size_t item_size,
                                 size_t key_offset)
{
    const void *item;
    const void *last_item;

    if (bksearchrange(base, items, item_count, item_size,
	              key_offset, (comparison_fn_t) strleftcmp,
	              &item, &last_item)) {
        while (item <= last_item) {
	    char *key = xstrdup(*(const char **) (((const char *) item) + key_offset));
	    ptrv_append(results, new_completion_entry(key, NULL));
	    item = (const void *) (((const char *) item) + item_size);
        }
    }
}

int
completion_entry_display_compare(const void *e1, const void *e2)
{
    const DCCompletionEntry *ce1 = *(const DCCompletionEntry **) e1;
    const DCCompletionEntry *ce2 = *(const DCCompletionEntry **) e2;
    char *s1 = xasprintf(ce1->display_fmt, ce1->display); /* XXX: this is really slow */
    char *s2 = xasprintf(ce2->display_fmt, ce2->display); /* XXX: this is really slow */
    int cmp = strcmp(s1, s2);
    free(s1);
    free(s2);
    return cmp;
}
