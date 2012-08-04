/* filelist.c - File list parsing
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
#include <unistd.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <signal.h>		/* POSIX.1 */
#include "gettext.h"            /* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xalloc.h"             /* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "full-read.h"		/* Gnulib */
#include "iconvme.h"
#include "common/msgq.h"
#include "common/byteq.h"
#include "common/ptrv.h"
#include "common/intutil.h"
#include "microdc.h"

//#define _TRACE
#include <stdio.h>
#if defined(_TRACE)
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

struct _DCFileListParse {
    DCFileListParseCallback callback;
    void *data;
    bool cancelled;
};

static PtrV *pending_parses;
MsgQ *parse_request_mq = NULL;
MsgQ *parse_result_mq = NULL;
pid_t parse_child;

static size_t
calculate_filelist_data_size(DCFileList *node)
{
    size_t size;
    HMapIterator it;

    if (node->type == DC_TYPE_REG)
        return sizeof(DCFileType) + strlen(node->name)+1 + sizeof(uint64_t) + 1 + sizeof(node->reg.tth) + sizeof(time_t);

    size = sizeof(DCFileType) + strlen(node->name)+1 + 1 + (node->dir.real_path != NULL ? strlen(node->dir.real_path)+1 : 0) + sizeof(size_t);
    hmap_iterator(node->dir.children, &it);
    while (it.has_next(&it)) {
        DCFileList *child_node = it.next(&it);
        size += calculate_filelist_data_size(child_node);
    }

    return size;
}

static char *
copy_filelist_to_data(DCFileList *node, char *data)
{
    memcpy(data, &node->type, sizeof(node->type));
    data += sizeof(node->type);
    memcpy(data, node->name, strlen(node->name)+1);
    data += strlen(node->name)+1;

    if (node->type == DC_TYPE_REG) {
        memcpy(data, &node->size, sizeof(uint64_t));
	    data += sizeof(uint64_t);
        *data = node->reg.has_tth;
        data += 1;
        memcpy(data, node->reg.tth, sizeof(node->reg.tth));
        data += sizeof(node->reg.tth);
        memcpy(data, &node->reg.mtime, sizeof(time_t));
        data += sizeof(time_t);
    } else {
        size_t children;
        HMapIterator it;

        *data = node->dir.real_path != NULL ? 1 : 0;
        data += 1;
        if (node->dir.real_path != NULL) {
            memcpy(data, node->dir.real_path, strlen(node->dir.real_path)+1);
            data += strlen(node->dir.real_path)+1;
        }

        children = hmap_size(node->dir.children);
        memcpy(data, &children, sizeof(size_t));
        data += sizeof(size_t);
        hmap_iterator(node->dir.children, &it);
        while (it.has_next(&it)) {
            DCFileList *child_node = it.next(&it);
            data = copy_filelist_to_data(child_node, data);
        }
    }

    return data;
}

void
filelist_to_data(DCFileList *node, void **dataptr, size_t *sizeptr)
{
    size_t size;
    char *data;

    if (node == NULL) {
	*dataptr = NULL;
	*sizeptr = 0;
	return;
    }

    size = calculate_filelist_data_size(node);
    data = xmalloc(size);
    *dataptr = data;
    *sizeptr = size;

    copy_filelist_to_data(node, data);
}

/* This assumes that dataptr contains data that is complete and valid. */
void *
data_to_filelist(void *dataptr, DCFileList **outnode)
{
    DCFileList *node;
    DCFileType node_type;
    char *node_name;
    char *data = dataptr;

    if (dataptr == NULL) {
        *outnode = NULL;
        return NULL;
    }
    
    memcpy(&node_type, data, sizeof(node_type));
    data += sizeof(node_type);
    node_name = data;
    data += strlen(node_name) + 1;

    node = new_file_node(node_name, node_type, NULL);
    if (node_type == DC_TYPE_DIR) {
        size_t count;

        if (*data == 1) {
            node->dir.real_path = xstrdup(data+1);
            data += strlen(node->dir.real_path) + 1;
        }
        data += 1;

        memcpy(&count, data, sizeof(count));
        data += sizeof(count);

        for (; count > 0; count--) {
            DCFileList *child_node;

            data = data_to_filelist(data, &child_node);
            hmap_put(node->dir.children, child_node->name, child_node);
            child_node->parent = node;
            node->size += child_node->size;
        }
    } else {
        memcpy(&node->size, data, sizeof(node->size));
        data += sizeof(node->size);
        node->reg.has_tth = *data;
        data += 1;
        memcpy(node->reg.tth, data, sizeof(node->reg.tth));
        data += sizeof(node->reg.tth);
        memcpy(&node->reg.mtime, data, sizeof(time_t));
        data += sizeof(time_t);
    }

    *outnode = node;
    return data;
}

static DCFileList *
parse_decoded_dclst(char *decoded, uint32_t decoded_len)
{
    DCFileList *node;
    uint32_t c;
    PtrV *dirs;
    char *conv_name;

    dirs = ptrv_new();
    node = new_file_node("", DC_TYPE_DIR, NULL);
    ptrv_append(dirs, node);
    for (c = 0; c < decoded_len; c++) {
    	DCFileList *oldnode;
	char *name;
    	int depth;

	for (; c < decoded_len && decoded[c] == '\n'; c++);
    	depth = 1;
	for (; c < decoded_len && decoded[c] == '\t'; c++)
	    depth++;
	if (c >= decoded_len)
	    break; /* Premature end */
	if (decoded[c] == '\r') {
	    c++; /* skip LF */
	    continue; /* Skipping bad line */
	}

    	name = decoded + c;
    	for (; c < decoded_len && decoded[c] != '\r' && decoded[c] != '|'; c++);
	if (c >= decoded_len)
	    break;

	if (depth < dirs->cur)
	    ptrv_remove_range(dirs, depth, dirs->cur);

	oldnode = dirs->buf[dirs->cur-1];
	if (decoded[c] == '|') {
	    char *sizestr;
	    uint64_t size;

	    decoded[c] = '\0';
    	    sizestr = decoded+c+1;
    	    for (c++; c < decoded_len && decoded[c] != '\r'; c++);
	    if (c >= decoded_len)
	    	break; /* Premature end */
	    decoded[c] = '\0';
    	    decoded[++c]='\0'; /* skip LF */ /*?????????????????????????*/
	    if (!parse_uint64(sizestr, &size))
	    	continue; /* Skipping bad line */

	    /* convert name from hub (other clients) charset to local charset*/
	    conv_name = hub_to_main_string(name);
	    node = new_file_node(conv_name, DC_TYPE_REG, oldnode);
	    free(conv_name);
	    node->size = size;
	} else {
	    decoded[c] = '\0';
    	    decoded[++c]='\0'; /* skip LF */ /*????????????????????????????????????*/
	    conv_name = hub_to_main_string(name);
	    node = new_file_node( conv_name, DC_TYPE_DIR, oldnode);
	    free(conv_name);
	}

	if (node->type == DC_TYPE_REG) {
	    DCFileList *up_node;
	    for (up_node = oldnode; up_node != NULL; up_node = up_node->parent)
	    	up_node->size += node->size;
	}
	if (node->type == DC_TYPE_DIR)
	    ptrv_append(dirs, node);
    }

    node = dirs->buf[0];
    ptrv_free(dirs); /* ignore non-empty */
    return node;
}

static DCFileList *
filelist_open(const char *filename)
{
    struct stat st;
    uint8_t *contents;
    char *decoded;
    uint32_t decoded_len;
    int fd;
    DCFileList *root;
    ssize_t res;

    if (stat(filename, &st) < 0) {
    	screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
	return NULL;
    }
    contents = malloc(st.st_size);
    if (contents == NULL) {
    	screen_putf("%s: %s\n", quotearg(filename), errstr);
	return NULL;
    }
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
    	screen_putf(_("%s: Cannot open file for reading - %s\n"), quotearg(filename), errstr);
	return NULL;
    }
    res = full_read(fd, contents, st.st_size);
    if (res < st.st_size) {
    	if (res < 0)
    	    screen_putf(_("%s: Cannot read from file - %s\n"), quotearg(filename), errstr);
	else
	    screen_putf(_("%s: Premature end of file\n"), quotearg(filename));	/* XXX: really: file was truncated? */
	free(contents);
	if (close(fd) < 0)
    	    screen_putf(_("%s: Cannot close file - %s\n"), quotearg(filename), errstr);
	return NULL;
    }
    if (close(fd) < 0)
    	screen_putf(_("%s: Cannot close file - %s\n"), quotearg(filename), errstr);
    decoded = huffman_decode(contents, st.st_size, &decoded_len);
    free(contents);
    if (decoded == NULL) {
    	screen_putf(_("%s: Invalid data, cannot decode\n"), quotearg(filename));
	return NULL;
    }
    root = parse_decoded_dclst(decoded, decoded_len);
    free(decoded);
    return root;
}

static void
__attribute__((noreturn))
parse_main(int request_fd[2], int result_fd[2])
{
    MsgQ *request_mq;
    MsgQ *result_mq;
    struct sigaction sigact;
    
    close(request_fd[1]);
    close(result_fd[0]);
    request_mq = msgq_new(request_fd[0]);
    result_mq = msgq_new(result_fd[1]);

    /* Inability to register these signals is not a fatal error. */
    sigact.sa_flags = SA_RESTART;
    sigact.sa_handler = SIG_IGN;
#ifdef HAVE_STRUCT_SIGACTION_SA_RESTORER
    sigact.sa_restorer = NULL;
#endif
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGUSR1, &sigact, NULL);
    sigaction(SIGCHLD, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
    
    while (msgq_read_complete_msg(request_mq) > 0) {
        DCFileList *node;
        char *filename;
        char *main_hub_charset;
        void *data;
        size_t size;
        size_t filename_len = 0;

        msgq_get(request_mq, MSGQ_STR, &filename, MSGQ_STR, &main_hub_charset, MSGQ_END);

	    set_hub_charset(main_hub_charset);
	    free(main_hub_charset);

        filename_len = strlen(filename);
        if (strcmp(filename+filename_len-6, ".DcLst") == 0) {
            node = filelist_open(filename);
        } else if (strcmp(filename+filename_len-4, ".xml") == 0) {
            node = filelist_xml_open(filename);
        } else if (strcmp(filename+filename_len-8, ".xml.bz2") == 0) {
            node = filelist_bzxml_open(filename);
        } else {
            node = NULL;
        }
        filelist_to_data(node, &data, &size);
	    filelist_free(node);
        free(filename);
        msgq_put(result_mq, MSGQ_BLOB, data, size, MSGQ_END);
        free(data);
        if (msgq_write_all(result_mq) < 0)
            break;
    }

    /* msgq_read_complete_msg may have failed if it returned < 0.
     * But we can't print any errors from this process (it would
     * interfere with the readline-managed display, so just exit
     * gracefully.
     */

    msgq_free(request_mq);
    msgq_free(result_mq);
    close(request_fd[0]);
    close(result_fd[1]);
    exit(EXIT_SUCCESS);
}

void
parse_request_fd_writable(void)
{
    int res;
    
    res = msgq_write(parse_request_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, true, "parse request pipe");
        running = false;
        return;
    }
    if (!msgq_has_partial_msg(parse_request_mq))
        FD_CLR(parse_request_mq->fd, &write_fds);
}

void
parse_result_fd_readable(void)
{
    int res;

    res = msgq_read(parse_result_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, false, "parse result pipe");
        running = false;
        return;
    }
    while (msgq_has_complete_msg(parse_result_mq)) {
        DCFileListParse *parse;
        void *data;
        size_t size;

        msgq_get(parse_result_mq, MSGQ_BLOB, &data, &size, MSGQ_END);
        parse = ptrv_remove_first(pending_parses);
        if (!parse->cancelled) {
            DCFileList *node;

            data_to_filelist(data, &node); /* XXX: error reporting! */
            parse->callback(node, parse->data);
            /* It is the responsibility of the callback to free node
	     * when appropriate.
	     */
        }
        free(data);
        free(parse);
    }
}

/* Note that this function is currently not needed.
 * Cancelling of parse requests is not done - instead
 * the callback that is called when the parsing is done
 * handle the case when the browsing was cancelled (see
 * command.c:browse_list_parsed).
 */
void
cancel_parse_request(DCFileListParse *parse)
{
    int c;

    for (c = 0; c < pending_parses->cur; c++) {
        if (parse == pending_parses->buf[c]) {
            if (c == 0) {
                parse->cancelled = true;
            } else {
                ptrv_remove_range(pending_parses, c, c+1);
            }
            break;
        }
    }
}

DCFileListParse *
add_parse_request(DCFileListParseCallback callback, const char *filename, void *userdata)
{
    DCFileListParse *parse;

    msgq_put(parse_request_mq, MSGQ_STR, filename, MSGQ_STR, hub_charset ? hub_charset : "", MSGQ_END);
    FD_SET(parse_request_mq->fd, &write_fds);
    
    parse = xmalloc(sizeof(DCFileListParse));
    parse->callback = callback;
    parse->data = userdata;
    parse->cancelled = false;
    ptrv_append(pending_parses, parse);
    
    return parse;
}

bool
file_list_parse_init(void)
{
    int request_fd[2];
    int result_fd[2];
    
    if (pipe(request_fd) != 0 || pipe(result_fd) != 0) {
        warn(_("Cannot create pipe pair - %s\n"), errstr);
        return false;
    }
    if (!fd_set_nonblock_flag(request_fd[1], true)
            || !fd_set_nonblock_flag(result_fd[0], true)) {
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        return false;
    }

    parse_child = fork();
    if (parse_child < 0) {
        warn(_("Cannot create process - %s\n"), errstr);
        return false;
    }
    if (parse_child == 0)
        parse_main(request_fd, result_fd);
    
    pending_parses = ptrv_new();
    close(request_fd[0]);
    close(result_fd[1]);
    parse_request_mq = msgq_new(request_fd[1]);
    parse_result_mq = msgq_new(result_fd[0]);
    FD_SET(parse_result_mq->fd, &read_fds);
    return true;
}

void
file_list_parse_finish(void)
{
    if (pending_parses != NULL) {
        ptrv_foreach(pending_parses, free);
        ptrv_free(pending_parses);
    }
    if (parse_request_mq != NULL) {
        close(parse_request_mq->fd);
        msgq_free(parse_request_mq);
    }
    if (parse_result_mq != NULL) {
        close(parse_result_mq->fd);
        msgq_free(parse_result_mq);
    }
}
