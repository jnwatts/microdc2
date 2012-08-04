/* range.c - Parsing of simple range expressions
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
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <config.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include "range.h"

/* foreach_in_range implements parsing of simple range expressions
 * such as "a,b-c,d-,-e" etc. It implements the following yacc/bison
 * parser:
 */
/*
%token NUM
%%
input	: // empty
	| exprs
	;
exprs	: expr
	| exprs ',' expr
	;
expr	: NUM '-' NUM
	| NUM '-'
	| '-' NUM
	| NUM
	;
*/

#define DIGIT_A2I(a) ( (a)>='a' ? (a)-'a' : (a)>='A' ? (a)-'A' : (a)-'0' )

typedef struct {
    char type; /* 0 = EOF, '#' = NUM */
    uint32_t num;
} RangeToken;

static void
pop_token(const char **range, RangeToken *token)
{
    if (isdigit(**range)) {
        token->type = '#';
        token->num = DIGIT_A2I(**range);
        for ((*range)++; isdigit(**range); (*range)++)
            token->num = token->num * 10 + DIGIT_A2I(**range);
    } else {
        token->type = **range;
        (*range)++;
    }
}

bool
foreach_in_range(const char *range, uint32_t start, uint32_t end, RangeCallback callback, void *userdata)
{
    RangeToken t;

    pop_token(&range, &t);
    if (t.type == 0)
        return true;

    for (;;) {
        if (t.type == '-') {
	    pop_token(&range, &t);
	    if (t.type != '#')
                return false;
            if (t.num < start || t.num > end)
                return false;
            if (callback != NULL)
                callback(start, t.num, userdata);
            pop_token(&range, &t);
            if (t.type == 0)
                return true;
            if (t.type != ',')
                return false;
        } else if (t.type == '#') {
            RangeToken t2;

            if (t.num < start || t.num > end)
	        return false;
	    pop_token(&range, &t2);
	    if (t2.type == 0) {
                if (callback != NULL)
                    callback(t.num, t.num, userdata);
	        return true;
	    }
	    if (t2.type == ',') {
                if (callback != NULL)
                    callback(t.num, t.num, userdata);
	    } else if (t2.type == '-') {
	        pop_token(&range, &t2);
	        if (t2.type == 0) {
                    if (callback != NULL)
                        callback(t.num, end, userdata);
                    return true;
                }
                if (t2.type == ',') {
                    if (callback != NULL)
                        callback(t.num, end, userdata);
	        } else if (t2.type == '#') {
                    if (t2.num < start || t2.num > end)
	                return false;
                    if (callback != NULL)
                        callback(t.num, t2.num, userdata);
                    pop_token(&range, &t);
                    if (t.type == 0)
		        return true;
		    if (t.type != ',')
                        return false;
                } else {
                    return false;
                }
	    }
        } else {
            return false;
        }

        pop_token(&range, &t);
    }
}
