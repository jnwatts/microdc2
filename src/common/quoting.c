/* quoting.c - Various routines for working with quoted strings
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

/* TODO: unified handling of whitespace (single point of definition),
 * both as string and function?
 */

#if HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdbool.h>		/* Gnulib/C99/POSIX */
#include <string.h>		/* C89 */
#include <stdint.h>		/* Gnulib/C99/POSIX */
#include <ctype.h>		/* C89 */
#include "xalloc.h"		/* Gnulib */
#include "strbuf.h"
#include "quoting.h"

#define HEX_TO_INT(x) \
    ( (x)>='0' && (x)<='9' ? (x)-'0' : ((x)>='A' && (x)<='F' ? (x)-'A'+10 : (x)-'a'+10 ))
#define IS_OCT_DIGIT(d) ( (d) >= '0' && (d) <= '7' )

/* Quote STRING with double quotes if QUOTED is true, otherwise
 * by escaping whitespace, double quote and backslash as well
 * as characters in QC. Also, if the first character in STRING
 * is found in LEADING_QC, that character will be escaped.
 */
char *
quote_word_full(const char *string, bool quoted, bool add_end_quote,
		const char *qc, const char *leading_qc, bool quote_non_print_hex,
		bool quote_non_print_oct, bool quote_non_print_c, bool quote_wc)
{
    StrBuf *r = strbuf_new();
    const char *s;
    const unsigned char *u_s;

    if (quoted) {
        strbuf_append_char(r, '"');
        for (s = string; *s != '\0'; s++) {
	    if (quote_non_print_c) {
		int chr = 0;
		switch (*s) {
		case '\a': chr = 'a'; break;
		case '\b': chr = 'b'; break;
		case '\f': chr = 'f'; break;
		case '\n': chr = 'n'; break;
		case '\r': chr = 'r'; break;
		case '\t': chr = 't'; break;
		case '\v': chr = 'v'; break;
		}
		if (chr != 0) {
		    strbuf_appendf(r, "\\%c", chr);
		    continue;
		}
	    }
	    u_s = (const unsigned char *)s;
            if (quote_non_print_hex && !isprint(*u_s)) {
                strbuf_appendf(r, "\\x%02x", (unsigned)*s);
	    } else if (quote_non_print_oct && !isprint(*u_s)) {
		strbuf_appendf(r, "\\%03o", (unsigned)*s);
            } else {
                if (*s == '"' || *s == '\\')
                    strbuf_append_char(r, '\\');
                strbuf_append_char(r, *s);
            }
        }
        if (add_end_quote)
            strbuf_append_char(r, '"');
    } else {
        for (s = string; *s != '\0'; s++) {
	    if (quote_non_print_c) {
		int chr = 0;
		switch (*s) {
		case '\a': chr = 'a'; break;
		case '\b': chr = 'b'; break;
		case '\f': chr = 'f'; break;
		case '\n': chr = 'n'; break;
		case '\r': chr = 'r'; break;
		case '\t': chr = 't'; break;
		case '\v': chr = 'v'; break;
		}
		if (chr != 0) {
		    strbuf_appendf(r, "\\%c", chr);
		    continue;
		}
	    }
	    u_s = (const unsigned char *)s;
            if (quote_non_print_hex && !isprint(*u_s)) {
                strbuf_appendf(r, "\\x%02x", (unsigned)*s);
	    } else if (quote_non_print_oct && !isprint(*u_s)) {
		strbuf_appendf(r, "\\%03o", (unsigned)*u_s);
	    } else {
	        if (*s == '"'
      	                || *s == '\\'
	                || (quote_wc && *s == ' ')
  		        || strchr(qc, *s) != NULL
		        || (s == string && strchr(leading_qc, *u_s) != NULL))
		    strbuf_append_char(r, '\\');
                strbuf_append_char(r, *s);
            }
        }
    }

    return strbuf_free_to_string(r);
}

/* Remove quotes and backslashs from STR, looking no further than
 * MAXEND, or if MAXEND is null, where STR ends. MODE specifies
 * initial quoting - false for no quoting, true for double quotes.
 */
char *
dequote_words_full(const char *str, bool quoted, bool c_hex_unescape, bool c_oct_unescape, bool c_simple_unescape, const char *maxend)
{
    const char *p;
    char *result;
    char *r;

    if (maxend == NULL)
	maxend = (void *) UINTPTR_MAX;

    result = r = xmalloc(strlen(str) + 1);
    for (p = str; p < maxend && *p != '\0'; p++) {
	if (*p == '\\') {
	    p++;
	    if (p >= maxend || *p == '\0')
		break; /* also discards bogus trailing backslash */
	    if (c_simple_unescape) {
		int chr = 0;
		switch (*p) {
		case 'a': chr = '\a'; break;
		case 'b': chr = '\b'; break;
		case 'f': chr = '\f'; break;
		case 'n': chr = '\n'; break;
		case 'r': chr = '\r'; break;
		case 't': chr = '\t'; break;
		case 'v': chr = '\v'; break;
		}
		if (chr != 0) {
		    *r++ = chr;
		    continue;
		}
	    }
	    if (c_oct_unescape && IS_OCT_DIGIT(*p)) {
		int chr = *p - '0';
		p++;
		if (p < maxend && IS_OCT_DIGIT(*p)) {
		    chr = (chr * 8) + (*p - '0');
		    p++;
		    if (p < maxend && IS_OCT_DIGIT(*p)) {
			chr = (chr * 8) + (*p - '0');
			p++;
		    }
		}
		p--;
		*r++ = chr;
		continue;
	    }
	    if (c_hex_unescape && *p == 'x') {
		int chr;
		const char *q = p+1;
		if (q >= maxend
			|| *q == '\0' 
			|| !isxdigit(*(const unsigned char *)q))
		    break;
		q++;
		p++;
		chr = HEX_TO_INT(*p);
		if (q < maxend && isxdigit(*(const unsigned char *)q)) {
		    p++;
		    chr = chr*16 + HEX_TO_INT(*p);
		}
		*r++ = chr;
		continue;
	    }
	    *r++ = *p;
	} else if (*p == '"') {
	    quoted = !quoted;
	} else {
	    *r++ = *p;
	}
    }
    *r = '\0';

    return result;
}

/* Find the first character which is part of the first word
 * in STR, i.e. skip past leading whitespace. Look no further
 * than MAXEND or where STR ends. If MAXEND is NULL, stop only
 * where STR ends. STR must be non-null. This function never
 * returns null.
 */
const char *
find_word_start(const char *str, const char *maxend)
{
    const char *whitespace = " \n\t";

    if (maxend == NULL)
        maxend = (void *) UINTPTR_MAX;

    for (; str < maxend && *str != '\0'; str++) {
        if (strchr(whitespace, *str) == NULL)
            break;
    }

    return str;
}

/* Find the first character which is not part of the first word
 * in STR, but look no at MAXEND or further, or where STR ends or
 * where an unquoted TERMCHAR is found.
 * If MAXEND is NULL, stop only where STR ends or TERMCHAR is
 * found. TERMCHAR can be specified as '\0'. STR must be non-null.
 * This function does not return null.
 */
const char *
find_word_end_termchar(const char *str, const char *maxend, char termchar)
{
    const char *whitespace = " \n\t";
    bool quoted = false;

    if (maxend == NULL)
        maxend = (void *) UINTPTR_MAX;

    str = find_word_start(str, maxend);
    for (; str < maxend && *str != '\0'; str++) {
        if (*str == '\\') {
            str++;
            if (str >= maxend || *str == '\0')
                break;
        } else if (*str == '"') {
            quoted = !quoted;
        } else if (!quoted && (*str == termchar || strchr(whitespace, *str) != NULL)) {
            break;
        }
    }

    return str;
}

/* Return index of the word at position POS in the string STR.
 * This is also the index of the word that would be at POS -
 * there may not necessarily be a complete word at POS.
 */
int
get_word_index(const char *str, int pos)
{
    const char *maxend = str + pos;
    int index;

    for (index = 0; ; index++) {
        str = find_word_end(str, maxend);
        if (str >= maxend || *str == '\0')
            return index;
    }
}

const char *
find_last_unquoted_char(const char *str, const char *maxend, char ch)
{
    bool quoted = false;
    const char *match = NULL;

    if (maxend == NULL)
        maxend = (void *) UINTPTR_MAX;

    for (; str < maxend && *str != '\0'; str++) {
        if (*str == '\\') {
            str++;
            if (str >= maxend || *str == '\0')
                break;
        } else if (*str == '"') {
            quoted = !quoted;
        } else if (!quoted && *str == ch) {
            match = str;
        }
    }

    return match;
}

/* find_unquoted_char is much like find_word_end_termchar,
 * except that we treat whitespace like regular characters.
 * This function will also return NULL. This function is
 * used to find unquoted semicolons (`;').
 */
const char *
find_unquoted_char(const char *str, const char *maxend, char ch)
{
    bool quoted = false;

    if (maxend == NULL)
        maxend = (void *) UINTPTR_MAX;

    for (; str < maxend && *str != '\0'; str++) {
        if (*str == '\\') {
            str++;
            if (str >= maxend && *str == '\0')
                break;
        } else if (*str == '"') {
            quoted = !quoted;
        } else if (!quoted && *str == ch) {
            return str;
        }
    }

    return NULL;
}

/* Find a character which is the leading character in an unquoted
 * word. This is used to detect comments introduced by number sign
 * (`#').
 */
const char *
find_unquoted_leading_char(const char *str, const char *maxend, char ch)
{
    const char *whitespace = " \n\t";
    bool quoted = false;
    bool word = false;

    if (maxend == NULL)
        maxend = (void *) UINTPTR_MAX;

    for (; str < maxend && *str != '\0'; str++) {
        if (*str == '\\') {
            str++;
            if (str >= maxend && *str == '\0')
                break;
        } else if (*str == '"') {
            quoted = !quoted;
        } else if (!quoted) {
	    if (strchr(whitespace, *str) != NULL) {
		word = false;
	    } else {
		if (!word && *str == ch)
		    return str;
		word = true;
	    }
        }
    }

    return NULL;
}

/* Get the word in STR that would be used for completion if 
 * the user were to press tab at POS. This function may return
 * partial words in STR, or even the empty string.
 */
char *
get_completion_word_dequoted(const char *str, int pos)
{
    /*return dequote_words(find_completion_word_start(str, pos), false, str+pos);*/
    const char *maxend = str + pos;
    const char *start = str;
    int index;

    for (index = 0; ; index++) {
        str = find_word_end(str, maxend);
        if (str >= maxend || *str == '\0') {
	    start = find_word_start(start, maxend);
	    if (start >= maxend || *start == '\0')
		return xstrdup("");
	    return dequote_words(start, false, maxend);
	}
	start = str;
    }
}

int
find_completion_word_start(const char *str, int pos)
{
    const char *maxend = str + pos;
    const char *start = str;
    const char *end = str;
    int index;

    for (index = 0; ; index++) {
        end = find_word_end(end, maxend);
        if (end >= maxend || *end == '\0') {
	    start = find_word_start(start, maxend);
	    if (start >= maxend || *start == '\0')
		return pos;
	    return start-str;
	}
	start = end;
    }
}

/* Split input into an array of words. The array will be null-terminated.
 *
 * XXX: this function has not been tested
 */
char **
get_word_array_dequoted(const char *str, const char *strend, int *argc)
{
    size_t array_cur;
    size_t array_max;
    char **array;

    if (strend == NULL)
        strend = (void *) UINTPTR_MAX;

    array_cur = 0;
    array_max = 8;
    array = xmalloc(array_max * sizeof(char *));

    str = find_word_start(str, strend);
    if (str < strend && *str != '\0') {
        while (str < strend && *str != '\0') {
            const char *end;
            end = find_word_end(str, strend);
            if (array_cur >= array_max) {
                array_max *= 2;
                array = xrealloc(array, array_max * sizeof(char *));
            }
            array[array_cur++] = dequote_words(str, false, end);
            if (end >= strend || *end == '\0')
                break;
            str = end;
        }
    }

    if (array_cur >= array_max) {
        array_max *= 2;
        array = xrealloc(array, array_max * sizeof(char *));
    }
    array[array_cur] = NULL;
    return array;
}

/* Return a newly allocated string of COUNT words starting at
 * INDEX in string STR. The returned string with be dequoted.
 * Do not look at or beyond STREND. TERMCHAR is a character which
 * signifies end of STR.
 *
 * Note: A non-null-byte TERMCHAR is generally only useful
 * when INDEX is 0. This is because TERMCHAR isn't counted
 * as a word, so a TERMCHAR in STR will mark the end of the
 * string - this function won't actually look beyond a
 * TERMCHAR in STR.
 */
char *
get_subwords_dequoted_termchar(const char *str, const char *strend, int index, size_t count, char termchar)
{
    const char *end;
    int c;

    /*if (str == NULL) // This is not necessary any longer!
	return NULL;*/

    if (strend == NULL)
        strend = (void *) UINTPTR_MAX;

    for (c = 0; c < index; c++) {
        str = find_word_end_termchar(str, strend, termchar);
        if (str >= strend || *str == '\0' || *str == termchar)
            return NULL;
    }

    str = find_word_start(str, strend);
    if (str >= strend || *str == '\0' || *str == termchar)
        return NULL;

    end = str;
    for (c = 0; c < count; c++) {
        end = find_word_end_termchar(end, strend, termchar);
        if (end >= strend || *end == '\0' || *end == termchar)
            break;
    }
    
    return dequote_words(str, false, end);
}

/* Determine if character INDEX in STRING is
 * inside a quoted string or right behind a backslash.
 */
int
char_is_quoted(char *string, int index)
{
    bool escaped = false;
    int c;

    for (c = 0; c <= index; c++) {
	if (escaped) {
	    if (c >= index)
		return 1;
	    escaped = false;
	} else if (string[c] == '"') {
	    char quote = string[c];
	    c++;
	    for (; c < index && string[c] != '\0' && string[c] != quote; c++) {
		if (string[c] == '\\' && string[c+1] != '\0')
		    c++;
	    }
	    if (c >= index)
		return 1;
	} else if (string[c] == '\\') {
	    escaped = true;
	}
    }

    return 0;
}

int
count_unquoted_chars(const char *str, const char *end)
{
    int count = 0;
    for (; *str && str < end; str++) {
        if (*str != '"' && *str != '\\')
            count++;
    }
    return count;
}
