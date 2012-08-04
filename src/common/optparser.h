/* optparser.h - Option parser that reports errors properly
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

#ifndef COMMON_OPTPARSER_H
#define COMMON_OPTPARSER_H

#include <stdarg.h>
#include <limits.h>
#include "tmap.h"

typedef enum {
    OPTP_PARSE_ARGV0,
//  OPTP_NO_ERRORS,
//  OPTP_NO_REORDER,
//  OPTP_RETURN_ARGS,
} OptParserConfig;

typedef enum {
    OPTP_NO_ARG,
    OPTP_OPT_ARG,
    OPTP_REQ_ARG,
} OptParserArgument;

typedef enum {
    OPTP_ERROR = -1,
    OPTP_DONE = 0,
    OPTP_ARG = 10000,
} OptParserResult;

typedef struct _OptParser OptParser;
typedef struct _OptDetail OptDetail;

struct _OptDetail {
    const char *names;
    OptParserArgument arg;
    int code;
};

/* Create a new option parser. The config options may require additional arguments,
 * thus the variadic optional arguments
 */
OptParser *optparser_new(OptDetail *opts, int count, OptParserConfig field, ...);

/* Start parsing the specified arguments.
 */
void optparser_parse(OptParser *parser, int argc, char **argv);

/* Return the next option. If OPTP_RETURN_ARGS is set, return the next option or
 * non-option argument (whichever comes first).
 */
int optparser_next(OptParser *parser);

/* Return the next non-option argument.
 */
char *optparser_next_arg(OptParser *parser);

/* This function returns false if the next call to optparser_next will return
 * OPTP_DONE. (OPTP_ERROR will be reported differently.)
 */
bool optparser_has_next(OptParser *p);

/* This function returns false if the next call to optparser_next_arg will return
 * OPTP_DONE. (OPTP_ERROR will be reported differently.)
 */
bool optparser_has_next_arg(OptParser *p);

/* Return the value associated with the current option
 * or the current non-option argument.
 */
char *optparser_value(OptParser *parser);

/* Return an error description if there was an error.
 * The returned string may not be used after optparser_free has been called.
 */
char *optparser_error(OptParser *parser);

/* Free all resources used by this parser.
 * No function may be called on the parser reference after this point.
 */
void optparser_free(OptParser *parser);

//int optparser_index(OptParser *parser);

#endif
