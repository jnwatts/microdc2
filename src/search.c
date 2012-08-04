/* search.c - Search support
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
#include <assert.h>		/* ? */
#include <string.h>		/* C89 */
#include <arpa/inet.h>		/* ? */
#include <sys/socket.h>		/* ? */
#include <netinet/in.h>		/* ? */
#include <inttypes.h>		/* ? */
#include <time.h>		/* ? */
#include "xalloc.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/strbuf.h"
#include "common/intutil.h"
#include "microdc.h"

//#define _TRACE
#if defined(_TRACE)
#include <stdio.h>
#define TRACE(x) printf x; fflush(stdout)
#else
#define TRACE(x)
#endif

#define MAX_RESULTS_ACTIVE 10		/* Max number of search results to send to active users */
#define MAX_RESULTS_PASSIVE 5		/* Max number of search results to send to passive users */

PtrV *our_searches;

static char *extensions[] = { // NULL means match any extension
    /* ANY */ NULL,
    /* AUDIO */ "mp3/mp2/wav/au/rm/mid/sm",
    /* COMPRESSED */ "zip/arj/rar/lzh/gz/z/arc/pak",
    /* DOCUMENTS */ "doc/txt/wri/pdf/ps/tex",
    /* EXECUTABLES */ "pm/exe/bat/com",
    /* PICTURES */ "gif/jpg/jpeg/bmp/pcx/png/wmf/psd",
    /* VIDEO */ "mpg/mpeg/avi/asf/mov",
    /* FOLDERS */ NULL,
    /* CHECKSUM */ NULL,
};

static int
compare_search_response(DCSearchResponse *sr1, DCSearchResponse *sr2)
{
    /* refcount is not derived from results - not compared. */
    COMPARE_RETURN_FUNC(strcmp(sr1->userinfo->nick, sr2->userinfo->nick));
    COMPARE_RETURN_FUNC(strcmp(sr1->filename, sr2->filename));
    COMPARE_RETURN(sr1->filetype, sr2->filetype);
    COMPARE_RETURN(sr1->filesize, sr2->filesize);
    /* slots_free and slots_total may change - not compared. */
    /* hub_name and hub_addr may be incomplete - not compared. */
    /* XXX: when/if multiple hub connections are supported, we probably need
     * to compare hub_name and/or hub_addr as well.
     */
    return 0;
}

void
search_string_new(DCSearchString *sp, const char *p, int len)
{
    int c;
    unsigned char *u_str;

    u_str = xstrndup(p, len);
    for (c = 0; c < len; c++)
    	u_str[c] = tolower(u_str[c]);
    sp->len = len;
    sp->str = u_str;

    for (c = 0; c < 256; c++)
    	sp->delta[c] = len+1;
    for (c = 0; c < len; c++)
    	sp->delta[ u_str[c] ] = len-c;
}

void
search_hash_new(DCSearchString *sp, const char *p, int len)
{
    int c;

    sp->str = xstrndup(p, len);
    for (c = 0; c < len; c++)
    	sp->str[c] = toupper(sp->str[c]);
    sp->len = len;

    for (c = 0; c < 256; c++)
    	sp->delta[c] = len+1;
    for (c = 0; c < len; c++)
    	sp->delta[(uint8_t) sp->str[c]] = len-c;
}

void
search_string_free(DCSearchString *sp)
{
    free(sp->str);
}

static bool
parse_hash(char *str, DCSearchSelection *ss)
{
    int len = strlen(str);
    flag_putf(DC_DF_DEBUG, _("incoming hash: %s\n"), str);
    if (len > 4 && str[3] == ':' &&
        (str[0] == 't' || str[0] == 'T') && 
        (str[1] == 't' || str[1] == 'T') && 
        (str[2] == 'h' || str[2] == 'H')) {
        /* TTH hash lookup */

        ss->patterncount = 1;
        ss->patterns = xmalloc(sizeof(DCSearchString));
        search_hash_new(ss->patterns, str+4, len-4);
        return true;
    }
    return false;
}

static bool
parse_search_strings(char *str, DCSearchSelection *ss)
{
    uint32_t c;
    char *t1;
    char *t2;

    ss->patterncount = 0;
    for (t1 = str; (t2 = strchr(t1, '$')) != NULL; t1 = t2+1) {
    	if (t2 != str && t2[-1] != '$')
	    ss->patterncount++;
    }
    if (*t1)
        ss->patterncount++;

    if (ss->patterncount == 0)
	    return false;

    ss->patterns = xmalloc(sizeof(DCSearchString) * ss->patterncount);

    c = 0;
    for (t1 = str; (t2 = strchr(t1, '$')) != NULL; t1 = t2+1) {
    	if (t2 != str && t2[-1] != '$') {
    	    search_string_new(ss->patterns+c, t1, t2-t1);
	        c++;
	    }
    }
    if (*t1)
       search_string_new(ss->patterns+c, t1, strlen(t1));

    return true;
}

static bool
match_file_extension(const char *filename, DCSearchDataType type)
{
    char *ext;
    char *t1;
    char *t2;

    t1 = extensions[type];
    if (t1 == NULL)
	return true;

    ext = strrchr(filename, '.');
    if (ext == NULL)
	return false;
    ext++;

    for (; (t2 = strchr(t1, '/')) != NULL; t1 = t2+1) {
	if (strncasecmp(ext, t1, t2-t1) == 0)
	    return true;
    }
    if (strcasecmp(ext, t1) == 0)
	return true;

    return false;
}

int
parse_search_selection(char *str, DCSearchSelection *data)
{
    char sizeres;
    char sizemin;
    char *sizestr;
    uint64_t size;
    char *datatype;
    char *tmpstr;
    bool ret;

    if (str[0] != 'T' && str[0] != 'F')
    	return 0;
    if (str[1] != '?')
    	return 0;
    if (str[2] != 'T' && str[2] != 'F')
    	return 0;
    if (str[3] != '?')
    	return 0;
    sizeres = str[0];
    sizemin = str[2];

    str += 4;
    sizestr = strsep(&str, "?");
    if (sizestr == NULL)
    	return 0;
    if (!parse_uint64(sizestr, &size))
    	return 0;
    datatype = strsep(&str, "?");
    if (datatype == NULL || datatype[0] < '1' || datatype[0] > '9' || datatype[1] != '\0')
    	return 0;
    if (*str == '\0')
    	return 2;
    if (strlen(str) >= 1 << 16) /* Needed for delta match */
    	return 0;

    if (sizeres) {
        if (sizemin) {
            data->size_min = size;
            data->size_max = UINT64_MAX;
        } else {
            data->size_min = 0;
            data->size_max = size;
        }
    } else {
        data->size_min = 0;
        data->size_max = UINT64_MAX;
    }
    data->datatype = datatype[0]-'1';

    if (data->datatype == DC_SEARCH_CHECKSUM) {
        return parse_hash(str, data);
    }

    /* convert search string from hub charset to local charset */
    tmpstr = hub_to_main_string(str);
    ret = parse_search_strings(tmpstr, data);
    free (tmpstr);

    return ret;
}

static bool
match_search_pattern(const char *t, DCSearchString *pattern)
{
    const char *end;
    const unsigned char *ut;
    uint32_t tlen;

    tlen = strlen(t);
    if (tlen < pattern->len)
    	return false;

    end = t + tlen - pattern->len + 1;
    while (t < end) {
    	uint32_t i = 0;
	ut = (const unsigned char *)t;
	for (; pattern->str[i] && pattern->str[i] == (char)tolower(ut[i]); i++)
	    ;
	if (pattern->str[i] == '\0')
	    return true;
	t += pattern->delta[(uint8_t) tolower(ut[pattern->len])];
    }
    return false;
}

static bool
match_search_patterns(const char *text, DCSearchSelection *data)
{
    uint32_t c;
    for (c = 0; c < data->patterncount; c++) {
	if (!match_search_pattern(text, data->patterns+c))
	    return 0;
    }
    return 1;
}

static void
append_result(DCFileList *node, DCUserInfo *ui, struct sockaddr_in *addr)
{
    StrBuf *sb;
    char *lpath;
    char *rpath;
    char *conv_rpath;
    char *hub_my_nick;
    char *hub_hub_name;
    char *hub_ui_nick;
    int free_slots;

    sb = strbuf_new();
    lpath = filelist_get_path(node);
    rpath = translate_local_to_remote(lpath);
    free(lpath);

    free_slots = used_ul_slots > my_ul_slots ? 0 : my_ul_slots-used_ul_slots;

    conv_rpath = main_to_hub_string(rpath);
    hub_my_nick = main_to_hub_string(my_nick);
    hub_hub_name = main_to_hub_string(hub_name);
    if ( ui != NULL)
        hub_ui_nick = main_to_hub_string(ui->nick);
    else
        hub_ui_nick = NULL;

    strbuf_appendf(sb, "$SR %s %s", hub_my_nick, conv_rpath);

    if (node->type == DC_TYPE_REG)
    	strbuf_appendf(sb, "\x05%" PRIu64, node->size);
    strbuf_appendf(sb, " %d/%d\x05", free_slots, my_ul_slots);
    if (node->type == DC_TYPE_REG && node->reg.has_tth) {
        unsigned char tth[40];
        memcpy(tth, node->reg.tth, sizeof(node->reg.tth));
        tth[39] = '\0';
        strbuf_appendf(sb, "TTH:%s", tth);
    } else {
        strbuf_appendf(sb, "%s", hub_hub_name);
    }
    strbuf_appendf(sb, " (%s)", sockaddr_in_str(&hub_addr));
    if (ui != NULL)
    	strbuf_appendf(sb, "\x05%s", hub_ui_nick);
    strbuf_append(sb, "|");

    if (ui != NULL) {
    	hub_putf("%s", sb->buf); /* want hub_put here */
    } else {
    	add_search_result(addr, sb->buf, strbuf_length(sb));
    }
    
    free(hub_ui_nick);
    free(hub_hub_name);
    free(hub_my_nick);
    free(conv_rpath);   
    strbuf_free(sb);
}

static int
filelist_search(DCFileList *node, DCSearchSelection *data, int maxresults, DCUserInfo *ui, struct sockaddr_in *addr)
{
    assert(maxresults > 0);

    if (node->type == DC_TYPE_REG) {
        if (data->datatype == DC_SEARCH_CHECKSUM) {
            if (!node->reg.has_tth)
                return 0;
            if (strncmp(node->reg.tth, data->patterns->str, sizeof(node->reg.tth)) != 0) { /* only TTH is supported up to now */
                return 0;
            }
        } else {
    	    if (data->datatype == DC_SEARCH_FOLDERS)
	            return 0;
    	    if (node->size < data->size_min)
	            return 0;
	        if (node->size > data->size_max)
	            return 0;
	        if (!match_search_patterns(node->name, data))
	            return 0;
	        if (!match_file_extension(node->name, data->datatype))
	            return 0;
        }
	    append_result(node, ui, addr);
	    return 1;
    }

    if (node->type == DC_TYPE_DIR) {
    	HMapIterator it;
    	int curresults = 0;

    	if (data->datatype == DC_SEARCH_ANY || data->datatype == DC_SEARCH_FOLDERS) {
	        if (match_search_patterns(node->name, data)) {
                append_result(node, ui, addr);
            	curresults++;
		        if (curresults >= maxresults)
                    return curresults;
	        }
	    }

	    hmap_iterator(node->dir.children, &it);
	    while (it.has_next(&it)) {
            DCFileList *subnode = it.next(&it);
	        curresults += filelist_search(subnode, data, maxresults-curresults, ui, addr);
            if ((data->datatype == DC_SEARCH_CHECKSUM && curresults > 0) ||
	            (curresults >= maxresults))
                break;
    	}
	
	    return curresults;
    }

    return 0;
}

//XXX: assumes our_filelist != NULL
bool
perform_inbound_search(DCSearchSelection *data, DCUserInfo *ui, struct sockaddr_in *addr)
{
    int maxresults;
    int curresults;

    TRACE(("%s:%d: \n", __FUNCTION__, __LINE__));

    maxresults = (ui == NULL ? MAX_RESULTS_ACTIVE : MAX_RESULTS_PASSIVE);
    curresults = filelist_search(our_filelist, data, maxresults, ui, addr);

    if (curresults > 0) {
        if (ui != NULL) {
            flag_putf(DC_DF_DEBUG, _("Sent %d/%d search results to %s.\n"), curresults, maxresults, quotearg(ui->nick));
        } else {
            flag_putf(DC_DF_DEBUG, _("Sent %d/%d search results to %s.\n"), curresults, maxresults, sockaddr_in_str(addr));
        }
    } else {
        flag_putf(DC_DF_DEBUG, _("No search results.\n"));
    }

    return false;
}

/* Parse a $SR message into a DCSearchResponse */
static DCSearchResponse *
parse_search_response(char *buf, uint32_t len)
{
    DCSearchResponse *sr;
    DCUserInfo *ui;
    char *token;
    char *filename;
    uint64_t filesize;
    DCFileType filetype;
    uint32_t slots_free;
    uint32_t slots_total;
    char *hub_name;
    char *local_nick;
    struct sockaddr_in hub_addr;

    if (strncmp(buf, "$SR ", 4) != 0)
    	return NULL; /* Invalid $SR message: Not starting with $SR. */

    buf += 4;
    token = strsep(&buf, " ");
    if (token == NULL)
    	return NULL; /* Invalid $SR message: Missing user. */
    local_nick = hub_to_main_string(token);
    ui = hmap_get(hub_users, local_nick);
    free(local_nick);

    if (ui == NULL)
    	return NULL; /* Invalid $SR message: Unknown user. */

    /* Why look for `/' here?
     * A search result looks like this:
     * Directories: $SR <nick><0x20><directory><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
     * Files:       $SR <nick><0x20><filename><0x05><filesize><0x20><free slots>/<total slots><0x05><Hubname><0x20>(<Hubip:port>)
     * In the directories case, the only way to determine where the file
     * name ends is to look for `/' because it delimits the free slots and
     * total slots value. It is assumed that file names cannot contain the
     * slash (which is true for both *nix and windows).
     */

    filename = strsep(&buf, "/");
    if (filename == NULL)
    	return NULL; /* Invalid $SR message: Missing filename. */
    buf[-1] = '/';
    for (; filename < buf && *buf != ' '; buf--);
    if (filename == buf)
    	return NULL; /* Invalid $SR message: Missing free slots. */
    *buf = '\0';

	filename = hub_to_main_string(filename);
    if (filename == NULL)
        return NULL; /* Invalid $SR message: not convertable to local charset */

    buf++;
    token = strchr(filename, '\x05');
    if (token == NULL) {
        filetype = DC_TYPE_DIR;
    	filesize = 0;
    } else {
        filetype = DC_TYPE_REG;
        *token = '\0';
        if (!parse_uint64(token+1, &filesize)) {
            free(filename);
            return NULL; /* Invalid $SR message: Invalid file size */
        }
    }

    token = strsep(&buf, "/");
    assert(token != NULL);
    if (!parse_uint32(token, &slots_free)) {
        free(filename);
        return NULL; /* Invalid $SR message: Invalid free slots. */
    }
    token = strsep(&buf, "\x05");
    if (token == NULL) {
        free(filename);
        return NULL; /* Invalid $SR message: Missing total slots. */
    }
    if (!parse_uint32(token, &slots_total)) {
        free(filename);
        return NULL; /* Invalid $SR message: Invalid total slots. */
    }

    hub_name = buf;
    token = strrchr(buf, '(');
    if (token == NULL) {
        free(filename);
        return NULL; /* Invalid $SR message: Missing hub address. */
    }
    *token = '\0';
    buf = token+1;
    token = strchr(buf, ')');
    /* NeoModus DirectConnect 2.20 doesn't appear to add `)|' to $SR
     * messages. Or some other client which puts 'DC V:2.20' in the
     * client tag.
     */
    if (token != NULL)
        *token = '\0';

    token = strchr(buf, ':');
    if (token != NULL) {
        *token = '\0';
        if (!parse_uint16(token+1, &hub_addr.sin_port)) {
            free(filename);
            return NULL; /* Invalid $SR message: Invalid hub port. */
        }
        hub_addr.sin_port = htons(hub_addr.sin_port);
    } else {
    	hub_addr.sin_port = htons(DC_HUB_TCP_PORT);
    }
    if (!inet_aton(buf, &hub_addr.sin_addr)) {
        free(filename);
        return NULL; /* Invalid $SR message: Invalid hub address. */
    }

    sr = xmalloc(sizeof(DCSearchResponse));
    sr->userinfo = ui;
    ui->refcount++;
    sr->filename = filename;
    sr->filetype = filetype;
    sr->filesize = filesize;
    sr->slots_free = slots_free;
    sr->slots_total = slots_total;
    sr->hub_name = xstrdup(hub_name);
    sr->hub_addr = hub_addr;
    sr->refcount = 1;

    return sr;
}

static void
free_search_response(DCSearchResponse *sr)
{
    sr->refcount--;
    if (sr->refcount == 0) {
	user_info_free(sr->userinfo);
	free(sr->filename);
	free(sr->hub_name);
	free(sr);
    }
}

static bool
match_selection_against_response(DCSearchSelection *ss, DCSearchResponse *sr)
{
    if (sr->filetype == DC_TYPE_DIR) {
    	if (ss->datatype != DC_SEARCH_ANY && ss->datatype != DC_SEARCH_FOLDERS)
    	    return false;
    	if (ss->datatype == DC_SEARCH_CHECKSUM)
    	    return false;
	if (!match_search_patterns(sr->filename, ss))
	    return false;
	return true;
    } else {
    	if (ss->datatype == DC_SEARCH_FOLDERS)
    	    return false;
	
//	if (ss->datatype == DC_SEARCH_CHECKSUM) /* TTH not supported yet */
//	    return false;
    	if (sr->filesize < ss->size_min || sr->filesize > ss->size_max)
	    return false;
	if (!match_search_patterns(sr->filename, ss))
	    return false;
	if (!match_file_extension(sr->filename, ss->datatype))
	    return false;
	return true;
    }
}

static int
compare_search_selection(DCSearchSelection *s1, DCSearchSelection *s2)
{
    uint32_t c;

    COMPARE_RETURN(s1->size_min, s2->size_min);
    COMPARE_RETURN(s1->size_max, s2->size_max);
    COMPARE_RETURN(s1->datatype, s2->datatype);
    COMPARE_RETURN(s1->patterncount, s2->patterncount);
    for (c = 0; c < s1->patterncount; c++) {
    	COMPARE_RETURN(s1->patterns[c].len, s2->patterns[c].len);
    	COMPARE_RETURN_FUNC(memcmp(s1->patterns[c].str, s2->patterns[c].str, s1->patterns[c].len));
    }

    return 0;
}

bool
add_search_request(char *args)
{
    DCSearchSelection sel;
    DCSearchRequest *sr = NULL;
    uint32_t c;
    time_t now;
    char *hub_args;

    for (c = 0; args[c] != '\0'; c++) {
    	if (args[c] == '|' || args[c] == ' ')
	    args[c] = '$';
    }

    sel.size_min = 0;
    sel.size_max = UINT64_MAX;
    sel.datatype = DC_SEARCH_ANY;
    if (!parse_search_strings(args, &sel)) {
        int i = 0;
        if (sel.patterns != NULL) {
            for (i = 0; i < sel.patterncount; i++) {
                search_string_free(sel.patterns+i);
            }
            free(sel.patterns);
        }

        warn(_("No pattern to match.\n"));
        return false;
    }

    for (c = 0; c < our_searches->cur; c++) {
    	sr = our_searches->buf[c];
    	if (compare_search_selection(&sel, &sr->selection) == 0)
            break;
    }

    if (time(&now) == (time_t) -1) {
        warn(_("Cannot get current time - %s\n"), errstr);
        if (sel.patterns != NULL) {
            int i = 0;
            for (i = 0; i < sel.patterncount; i++) {
                search_string_free(sel.patterns+i);
            }
            free(sel.patterns);
        }
        return false;
    }

    if (c < our_searches->cur) {
    	screen_putf(_("Reissuing search %d.\n"), c+1);
        if (sel.patterns != NULL) {
            int i = 0;
            for (i = 0; i < sel.patterncount; i++) {
                search_string_free(sel.patterns+i);
            }
            free(sel.patterns);
        }
        sr->issue_time = now;
    } else {
        screen_putf(_("Issuing new search with index %d.\n"), c+1);
        sr = xmalloc(sizeof(DCSearchRequest));
        sr->selection = sel;
        sr->responses = ptrv_new();
        sr->issue_time = now;
        ptrv_append(our_searches, sr);
    }


    /* convert search string from local to hub charset */
    hub_args = main_to_hub_string(args);

    if (is_active) {
        hub_putf("$Search %s:%u F?F?0?1?%s|", inet_ntoa(local_addr.sin_addr), listen_port, hub_args);
    } else {
        char *hub_my_nick;
        hub_my_nick = main_to_hub_string(my_nick);
        hub_putf("$Search Hub:%s F?F?0?1?%s|", hub_my_nick, hub_args);
        free(hub_my_nick);
    }

    free(hub_args);

    return true;
}

void
free_search_request(DCSearchRequest *sr)
{
    int i = 0;
    for (i = 0; sr->selection.patterns != NULL && i < sr->selection.patterncount; i++) {
        search_string_free(sr->selection.patterns+i);
    }
    free(sr->selection.patterns);
    ptrv_foreach(sr->responses, (PtrVForeachCallback) free_search_response);
    ptrv_free(sr->responses);
}

void
handle_search_result(char *buf, uint32_t len)
{
    DCSearchResponse *sr;
    uint32_t c;
    time_t now;

    if (time(&now) == (time_t) -1) {
        warn(_("Cannot get current time - %s\n"), errstr);
        return;
    }

    sr = parse_search_response(buf, len);
    if (sr == NULL) {
        warn(_("Unterminated or invalid $SR, discarding: %s\n"), quotearg_mem(buf, len));
        return;
    }

    for (c = 0; c < our_searches->cur; c++) {
    	DCSearchRequest *sd = our_searches->buf[c];

        if (sd->issue_time + SEARCH_TIME_THRESHOLD <= now)
            continue;

        if (match_selection_against_response(&sd->selection, sr)) {
            char *fmt;
            uint32_t d;

            /* This is slow, but better than dupes... */
            for (d = 0; d < sd->responses->cur; d++) {
                if (compare_search_response(sd->responses->buf[d], sr) == 0) {
                    screen_putf(_("Result has been added earlier to search %d.\n"), c+1);
                    break;
                }
            }
            if (d >= sd->responses->cur) {
                ptrv_append(sd->responses, sr);
                fmt = ngettext("Added result to search %d (now %d result).\n",
                               "Added result to search %d (now %d results).\n",
                               sd->responses->cur);
                flag_putf(DC_DF_SEARCH_RESULTS, fmt, c+1, sd->responses->cur);
                sr->refcount++;
            }
        }
    }

    free_search_response(sr);
}

char *
search_selection_to_string(DCSearchSelection *sr)
{
    StrBuf *out;
    uint32_t c;

    out = strbuf_new_from_char('"');
    for (c = 0; c < sr->patterncount; c++) {
        if (c != 0)
            strbuf_append_char(out, ' ');
        strbuf_append_substring(out, sr->patterns[c].str, 0, sr->patterns[c].len);
    }
    strbuf_append_char(out, '"');

    return strbuf_free_to_string(out);
}
