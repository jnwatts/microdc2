/* connection.c - Functions generic to hub and user connections
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
#include <ctype.h>		/* C89 */
#include "xalloc.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/strbuf.h"
#include "microdc.h"

/* Decode data from a $Lock command sent by either a client or a hub.
 * The returned value must be freed using free(). The length of the
 * returned key can be retrieved using strlen.
 *
 * XXX: this should be generalized and the error message removed.
 */
char *
decode_lock(const char *lock, size_t locklen, uint32_t basekey)
{
    uint8_t key[locklen];
    char *outkey;
    int c;
    int d;

    if (locklen < 3) {
    	screen_putf(_("Invalid $Lock message: Key to short\n"));
    	return xstrdup("");
    }

    key[0] = lock[0] ^ basekey;
    key[0] = (key[0] << 4) | (key[0] >> 4);
    for (c = 1; c < locklen; c++) {
    	key[c] = lock[c] ^ lock[c-1];
    	key[c] = (key[c] << 4) | (key[c] >> 4);
    }
    key[0] = key[0] ^ key[locklen-1];

    d = 1; /* 1 for nullbyte at end */
    for (c = 0; c < locklen; c++) {
    	switch (key[c]) {
	case 0:
	case 5:
	case 36:
	case 96:
	case 124:
	case 126:
	    d += 10;
	    break;
	default:
	    d++;
	    break;
	}
    }

    outkey = xmalloc(sizeof(char) * d);

    d = 0;
    for (c = 0; c < locklen; c++) {
    	switch (key[c]) {
	case 0:
	case 5:
	case 36:
	case 96:
	case 124:
	case 126:
	    sprintf(outkey+d, "/%%DCN%03d%%/", key[c]);
	    d += 10;
	    break;
	default:
	    outkey[d++] = key[c];
	    break;
	}
    }
    outkey[d] = '\0';

    return outkey;
}

/* XXX: This is somewhat crude... Generalize for all HTML/SGML escaping? */
char *
unescape_message(const char *str)
{
    char *out;
    char *cur;
    
    cur = out = xmalloc(strlen(str)+1);
    while (*str != '\0') {
        if (str[0] == '&') {
            if (str[1] == 'a' && str[2] == 'm' && str[3] == 'p' && str[4] == ';') {
                *cur++ = '&';
                str += 5;
                continue;
            } else if (str[1] == '#' && str[2] == '3' && str[3] == '6' && str[4] == ';') {
                *cur++ = '$';
                str += 5;
                continue;
            } else if (str[1] == '#' && str[2] == '1' && str[3] == '2' && str[4] == '4' && str[5] == ';') {
                *cur++ = '|';
                str += 6;
                continue;
            }
        }
        *cur++ = *str++;
    }
    *cur = '\0';
    return out;
}


/* Escape a string for putting in a command. The returned string must be
 * freed with free.
 */
char *
escape_message(const char *str)
{
    StrBuf *out;

    out = strbuf_new();
    for (; *str != '\0'; str++) {
    	switch (*str) {
    	case '$':
	    strbuf_append(out, "&#36;");
	    break;
	case '&':
	    strbuf_append(out, "&amp;");
	    break;
	case '|':
	    strbuf_append(out, "&#124;");
	    break;
	default:
	    strbuf_append_char(out, *str);
	    break;
	}
    }

    return strbuf_free_to_string(out);
}

/* Dump a received or sent piece of data.
 */
void
dump_command(const char *header, const char *buf, size_t len)/* XXX: uint32_t=>size_t, same for byteq */
{
    const unsigned char *ubuf;
    StrBuf *out;
    uint32_t c;

    ubuf = (const unsigned char *)buf;
    out = strbuf_new();
    for (c = 0; c < len; c++) {
        if (isprint(ubuf[c]))
            strbuf_appendf(out, "%c", ubuf[c]);
        else
            strbuf_appendf(out, "\\x%02x", ubuf[c] & 0xFF);
    }
    flag_putf(DC_DF_DEBUG, "%s %s\n", header, out->buf);
    strbuf_free(out);
}
