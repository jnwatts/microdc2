/* fs.c - Local and remote file system management
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
#include <assert.h>		/* ? */
#include <ctype.h>
#include <sys/types.h>		/* ? */
#include <sys/stat.h>		/* ? */
#include <unistd.h>		/* POSIX */
#include <fcntl.h>		/* ? */
#include <stdlib.h>		/* C89 */
#include <dirent.h>		/* ? */
#include <sys/types.h>		/* ? */
#include <inttypes.h>		/* ? */
#include "full-read.h"		/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "full-write.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "fnmatch.h"		/* Gnulib */
#include "dirname.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/minmaxonce.h"
#include "common/error.h"
#include "common/intutil.h"
#include "common/strbuf.h"
#include "common/strleftcmp.h"
#include "common/substrcmp.h"
#include "common/comparison.h"
#include "iconvme.h"
#include "microdc.h"

//#define _TRACE
#if defined(_TRACE)
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

#define MAX_DIR_DEPTH 32
#define IS_OCT_DIGIT(d) ( (d) >= '0' && (d) <= '7' )

typedef struct _DCFileListIterator DCFileListIterator;

struct _DCFileListIterator {
    DCFileList *node;
    HMapIterator it;
    uint32_t c;
};

DCFileList *our_filelist              = NULL;
time_t      our_filelist_last_update  = 0;

static int
mkdirs_for_temp_file(char *filename)
{
    char *t;

    for (t = filename; *t == '/'; t++);
    while ((t = strchr(t, '/')) != NULL) {
	struct stat st;

        *t = '\0';
	if (stat(filename, &st) < 0) {
	    if (errno != ENOENT) {
	    	warn(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
		return -1;
	    } else {
        	if (mkdir(filename, 0777) < 0) {
    	    	    warn(_("%s: Cannot create directory - %s\n"), quotearg(filename), errstr); 
		    return -1;
		} else {
	    	    if (ptrv_find(delete_dirs, filename, (comparison_fn_t) strcmp) < 0)
	    		ptrv_append(delete_dirs, xstrdup(filename));
		}
	    }
	}
        *t = '/';
	for (; *t == '/'; t++);
    }

    return 0;
}

static int
fs_completion_entry_compare(const void *e1, const void *e2)
{  
    const DCCompletionEntry *ce1 = *(const DCCompletionEntry **) e1;
    const DCCompletionEntry *ce2 = *(const DCCompletionEntry **) e2;
    if (ce1->sorting.file_type == ce2->sorting.file_type) {
        char *s1 = xasprintf(ce1->display_fmt, ce1->display); /* XXX: this is really slow */
        char *s2 = xasprintf(ce2->display_fmt, ce2->display); /* XXX: this is really slow */
        int cmp = strcoll(s1, s2);
        free(s1);
        free(s2);
        return cmp;
    }
    return ce1->sorting.file_type - ce2->sorting.file_type;
}   

DCFileList *
new_file_node(const char *name, DCFileType type, DCFileList *parent)
{
    DCFileList *node;

    node = xmalloc(sizeof(DCFileList));
    node->name = xstrdup(name);
    node->type = type;
    node->parent = parent;
    if (parent != NULL)
        hmap_put(parent->dir.children, node->name, node);
    node->size = 0;
    switch (node->type) {
    case DC_TYPE_DIR:
        node->dir.real_path = NULL;
    	node->dir.children = hmap_new();
	break;
    case DC_TYPE_REG:
        node->reg.has_tth = false;
        memset(node->reg.tth, 0, sizeof(node->reg.tth));
        node->reg.mtime = 0;
        /* No more operation at the moment! */
        break;
    }

    return node;
}

void
rename_node(DCFileList *node, const char* new_name)
{
    char* node_name;

    if (node == NULL || new_name == NULL) {
        return;
    }

    node_name = node->name;
    node->name = xstrdup(new_name);

    if (node->parent != NULL) {
        hmap_remove(node->parent->dir.children, node_name);
        hmap_put(node->parent->dir.children, node->name, node);
    }
    free(node_name);
}

void
set_child_node(DCFileList *parent, DCFileList *child)
{
    if (parent == NULL || child == NULL || parent->type != DC_TYPE_DIR) {
        return;
    }
    child->parent = parent;
    hmap_put(parent->dir.children, child->name, child);
}

static DCFileList *
get_child_node(DCFileList *node, const char *path)
{
    if (IS_CURRENT_DIR(path))
        return node;
    if (IS_PARENT_DIR(path))
        return node->parent == NULL ? node : node->parent;
    return hmap_get(node->dir.children, path);
}

void
filelist_free(DCFileList *node)
{
    if (node != NULL) {
        switch (node->type) {
        case DC_TYPE_REG:
    	    break;
        case DC_TYPE_DIR:
            if (node->dir.real_path != NULL)
                free(node->dir.real_path);
            hmap_foreach_value(node->dir.children, filelist_free);
            hmap_free(node->dir.children);
    	    break;
        }
        free(node->name);
        free(node);
    }
}

static DCFileList *
filelist_lookup_tth(DCFileList *node, const char *tth)
{
    if (node->type == DC_TYPE_REG) {
	unsigned n;

	if (!node->reg.has_tth)
	    return NULL;

	for (n = 0; n < sizeof(node->reg.tth); n++) {
	    if ( tolower(node->reg.tth[n]) != tolower(tth[n]))
		return NULL;
	}
	return node;
    }else {
	HMapIterator it;
	DCFileList *res;

	hmap_iterator(node->dir.children, &it);
	while (it.has_next(&it)) {
	    DCFileList *subnode = it.next(&it);
	    res =  filelist_lookup_tth( subnode, tth);
	    if (res)
		return res;
	}
    }
    return NULL;
}

DCFileList *
filelist_lookup(DCFileList *node, const char *filename)
{
    const char *end;
    char *name;

    if (*filename != '/')
        return NULL;

    for (filename++; *filename == '/'; filename++);

    end = strchr(filename, '/');
    if (end == NULL) {
        if (!*filename)
            return node;
        end = filename + strlen(filename);
    }
    if (node->type != DC_TYPE_DIR)
        return NULL;

    name = xstrndup(filename, end-filename); /* XXX: strndupa? */
    node = get_child_node(node, name);
    free(name);

    if (node != NULL)
        return (*end == '\0' ? node : filelist_lookup(node, end));

    return NULL;
}

char *
filelist_get_path_with_trailing_slash(DCFileList *node)
{
    StrBuf *sb;

    sb = strbuf_new();
    while (node->parent != NULL) {
	strbuf_prepend(sb, node->name);
	strbuf_prepend_char(sb, '/');
    	node = node->parent;
    }
    if (node->type == DC_TYPE_DIR)
        strbuf_append_char(sb, '/');

    return strbuf_free_to_string(sb);
}

/* Return the path of the node relative to the share root directory.
 */
char *
filelist_get_path(DCFileList *node)
{
    StrBuf *sb;

    if (node->parent == NULL)
	return xstrdup("/"); /* root */

    sb = strbuf_new();
    while (node->parent != NULL) {
	strbuf_prepend(sb, node->name);
	strbuf_prepend_char(sb, '/');
    	node = node->parent;
    }

    return strbuf_free_to_string(sb);
}

/* Return the physical path of the node, by prepending the share directory path.
 */
static char *
filelist_get_real_path(DCFileList *node)
{
    char *p2;

    /*
    char *p1;
    p1 = filelist_get_path(node);
    p2 = catfiles(share_dir, p1+1); / * p1[0] == '/', we don't want that * /
    free(p1);
    */
    if (node->parent != NULL) {
        /* parent may be only directory */

        assert(node->parent->dir.real_path != NULL);
        p2 = catfiles(node->parent->dir.real_path, node->name);
    } else {
        p2 = xstrdup(node->name);
    }
    return p2;
}

/*static int
filelist_completion_compare(const void *i1, const void *i2)
{
    const DCCompletionEntry *ce1 = *(const DCCompletionEntry **) i1;
    const DCCompletionEntry *ce2 = *(const DCCompletionEntry **) i2;
    return strcmp(ce1->input, ce2->input);
}
*/

static int
file_node_compare(const void *i1, const void *i2)
{
    const DCFileList *f1 = *(const DCFileList **) i1;
    const DCFileList *f2 = *(const DCFileList **) i2;
    unsigned char *str1, *str2;
    unsigned char s1[2], s2[2];
    int res;

    if (f1->type != f2->type)
	return f1->type - f2->type;

    str1 = (unsigned char *)f1->name;
    str2 = (unsigned char *)f2->name;

    s1[1] = s2[1] =  '\0';

    for (; *str1 && *str2; str1++, str2++) {
	if ((*str1 != *str2)
		&& (tolower(*str1) != tolower(*str2))) {
	    s1[0] = tolower(*str1);
	    s2[0] = tolower(*str2);
	    res = strcoll(s1, s2);
	    if (res != 0)
		return res;
	}
    }
    if ( *str1 )
	return 1;
    if ( *str2)
	return -1;
    return 0;
}

static DCFileList **
get_sorted_file_list(DCFileList *node, uint32_t *out_count)
{
    HMapIterator it;
    DCFileList **items;
    uint32_t count;
    uint32_t c;

    assert(node->type == DC_TYPE_DIR);
    count = hmap_size(node->dir.children);
    items = xmalloc((count+1) * sizeof(DCFileList *));
    hmap_iterator(node->dir.children, &it);
    for (c = 0; c < count; c++)
        items[c] = it.next(&it);
    items[count] = NULL;
    qsort(items, count, sizeof(DCFileList *), file_node_compare);

    if (out_count != NULL)
        *out_count = count;

    return items;
}

void
filelist_list_recursively(DCFileList *node, char *basepath)
{
    if (node->type == DC_TYPE_DIR) {
        DCFileList **items;
        uint32_t c;

        items = get_sorted_file_list(node, NULL);
        for (c = 0; items[c] != NULL; c++) {
            char *path = catfiles(basepath, items[c]->name);
            filelist_list_recursively(items[c], path);
            free(path);
        }
        free(items);
    } else {
        screen_putf("%7" PRIu64 "M %s\n", (uint64_t) (node->size/(1024*1024)), quotearg(basepath)); /* " */
    }
}

void
filelist_list(DCFileList *node, int mode)
{
    uint32_t maxlen;
    uint64_t maxsize;

    if (node->type == DC_TYPE_DIR) {
        HMapIterator it;

        maxlen = 0;
        maxsize = 0;
        hmap_iterator(node->dir.children, &it);
        while (it.has_next(&it)) {
            DCFileList *subnode = it.next(&it);

            switch (subnode->type) {
            case DC_TYPE_REG:
                maxsize = max(maxsize, subnode->size);
                maxlen = max(maxlen, strlen(subnode->name));
                break;
            case DC_TYPE_DIR:
                maxsize = max(maxsize, subnode->size);
                maxlen = max(maxlen, strlen(subnode->name)+1);
                break;
            }
        }
    } else {
        maxsize = node->size;
        maxlen = strlen(node->name);
    }

    if ((mode & DC_LS_LONG_MODE) != 0) {
        char *format;

        format = xasprintf("%%%d" PRIu64 "M %%s%%s\n", ilog10(max(1, maxsize/(1024*1024))));
        if (node->type == DC_TYPE_DIR) {
            DCFileList **items;
            uint32_t c;

            items = get_sorted_file_list(node, NULL);
            for (c = 0; items[c] != NULL; c++) {
                switch (items[c]->type) {
                case DC_TYPE_REG:
                    screen_putf(format, (uint64_t) (items[c]->size/(1024*1024)), quotearg(items[c]->name), "");
                    break;
                case DC_TYPE_DIR:
                    screen_putf(format, (uint64_t) (items[c]->size/(1024*1024)), quotearg(items[c]->name), "/");
                    break;
                }
	        }
            free(items);
        } else {
	        screen_putf(format, (uint64_t) (node->size/(1024*1024)), quotearg(node->name), "");
        }
        free(format);
    } else {
    	if (node->type == DC_TYPE_DIR) {
            DCFileList **items;
            int cols;
            int rows;
            int row;
            int per_row;
            uint32_t count;

            items = get_sorted_file_list(node, &count);
            screen_get_size(NULL, &cols);
            per_row = MAX(1, (cols+2)/(maxlen+2));
            rows = (count/per_row) + (count%per_row != 0);

            for (row = 0; row < rows; row++) {
                uint32_t c;
                for (c = row; c < count; c += rows) {
                    DCFileList *item = items[c];
                    int extlen = 0;
                    int d;

                    switch (item->type) {
                    case DC_TYPE_REG:
                        extlen = 0;
                        screen_putf("%s", quotearg(item->name));
                        break;
                    case DC_TYPE_DIR:
                        extlen = 1;
                        screen_putf("%s/", quotearg(item->name));
                        break;
                    }
                    if (c+rows < count) {
                    for (d = maxlen-strlen(items[c]->name)-extlen+2; d > 0; d--)
	                    screen_putf(" ");
                    }
                }
                screen_putf("\n");
            }
            free(items);
        } else {
            screen_putf("%s\n", quotearg(node->name));
        }
    }
}

void
dir_to_filelist(DCFileList *parent, const char *path)
{
    struct dirent *ep;
    DIR *dp;
    /*
    char* tth_path = NULL;
    */

    dp = opendir(path);
    if (dp == NULL) {
    	screen_putf(_("%s: Cannot open directory - %s\n"), quotearg(path), errstr);
	    return;
    }
    
    parent->dir.real_path = xstrdup(path);

    /*
    tth_path = catfiles(path, tth_directory_name);
    */

    while ((ep = xreaddir(dp)) != NULL) {
    	struct stat st;
	    char *fullname;

    	if (IS_SPECIAL_DIR(ep->d_name))
	        continue;

	    /* If we ran into looped symlinked dirs, stat will stop (errno=ELOOP). */

    	fullname = catfiles(path, ep->d_name);
    	if (stat(fullname, &st) < 0) {
	        screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(fullname), errstr);
	        free(fullname);
	        continue;
	    }
	    if (S_ISDIR(st.st_mode)) {
	        DCFileList *node = new_file_node(ep->d_name, DC_TYPE_DIR, parent);
	        dir_to_filelist(node, fullname);
	        parent->size += node->size;
	    }
	    else if (S_ISREG(st.st_mode)) {
            /*
            char *tth_fname = NULL;
            int tth_fd = -1;
            */
	        DCFileList *node = new_file_node(ep->d_name, DC_TYPE_REG, parent);
	        node->size = st.st_size;
            node->reg.has_tth = 0;
            memset(node->reg.tth, 0, sizeof(node->reg.tth));
            node->reg.mtime = st.st_mtime;
	        parent->size += node->size;

            /*
            tth_fname = xasprintf("%s%s%s%s", tth_path, tth_path[0] == '\0' || tth_path[strlen(tth_path)-1] == '/' ? "" : "/", ep->d_name, ".tth");
            //flag_putf(DC_DF_DEBUG, _("Opening TTH file <%s> for <%s>\n"), tth_fname, ep->d_name);
            tth_fd = open(tth_fname, O_RDONLY);
            if (tth_fd >= 0) {
                uint64_t fsize;
                time_t mtime, ctime;
                char tth[39];
                if (read(tth_fd, &fsize, sizeof(fsize)) == sizeof(fsize) && st.st_size == fsize && 
                    read(tth_fd, &mtime, sizeof(mtime)) == sizeof(mtime) && st.st_mtime == mtime && 
                    read(tth_fd, &ctime, sizeof(ctime)) == sizeof(ctime) && st.st_ctime == ctime &&
                    read(tth_fd, tth, sizeof(tth)) == sizeof(tth)) {
                    node->reg.has_tth = 1;
                    memcpy(node->reg.tth, tth, sizeof(node->reg.tth));
                    //fprintf(stderr, "File <%s> has TTH\n", ep->d_name);
                }
                close(tth_fd);
            }
            free(tth_fname);
            */
	    }
	    else {
	        screen_putf(_("%s: Not a regular file or directory, ignoring\n"), quotearg(fullname));
	    }
	    free(fullname);
    }

    /*
    free(tth_path);
    */
    if (errno != 0)
    	screen_putf(_("%s: Cannot read directory - %s\n"), quotearg(path), errstr);
    if (closedir(dp) < 0)
    	screen_putf(_("%s: Cannot close directory - %s\n"), quotearg(path), errstr);
}

static void
filelist_to_string(DCFileList *node, StrBuf *sb, int level)
{
    char *fname;

    if (level != 0)
    	strbuf_append_char_n(sb, level-1, '\t');

    /* convert filenames from filesystem charset to hub charset */
    fname = fs_to_hub_string(node->name);

    if (node->type == DC_TYPE_REG) {
        strbuf_appendf(sb, "%s|%" PRIu64 "\r\n", fname, node->size); /* " joe sh bug */
        free(fname);
    } else {
        HMapIterator it;

        if (level != 0)
            strbuf_appendf(sb, "%s\r\n", fname);
        free(fname);
        hmap_iterator(node->dir.children, &it);
        while (it.has_next(&it)) {
            DCFileList *subnode = it.next(&it);
            filelist_to_string(subnode, sb, level+1);
        }
    }
}

bool write_filelist_file(DCFileList* root, const char* prefix)
{
    StrBuf *sb;
    char *indata;
    char *outdata;
    int fd, xml_fd, bzxml_fd;
    char *filename;
    char *xml_filename;
    char *bzxml_filename;
    uint32_t len;
    struct stat st;
    int i, failed_count;
    int max_failed_count = 1;
#if defined(HAVE_LIBXML2)
    max_failed_count = 3;
#endif

    if (root != NULL) {
        sb = strbuf_new();
	    filelist_to_string(root, sb, 0);
        len = strbuf_length(sb);
        indata = strbuf_free_to_string(sb);
        outdata = huffman_encode((uint8_t *) indata, len, &len);
        free(indata);

        filename = xasprintf("%s%s%sMyList.DcLst", listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", prefix == NULL ? "" : prefix);
        mkdirs_for_temp_file(filename); /* Ignore errors */

        if (stat(filename, &st) < 0) {
    	    if (errno != ENOENT)
	        warn(_("%s: Cannot get file status - %s\n"), filename, errstr);
        } else {
	        if (unlink(filename) < 0)
	            warn(_("%s: Cannot remove file - %s\n"), filename, errstr);
        }
        i = ptrv_find(delete_files, filename, (comparison_fn_t) strcmp);
        if (i >= 0)
    	    ptrv_remove_range(delete_files, i, i+1);

#if defined(HAVE_LIBXML2)
        xml_filename = xasprintf("%s%s%sfiles.xml", listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", prefix == NULL ? "" : prefix);
        mkdirs_for_temp_file(xml_filename); /* Ignore errors */
        if (stat(xml_filename, &st) < 0) {
    	    if (errno != ENOENT)
	        warn(_("%s: Cannot get file status - %s\n"), xml_filename, errstr);
        } else {
	        if (unlink(xml_filename) < 0)
	            warn(_("%s: Cannot remove file - %s\n"), xml_filename, errstr);
        }
        i = ptrv_find(delete_files, xml_filename, (comparison_fn_t) strcmp);
        if (i >= 0)
    	    ptrv_remove_range(delete_files, i, i+1);


        bzxml_filename = xasprintf("%s%s%sfiles.xml.bz2", listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", prefix == NULL ? "" : prefix);
        mkdirs_for_temp_file(bzxml_filename); /* Ignore errors */
        if (stat(bzxml_filename, &st) < 0) {
    	    if (errno != ENOENT)
	        warn(_("%s: Cannot get file status - %s\n"), bzxml_filename, errstr);
        } else {
	        if (unlink(bzxml_filename) < 0)
	            warn(_("%s: Cannot remove file - %s\n"), bzxml_filename, errstr);
        }
        i = ptrv_find(delete_files, bzxml_filename, (comparison_fn_t) strcmp);
        if (i >= 0)
    	    ptrv_remove_range(delete_files, i, i+1);
#endif

        failed_count = 0;
        fd = open(filename, O_CREAT|O_EXCL|O_WRONLY, 0666);
        if (fd < 0) {
    	    screen_putf(_("%s: Cannot open file for writing - %s\n"), quotearg(filename), errstr);
            failed_count++;
        }
#if defined(HAVE_LIBXML2)
        xml_fd = open(xml_filename, O_CREAT|O_EXCL|O_WRONLY, 0666);
        if (xml_fd < 0) {
    	    screen_putf(_("%s: Cannot open file for writing - %s\n"), quotearg(xml_filename), errstr);
            failed_count++;
        }
        bzxml_fd = open(bzxml_filename, O_CREAT|O_EXCL|O_WRONLY, 0666);
        if (bzxml_fd < 0) {
    	    screen_putf(_("%s: Cannot open file for writing - %s\n"), quotearg(bzxml_filename), errstr);
            failed_count++;
        }
#endif
        if (failed_count == max_failed_count) {
	        filelist_free(root);
	        free(outdata);
	        free(filename);
#if defined(HAVE_LIBXML2)
	        free(xml_filename);
	        free(bzxml_filename);
#endif
	        return false;
        }
        /*
        if (ptrv_find(delete_files, filename, (comparison_fn_t) strcmp) < 0)
    	    ptrv_append(delete_files, xstrdup(filename));
#if defined(HAVE_LIBXML2)
        if (ptrv_find(delete_files, xml_filename, (comparison_fn_t) strcmp) < 0)
    	    ptrv_append(delete_files, xstrdup(xml_filename));
        if (ptrv_find(delete_files, bzxml_filename, (comparison_fn_t) strcmp) < 0)
    	    ptrv_append(delete_files, xstrdup(bzxml_filename));
#endif
        */

        failed_count = 0;
        if (fd < 0 || full_write(fd, outdata, len) < len) {
            if (fd >= 0)
    	        screen_putf(_("%s: Cannot write to file - %s\n"), quotearg(filename), errstr);
            failed_count++;
        }
        free(outdata);

#if defined(HAVE_LIBXML2)
        if (xml_fd < 0 || write_xml_filelist(xml_fd, root) < 0) {
            if (xml_fd >= 0) {
    	        screen_putf(_("%s: Cannot write to file - %s\n"), quotearg(xml_filename), errstr);
            }
            failed_count++;
        }
        if (bzxml_fd < 0 || write_bzxml_filelist(bzxml_fd, root) < 0) {
            if (bzxml_fd >= 0) {
    	        screen_putf(_("%s: Cannot write to file - %s\n"), quotearg(bzxml_filename), errstr);
            }
            failed_count++;
        }
#endif
        if (failed_count == max_failed_count) {
	        filelist_free(root); 
	        free(filename);
#if defined(HAVE_LIBXML2)
	        free(xml_filename);
	        free(bzxml_filename);
#endif
	        return false;
        }

        if (close(fd) < 0)
    	    screen_putf(_("%s: Cannot close file - %s\n"), quotearg(filename), errstr);
        free(filename);

#if defined(HAVE_LIBXML2)
        if (close(xml_fd) < 0)
    	    screen_putf(_("%s: Cannot close file - %s\n"), quotearg(xml_filename), errstr);
        free(xml_filename);

        // we don't need to close this file - it was closed by write_bzxml_filelist() function.
        free(bzxml_filename);
#endif

        return true;
    }
    return false;
}

bool
filelist_create(const char *basedir)
{
    char* conv_basedir;
    DCFileList *root = new_file_node("", DC_TYPE_DIR, NULL);

    if (basedir != NULL) {
    	screen_putf(_("Scanning directory %s\n"), quotearg(basedir));
        //screen_sync();
    	dir_to_filelist(root, basedir);
    }
	/*  basedir for dir_to_filelist must be in filesystem encoding */
	conv_basedir = main_to_fs_string(basedir);
	dir_to_filelist(root, conv_basedir);
    free(conv_basedir);


    if (our_filelist != NULL)
    	filelist_free(our_filelist);
    our_filelist = root;
    my_share_size = our_filelist->size;

    return write_filelist_file(root, NULL);
}

/* Find the physical path of a file that the remote end
 * wants to download from us.
 */
char *
resolve_upload_file(DCUserInfo *ui, DCAdcgetType ul_type, const char *name, DCTransferFlag* flag, uint64_t* size)
{
    DCFileList *node;

    if (ul_type == DC_ADCGET_FILE) {
        /* Note: 'name' will have been translated to local slashes
         * by a call to translate_remote_to_local prior to calling
         * this function.
         */

        /* Skip leading slashes, all but one */
        if (name[0] != '/')
            return NULL;
        for (; name[1] == '/'; name++);

        if (strcmp(name, "/MyList.DcLst") == 0
#if defined(HAVE_LIBXML2)
            || strcmp(name, "/files.xml") == 0 || strcmp(name, "/files.xml.bz2") == 0
#endif
            ) {
            char* filename = catfiles(listing_dir, name);
            if (flag != NULL) {
                *flag = DC_TF_LIST;
            }
            if (size != NULL) {
                struct stat st;
                if (stat(filename, &st) == 0) {
                    *size = st.st_size;
                } else {
                    *size = 0;
                }
            }
            return filename;
        }
    }

    if (our_filelist == NULL)
        return NULL;

    if (ul_type == DC_ADCGET_FILE) 
        node = filelist_lookup(our_filelist, name);
    else
        node = filelist_lookup_tth(our_filelist, name);

    if (node == NULL)
        return NULL;
    if (ul_type == DC_ADCGET_TTHL) {
        return NULL;
/*
	StrBuf *sb;

	sb = strbuf_new();

	strbuf_append_char(sb, '/');
	strbuf_append(sb, tth_directory_name);
	if ( tth_directory_name[strlen(tth_directory_name) - 1] != '/' )
	    strbuf_append_char(sb, '/');
	strbuf_append(sb, node->name);
	strbuf_append(sb, ".tth");

	node = node->parent;
	while (node != NULL) {
	    strbuf_prepend(sb, node->name);
	    strbuf_prepend_char(sb, '/');
	    node = node->parent;
	}

	strbuf_prepend(sb, share_dir);
	
	return strbuf_free_to_string(sb);
*/

    } else {
        if (flag != NULL) {
            *flag = DC_TF_NORMAL;
        }
        if (size != NULL) {
            *size = node->size;
        }
    }
    return filelist_get_real_path(node);
}

char *
resolve_download_file(DCUserInfo *ui, DCQueuedFile *queued)
{
    char *filename;
    char *tmp, *tmp2;

    if (queued->flag == DC_TF_LIST) {
        tmp = xasprintf("%s", ui->nick);
        tmp2 = catfiles(listing_dir, tmp);
        free(tmp);

        filename = main_to_fs_string(tmp2);
        free(tmp2);

        if (filename) {
            mkdirs_for_temp_file(filename); /* Ignore errors */
        }
    } else {
        tmp = catfiles(download_dir, queued->filename + strlen(queued->base_path));
        tmp2 = xasprintf("%s.part", tmp);
        free(tmp);

        filename = main_to_fs_string(tmp2);
	    free(tmp2);

        if (filename)
            mkdirs_for_file(filename); /* Ignore errors */
    }

    return filename;
}

char *
translate_remote_to_local(const char *remotename)
{
    StrBuf *sb;

    sb = strbuf_new();
    strbuf_append_char(sb, '/');
    for (; *remotename != '\0'; remotename++) {
    	if (*remotename == '\\')
	    strbuf_append_char(sb, '/');
	else
	    strbuf_append_char(sb, *remotename);
    }

    return strbuf_free_to_string(sb);
}

char *
translate_local_to_remote(const char *localname)
{
    StrBuf *sb;

    sb = strbuf_new();
    localname++;
    for (; *localname != '\0'; localname++) {
    	if (*localname == '/')
            strbuf_append_char(sb, '\\');
	else
	    strbuf_append_char(sb, *localname);
    }

    return strbuf_free_to_string(sb);
}

char *
apply_cwd(const char *path)
{
    if (*path == '/') {
    	return xstrdup(path);
    } else {
    	return catfiles(browse_path, path);
    }
}

/* Concatenate two file name components, taking in consideration trailing
 * slash in the first component P1.
 * An empty file name component ("") can be specified as well. It is a
 * special case which is treated as "no directory component".
 * "." is returned if both P1 and P2 are empty file name components.
 * Note that consecutive sequences of slashes ("/") in P1 and P2 are
 * untouched. Only the trailing slash of P1 is considered when concatenating
 * P1 and P2.
 * The returned value should be freed when no longer needed.
 * P1 or P2 may not be NULL.
 */   
char *
concat_filenames(const char *p1, const char *p2)
{
    size_t l1;
    size_t l2;
    char *out;

    if (*p1 == '\0' && *p2 == '\0')
        return xstrdup(".");
    if (*p1 == '\0')
        return xstrdup(p2);
    if (*p2 == '\0')
        return xstrdup(p1);

    l1 = strlen(p1);
    l2 = strlen(p2);
    if (p1[l1-1] == '/')
        l1--;

    out = xmalloc(l1+1+l2+1);
    memcpy(out, p1, l1);
    out[l1] = '/';
    memcpy(out+l1+1, p2, l2+1);

    return out;
}


/* Return true if the string BUF (which may contain quotes and
 * escapes) starts with a slash.
 * This is equivalent to dequoting BUF, and testing if the first
 * character of the result is '/', only that with this function
 * no memory is actually allocated.
 * This function could also be implemented as
 *   return skip_slashes(&buf, &quoted);
 */
bool
has_leading_slash(const char *buf)
{
    for (; buf[0] == '"'; buf++); /* Begins and ends empty strings */
    return buf[0] == '/' || (buf[0] == '\\' && buf[1] == '/');
}

/* Find the first non-slash character, not including
 * quotes. Return true if a slash was found.
 * Update *BUFPTR to point to the first non-slash character.
 * Also update *QUOTEDPTR to reflect quoted state;
 */
/* rename: skip_leading_slashes */
static bool
skip_slashes(char **bufptr, bool *quotedptr)
{
    char *buf = *bufptr;
    bool quoted = *quotedptr;
    bool slash = false;

    for (; ; buf++) {
        if (buf[0] == '"') {
            quoted = !quoted;
        } else if (buf[0] == '/') {
            slash = true;
        } else if (buf[0] == '\\' && buf[1] == '/') {
            slash = true;
            buf++;
        } else {
            break;
        }
    }    

    *bufptr = buf;
    *quotedptr = quoted;
    return slash;
}  


/* Extract the first file name component of *BUFPTR and place the
 * newly allocated string into *OUTPTR. Update *BUFPTR to point  
 * to the first character not in this component (excluding quotes).
 * Also update *QUOTEDPTR. A file component is not supposed to contain
 * slashes, so all leading slashes of *BUFPTR are ignored. Note that  
 * if *BUFPTR is the empty string (possibly after leading slashes have
 * been ignored), then *OUTPTR will be an empty string. You should
 * free *OUTPTR when it is no longer needed.
 *
 * This function combines many operations:
 *  - stop matching when a slash is encountered
 *  - determine if there are unquoted and unescaped wildcards
 *  - escape quoted wildcards for fnmatch, if there were wildcards
 *  - remove quotes and escapes
 */
static bool
dircomp_to_fnmatch_str(char **bufptr, bool *quotedptr, char **outptr)
{
    StrBuf *out;
    char *buf;  
    bool quoted;
    bool wildcards = false;

    skip_slashes(bufptr, quotedptr);
    out = strbuf_new(); 

    quoted = *quotedptr;
    for (buf = *bufptr; *buf != '\0' && *buf != '/'; buf++) {
        if (*buf == '"')
            quoted = !quoted;
        else if (*buf == '\\' && buf[1] != '\0')
            buf++;
        else if ((*buf == '*' || *buf == '?') && !quoted)
            wildcards = true;
    }

    quoted = *quotedptr;
    for (buf = *bufptr; *buf != '\0' && *buf != '/'; buf++) {
        if (*buf == '"') {
            quoted = !quoted;
        } else if (*buf == '\\') {
            buf++;
            if (*buf == '\0')
                break;

            /*if (*buf == 'a') {
                strbuf_append_char(out, '\a');
            } else if (*buf == 'b') {
                strbuf_append_char(out, '\b');
            } else if (*buf == 'f') {
                strbuf_append_char(out, '\f');
            } else if (*buf == 'n') {
                strbuf_append_char(out, '\n');
            } else if (*buf == 'r') {
                strbuf_append_char(out, '\r');
            } else if (*buf == 't') {
                strbuf_append_char(out, '\t');
            } else if (*buf == 'v') {
                strbuf_append_char(out, '\v');
            } else*/ if (IS_OCT_DIGIT(*buf)) {
                int chr = *buf - '0';
                buf++;
                if (*buf != '\0' && IS_OCT_DIGIT(*buf)) {
                    chr = (chr * 8) + (*buf - '0');    
                    buf++;
                    if (*buf != '\0' && IS_OCT_DIGIT(*buf)) {
                        chr = (chr * 8) + (*buf - '0');    
                        buf++;
                    }
                }
                buf--;
		strbuf_append_char(out, chr);
            } else if (*buf == '*' || *buf == '?') {
                if (quoted || !wildcards)
                    strbuf_append_char(out, '\\'); /* escape wildcard for fnmatch */
                strbuf_append_char(out, *buf);
            } else {
                strbuf_append_char(out, *buf);
            }
        } else {
            if (wildcards && quoted && (*buf == '*' || *buf == '?'))
                strbuf_append_char(out, '\\'); /* escape wildcard for fnmatch */
            strbuf_append_char(out, *buf);
        }
    }

    *bufptr = buf;
    *quotedptr = quoted;
    *outptr = strbuf_free_to_string(out);
    
    return wildcards;
}

static void
add_remote_wildcard_result(char *name, DCFileList *node, DCFSCompletionFlags flags, DCCompletionInfo *ci)
{
    DCCompletionEntry *entry = NULL;
    char *input;

    input = filename_quote_string(name, ci->word_full[0] == '"', true);
    if (node->type == DC_TYPE_DIR) {
        if (flags & DC_CPL_DIR)
            entry = new_completion_entry_full(input, name, "%s/", "%s/", false, true);
    } else {
        if (flags & DC_CPL_REG)
            entry = new_completion_entry_full(input, name, "%s", "%s", true, true);
    }
    if (entry != NULL) {
        entry->sorting.file_type = node->type;
        ptrv_append(ci->results, entry);
    } else {
        free(input);
        free(name);
    }
}

/* Note that NAME will never be "/" for this function.
 * It is not possible for a complete operation to have the bare result "/".
 */
static void
add_local_wildcard_result(char *name, DCFSCompletionFlags flags, DCCompletionInfo *ci)
{
    struct stat st;
    char *name_fs;

    name_fs = main_to_fs_string(name);

    if (lstat(name_fs, &st) == 0) {
        DCCompletionEntry *entry = NULL;
        char *input;
        bool dquoted = (ci->word_full[0] == '"');

        input = filename_quote_string(name, dquoted, true);
        if (S_ISDIR(st.st_mode)) {
            if (flags & DC_CPL_DIR) {
                entry = new_completion_entry_full(input, name, "%s/", "%s/", false, true);
                entry->sorting.file_type = DC_TYPE_DIR;
            }
        } else if (S_ISLNK(st.st_mode)) {
            if (stat(name_fs, &st) == 0 && S_ISDIR(st.st_mode)) {
                if (flags & DC_CPL_DIR) {
                    entry = new_completion_entry_full(input, name, "%s", "%s@", false, true);
                    if (dquoted)
                        input[strlen(input)-1] = '\0';
                    if (strcmp(ci->word_full, input) == 0)
                        entry->input_single_fmt = "%s/";
                    if (dquoted)
                        input[strlen(input)] = '"';
                    entry->sorting.file_type = DC_TYPE_DIR;
                }
            } else {
                if (flags & DC_CPL_REG) {
                    entry = new_completion_entry_full(input, name, "%s", "%s@", true, true);
                    entry->sorting.file_type = DC_TYPE_REG;
                }
            }
        } else if (S_ISREG(st.st_mode)) {
            if ((flags & DC_CPL_REG) || (flags & DC_CPL_EXE)) {
                if ((access(name_fs, X_OK) == 0) == ((flags & DC_CPL_EXE) != 0)) {
                    entry = new_completion_entry_full(input, name, "%s", "%s", true, true);
                    entry->sorting.file_type = DC_TYPE_REG;
                } else if ((flags & DC_CPL_EXE) == 0) {
                    entry = new_completion_entry_full(input, name, "%s", "%s*", true, true);
                    entry->sorting.file_type = DC_TYPE_REG;
                }
            }
        }
        free(name_fs);

        if (entry != NULL) {
            ptrv_append(ci->results, entry);
        } else {
            free(input);
            free(name);
        }
    }
}

static void
filelist_iterator(DCFileList *node, DCFileListIterator *it)
{
    hmap_iterator(node->dir.children, &it->it);
    it->node = node;
    it->c = 0;
}

static bool
filelist_get_next(DCFileListIterator *it, DCFileList **node, char **name)
{
    it->c++;
    if (it->c == 1) {
        *node = it->node; 
        *name = ".";
        return true;
    }
    if (it->c == 2) {
        *node = it->node->parent == NULL ? it->node : it->node->parent;
        *name = "..";
        return true;
    }
    if (it->it.has_next(&it->it)) {
        *node = it->it.next(&it->it);
        *name = (*node)->name;
        return true;
    }
    return false;
}

void
remote_wildcard_expand(char *matchpath, bool *quotedptr, const char *basedir, DCFileList *basenode, PtrV *results)
{
    char *matchcomp;
    char *fullpath;
    char *nodename;
    DCFileList *node;
    DCFileListIterator it;

    if (dircomp_to_fnmatch_str(&matchpath, quotedptr, &matchcomp)) {
        filelist_iterator(basenode, &it);
        while (filelist_get_next(&it, &node, &nodename)) {
            if (fnmatch(matchcomp, nodename, FNM_PERIOD) == 0) {
                if (*matchpath != '\0') {
                    if (node->type == DC_TYPE_DIR) {
                        fullpath = concat_filenames(basedir, nodename);
                        remote_wildcard_expand(matchpath, quotedptr, fullpath, node, results);
                        free(fullpath);
                    }
                } else {
                    ptrv_append(results, concat_filenames(basedir, nodename));
                    /*ptrv_append(results, node);*/
                }
            }
        }
    } else {
        if (*matchcomp == '\0') {
            ptrv_append(results, concat_filenames(basedir, matchcomp));
        } else {
            node = get_child_node(basenode, matchcomp);
            if (node != NULL) {
                if (*matchpath != '\0') {
                    fullpath = concat_filenames(basedir, matchcomp);
                    remote_wildcard_expand(matchpath, quotedptr, fullpath, node, results);
                    free(fullpath);
                } else {
                    ptrv_append(results, concat_filenames(basedir, matchcomp));
                    /*ptrv_append(results, node);*/
                }
            }
        }
    }
    free(matchcomp);
}

static void
remote_wildcard_complete(char *matchpath, bool *quotedptr, char *basedir, DCFileList *basenode, DCFSCompletionFlags flags, DCCompletionInfo *ci, bool found_wc)
{
    char *matchcomp;
    char *fullpath;
    char *nodename;
    DCFileList *node;
    DCFileListIterator it;

    if (dircomp_to_fnmatch_str(&matchpath, quotedptr, &matchcomp)) {
        filelist_iterator(basenode, &it);
        while (filelist_get_next(&it, &node, &nodename)) {
            if (fnmatch(matchcomp, nodename, FNM_PERIOD) == 0) {
                if (*matchpath != '\0') {
                    if (node->type == DC_TYPE_DIR) {
                        fullpath = concat_filenames(basedir, nodename);
                        remote_wildcard_complete(matchpath, quotedptr, fullpath, node, flags, ci, true);
                        free(fullpath);
                    }
                } else {
                    add_remote_wildcard_result(concat_filenames(basedir, node->name), node, flags, ci);
                }
            }
        }
    } else {
        fullpath = concat_filenames(basedir, matchcomp);
        if (*matchpath != '\0') { /* more components follow after this one? */
            node = get_child_node(basenode, matchcomp);
            if (node != NULL)
                remote_wildcard_complete(matchpath, quotedptr, fullpath, node, flags, ci, found_wc);
        } else {
            /* If the string we are completing had wild cards, attempt to expand the string
             * first rather than generating multiple possible completion results.
             * This behavior is similar to that of GNU readline.
             */
	    if (found_wc) {
	        if (*matchcomp == '\0') { /* completion word ends in slash */
                    add_remote_wildcard_result(concat_filenames(basedir, matchcomp), basenode, flags, ci);
	        } else {
	            node = get_child_node(basenode, matchcomp);
                    if (node != NULL)
                        add_remote_wildcard_result(concat_filenames(basedir, matchcomp), node, flags, ci);
                }
            } else {
                filelist_iterator(basenode, &it);
                while (filelist_get_next(&it, &node, &nodename)) {
                    if ((nodename[0] == '.') != (matchcomp[0] == '.'))
                        continue;
                    if (strleftcmp(matchcomp, nodename) == 0)
                        add_remote_wildcard_result(concat_filenames(basedir, nodename), node, flags, ci);
                }
            }
        }
        free(fullpath);
    }
    free(matchcomp);
}

static void
local_wildcard_complete(char *matchpath, bool *quotedptr, char *basedir, DCFSCompletionFlags flags, DCCompletionInfo *ci, bool found_wc)
{
    char *matchcomp;
    struct stat sb;
    char *fullpath_fs, *fullpath;
    DIR *dh;
    struct dirent *de;
    char *basedir_fs;
    char *fname_loc;

    basedir_fs = main_to_fs_string(basedir);

    if (dircomp_to_fnmatch_str(&matchpath, quotedptr, &matchcomp)) {
        dh = opendir(*basedir_fs == '\0' ? "." : basedir_fs);
        if (dh != NULL) {
            while ((de = readdir(dh)) != NULL) {
                /* convert filename from filesystem charset for fnmatch*/
                fname_loc = fs_to_main_string(de->d_name);

                if (fnmatch(matchcomp, fname_loc, FNM_PERIOD) == 0) {
                    if (*matchpath != '\0') {
                        fullpath_fs = concat_filenames(basedir_fs, de->d_name);
                        fullpath = concat_filenames(basedir, fname_loc);
                        if (stat(fullpath_fs, &sb) == 0 && S_ISDIR(sb.st_mode))
                            local_wildcard_complete(matchpath, quotedptr, fullpath, flags, ci, true);
                        free(fullpath_fs);
                        free(fullpath);
                    } else {
                        add_local_wildcard_result(concat_filenames(basedir, fname_loc), flags, ci);
                    }
                }

                free(fname_loc);
            }
            closedir(dh);
        }
    } else {
        fullpath = concat_filenames(basedir, matchcomp);
        fullpath_fs = main_to_fs_string(fullpath);
        if (*matchpath != '\0') {
            if (lstat(fullpath_fs, &sb) == 0)
                local_wildcard_complete(matchpath, quotedptr, fullpath, flags, ci, found_wc);
        } else {
            /* If the string we are completing had wild cards, attempt to expand the string
             * first rather than generating multiple possible completion results.
             * This behavior is similar to that of GNU readline.
             */
            if (found_wc) {
	            if (*matchcomp == '\0') { /* completion word ends in slash */
	                add_local_wildcard_result(concat_filenames(basedir, matchcomp), flags, ci);
                } else if (/*strcmp(basedir, "/") != 0 &&*/ lstat(fullpath_fs ? fullpath_fs : fullpath, &sb) == 0) {
                    add_local_wildcard_result(concat_filenames(basedir, matchcomp), flags, ci);
                }
            } else {
                dh = opendir(*basedir_fs == '\0' ? "." : basedir_fs);
                if (dh != NULL) {
                    while ((de = readdir(dh)) != NULL) {
                        if ((de->d_name[0] == '.') != (matchcomp[0] == '.'))
                            continue;

                        fname_loc = fs_to_main_string(de->d_name);

                        if (strleftcmp(matchcomp, fname_loc ? fname_loc : de->d_name) == 0)
                            add_local_wildcard_result(concat_filenames(basedir, fname_loc), flags, ci);
                        free(fname_loc);
                    }
                    closedir(dh);
                }
            }
        }
        free(fullpath);
        free(fullpath_fs);
    }
    free(basedir_fs);
    free(matchcomp);
}

static void
fixup_wildcard_completion_results(DCCompletionInfo *ci)
{
    if (ci->results->cur > 1) {
        DCCompletionEntry *ce;
        char *s1, *s2;
        int c, d;
        int min;

        /* This differs from GNU readline. Readline always displays
         * the base name of every result, even if the directory part
         * differs. Find longest common leading string, then find last
         * '/' and strip everything before and including the slash.
         */
        ce = ci->results->buf[0];
        s1 = xasprintf(ce->display_fmt, ce->display);
        min = strlen(s1);

        for (c = 1; c < ci->results->cur; c++) {
            char c1, c2;

            ce = ci->results->buf[c];
            s2 = xasprintf(ce->display_fmt, ce->display);
            for (d = 0; (c1 = s1[d]) != '\0' && (c2 = s2[d]) != '\0'; d++) {
                if (c1 != c2)
                    break;
            }
            min = MIN(min, d);
            free(s2);
        }

        if (min > 0) {
            s1[min] = '\0';
            s2 = strrchr(s1, '/');
            if (s2 != NULL) {
                min = s2-s1+1;
                for (c = 0; c < ci->results->cur; c++) {
                    ce = ci->results->buf[c];
                    s2 = strdup(ce->display + min);
                    if (ce->display != ce->input)
                        free(ce->display);
                    ce->display = s2;
                }
            }
        }
        free(s1);
 
        ptrv_sort(ci->results, fs_completion_entry_compare);
    }
}

void
local_fs_completion_generator(DCCompletionInfo *ci, DCFSCompletionFlags flags)
{
    bool quoted = false;
    local_wildcard_complete(ci->word_full, &quoted, has_leading_slash(ci->word_full) ? "/" : "", flags, ci, false);
    fixup_wildcard_completion_results(ci);
}

void
local_path_completion_generator(DCCompletionInfo *ci)
{
    local_fs_completion_generator(ci, DC_CPL_REG|DC_CPL_DIR);
}

void
local_dir_completion_generator(DCCompletionInfo *ci)
{
    local_fs_completion_generator(ci, DC_CPL_DIR);
}

void
remote_fs_completion_generator(DCCompletionInfo *ci, DCFSCompletionFlags flags)
{
    bool quoted = false;
    char *basedir;
    DCFileList *basenode;

    if (browse_list == NULL)
        return;
    if (has_leading_slash(ci->word_full)) {
        basenode = filelist_lookup(browse_list, "/");
        basedir = "/";
    } else {
        basenode = filelist_lookup(browse_list, browse_path);
        basedir = "";
    }
    if (basenode != NULL) {
        remote_wildcard_complete(ci->word_full, &quoted, basedir, basenode, flags, ci, false);
        fixup_wildcard_completion_results(ci);
    }
}

void
remote_path_completion_generator(DCCompletionInfo *ci)
{
    return remote_fs_completion_generator(ci, DC_CPL_REG|DC_CPL_DIR);
}

void
remote_dir_completion_generator(DCCompletionInfo *ci)
{
    return remote_fs_completion_generator(ci, DC_CPL_DIR);
}
