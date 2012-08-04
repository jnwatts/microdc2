/* optparser.c - Option parser that reports errors properly
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
#include <stdarg.h>		/* C89 */
#include <string.h>		/* C89 */
#include <stdio.h>		/* C89 */
#include <stdbool.h>		/* Gnulib/C99/POSIX */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xalloc.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "tmap.h"
#include "strleftcmp.h"
#include "substrcmp.h"
#include "optparser.h"

typedef struct _OptParserState OptParserState;

struct _OptParserState {
    int cur_arg;
    int cur_chr;
    bool no_more_opts;
    char *value;
    char *error;
};

struct _OptParser {
    TMap *options;
    OptDetail *short_opts[128-32]; /* XXX: this is crude, but it works */
    bool parse_argv0;
    //bool print_errors;
    //bool reorder_args;
    //bool return_args;
    bool parsing_opts;
    int argc;
    char **argv;
    OptParserState parse_state;
};

static void
parse_config(OptParser *parser, OptParserConfig field, va_list args)
{
    if (field & OPTP_PARSE_ARGV0)
        parser->parse_argv0 = true;
    /*if (field & OPTP_NO_ERRORS)
        parser->print_errors = false;
    if (field & OPTP_NO_REORDER)
        parser->reorder_args = false;
    if (field & OPTP_RETURN_ARGS)
        parser->return_args = true;*/
}

static void
report_error(OptParser *p, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    p->parse_state.error = xvasprintf(format, args);
    va_end(args);
}

static OptDetail *
lookup_long_option(OptParser *p, const char *str)
{
    TMapIterator it;

    tmap_iterator_partial(p->options, &it, str, (comparison_fn_t) strleftcmp);
    if (it.has_next(&it)) {
        OptDetail *option = it.next(&it);

        while (it.has_next(&it)) {
            if (it.next(&it) != option) {
                report_error(p, _("option `--%s' is ambiguous"), str);
                return NULL;
            }
        }

        return option;
    }

    report_error(p, _("unrecognized option `--%s'"), str);
    return NULL;
}

OptParser *
optparser_new(OptDetail *opts, int count, OptParserConfig field, ...)
{
    OptParser *parser;
    va_list args;
    int c;

    parser = xmalloc(sizeof(OptParser));
    parser->parse_argv0 = false;
    //parser->print_errors = true;
    //parser->reorder_args = true;
    //parser->return_args = false;
    parser->parse_state.error = NULL;
    if (count < 0)
        count = INT_MAX;
    memset(parser->short_opts, 0, sizeof(parser->short_opts));
    parser->options = tmap_new();
    tmap_set_compare_fn(parser->options, (comparison_fn_t) strcmp);
    for (c = 0; c < count && opts[c].names != NULL; c++) {
        const char *s;
        char *e;

        for (s = opts[c].names; (e = strchr(s, '|')) != NULL; s = e+1) {
            if (e-s == 1 && s[0] >= 32) {
                parser->short_opts[s[0] - 32] = &opts[c];
            } else {
                tmap_put(parser->options, xstrndup(s, e-s), &opts[c]);
            }
        }
        if (s[0] >= 32 && s[1] == '\0') {
            parser->short_opts[s[0] - 32] = &opts[c];
        } else {
            tmap_put(parser->options, xstrdup(s), &opts[c]);
        }
    }

    va_start(args, field);
    parse_config(parser, field, args);
    va_end(args);

    return parser;
}

void
optparser_free(OptParser *parser)
{
    tmap_foreach_key(parser->options, free);
    tmap_free(parser->options);
    free(parser->parse_state.error);
    free(parser);
}

void
optparser_parse(OptParser *p, int argc, char **argv)
{
    p->argc = argc;
    p->argv = argv;

    p->parse_state.cur_arg = p->parse_argv0 ? 0 : 1;
    p->parse_state.cur_chr = 0;
    p->parse_state.no_more_opts = false;
    free(p->parse_state.error);
    p->parse_state.error = NULL;
    p->parsing_opts = true;
}

static int
handle_short_option(OptParser *p, OptParserState *s)
{
    char *arg;
    OptDetail *opt;
    char name;

    arg = p->argv[s->cur_arg];
    name = arg[s->cur_chr];
    if (name < 32) {
        report_error(p, _("invalid option -- %c"), name);
        return OPTP_ERROR;
    }
    opt = p->short_opts[name - 32];
    if (opt == NULL) {
        report_error(p, _("invalid option -- %c"), name); 
        return OPTP_ERROR;
    }

    if (opt->arg == OPTP_NO_ARG) {
	if (arg[s->cur_chr + 1] == '\0') {
	    s->cur_chr = 0;
	    s->cur_arg++;
        } else {
            s->cur_chr++;
        }
	s->value = NULL;
    } else if (opt->arg == OPTP_OPT_ARG) {
	s->cur_arg++;
        s->value = (arg[s->cur_chr + 1] == '\0' ? NULL : arg + s->cur_chr + 1);
        s->cur_chr = 0;
    } else if (opt->arg == OPTP_REQ_ARG) {
	s->cur_arg++;
        if (arg[s->cur_chr + 1] == '\0') {
            if (s->cur_arg >= p->argc) {
                report_error(p, _("option requires an argument -- %c"), name);
                return OPTP_ERROR;
            }
            s->value = p->argv[s->cur_arg];
            s->cur_arg++;
        } else {
            s->value = arg + s->cur_chr + 1;
        }
        s->cur_chr = 0;
    }

    return opt->code;
}

static int
next_token(OptParser *p, OptParserState *s)
{
    char *arg;

    if (s->error != NULL)
        return OPTP_ERROR;
    if (s->cur_arg >= p->argc) {
        s->value = NULL;
        return OPTP_DONE;
    }
    if (s->cur_chr != 0)
        return handle_short_option(p, s);

    arg = p->argv[s->cur_arg];
    if (!s->no_more_opts) {
        if (arg[0] == '-') {
            if (arg[1] == '-') {
                OptDetail *opt;

                s->cur_arg++;
                if (arg[2] == '\0') {
                    s->no_more_opts = true;
                    return next_token(p, s); /* will recurse only once */
                }
                s->value = strchr(arg + 2, '=');
                if (s->value == NULL) {
                    opt = lookup_long_option(p, arg + 2);
                } else {
                    arg = xstrndup(arg + 2, s->value - arg - 2);
                    opt = lookup_long_option(p, arg);
                    free(arg);
                    s->value++;
                }
                if (opt == NULL)
                    return OPTP_ERROR;
                if (opt->arg == OPTP_NO_ARG) {
                    if (s->value != NULL) {
                        report_error(p, _("option `--%s' doesn't allow an argument"), arg + 2);
                        return OPTP_ERROR;
                    }
                } else if (opt->arg == OPTP_OPT_ARG) {
                    /* no op */
                } else if (opt->arg == OPTP_REQ_ARG) {
                    if (s->value == NULL) {
                        if (s->cur_arg >= p->argc) {
                            report_error(p, _("option `--%s' requires an argument"), arg + 2);
                            return OPTP_ERROR;
                        }
                        s->value = p->argv[s->cur_arg];
                        s->cur_arg++;
                    }
                }
                return opt->code;
            }
            if (arg[1] != '\0') {
                s->cur_chr = 1;
                return handle_short_option(p, s);
            }
        }
    }
   
    s->value = arg;
    s->cur_arg++;
    return OPTP_ARG;
}

static void
check_parsing_opts(OptParser *p, OptParserState *s, bool want_parsing_opts)
{
    if (want_parsing_opts != p->parsing_opts) {
        s->cur_arg = p->parse_argv0 ? 0 : 1;
        s->cur_chr = 0;
        s->no_more_opts = false;
        s->value = NULL;
	p->parse_state = *s;
        p->parsing_opts = want_parsing_opts;
    }
}

static int
look_for_token(OptParser *p, OptParserState *s, bool look_for_options)
{
    check_parsing_opts(p, s, look_for_options);
    for (;;) {
        int token;

        token = next_token(p, s);
        if (token == OPTP_DONE || token == OPTP_ERROR)
            return token;
        if (look_for_options ^ (token == OPTP_ARG))
            return token;
    }
}

bool
optparser_has_next(OptParser *p)
{
    OptParserState tmp_state = p->parse_state;
    int token = look_for_token(p, &tmp_state, true);
    return token != OPTP_DONE && token != OPTP_ERROR;
}

int
optparser_next(OptParser *p)
{
    return look_for_token(p, &p->parse_state, true);
}

bool
optparser_has_next_arg(OptParser *p)
{
    OptParserState tmp_state = p->parse_state;
    int token = look_for_token(p, &tmp_state, false);
    return token != OPTP_DONE && token != OPTP_ERROR;
}

char *
optparser_next_arg(OptParser *p)
{
    look_for_token(p, &p->parse_state, false);
    return p->parse_state.value;
}

char *
optparser_value(OptParser *p)
{
    return p->parse_state.value;
}

char *
optparser_error(OptParser *p)
{
    return p->parse_state.error;
}
