/* util.c - Utility functions that didn't make it elsewhere
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
#include <stdint.h>		/* Gnulib/POSIX/C99 */
#include <sys/types.h>		/* ? */
#include <sys/stat.h>		/* ? */
#include <unistd.h>		/* POSIX: confstr, ... */
#include <fcntl.h>		/* ? */
#include <string.h>		/* C89 */
#include <arpa/inet.h>		/* ? */
#include "xalloc.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "quotearg.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/comparison.h"
#include "common/intutil.h"
#include "microdc.h"

#define SECONDS_PER_DAY (SECONDS_PER_HOUR*HOURS_PER_DAY)
#define SECONDS_PER_HOUR (SECONDS_PER_MINUTE*MINUTES_PER_HOUR)
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define HOURS_PER_DAY 24

/* safe_rename: A rename that won't overwrite the destination.
 * Or at least try not to. :)
 */
int
safe_rename(const char *oldpath, const char *newpath)
{
    int fd;

    fd = open(newpath, O_CREAT|O_EXCL, 0600);
    if (fd < 0)
        return -1;
    if (close(fd) != 0)
        return -1;
    return rename(oldpath, newpath);
}

bool
parse_ip_and_port(char *source, struct sockaddr_in *addr, uint16_t defport)
{
    char *port;

    port = strrchr(source, ':');
    if (port == NULL) {
        if (defport == 0)
            return false;
        addr->sin_port = htons(defport);
    } else {
        *port = '\0';
        port++;
        if (!parse_uint16(port, &addr->sin_port))
            return false;
        addr->sin_port = htons(addr->sin_port);
    }

    if (!inet_aton(source, &addr->sin_addr))
        return false;

    addr->sin_family = AF_INET;
    return true;
}

/* Move to lib !? */
int
ilog10(uint64_t c)
{
    int r;
    for (r = 0; c > 0; r++)
        c /= 10;
    return r;
}

/* XXX: try to get rid of screen_putf from this function. */
int
mkdirs_for_file(char *filename)
{
    char *t;

    for (t = filename; *t == '/'; t++);
    while ((t = strchr(t, '/')) != NULL) {
	    struct stat st;

            *t = '\0';
	    if (stat(filename, &st) < 0) {
	        if (errno != ENOENT) {
	    	    screen_putf(_("%s: Cannot get file status - %s\n"), quotearg(filename), errstr);
                return -1;
            } else {
                if (mkdir(filename, 0777) < 0) {
                    screen_putf(_("%s: Cannot create directory - %s\n"), quotearg(filename), errstr); 
                    return -1;
                }
                /*
                else if (deletedirs) {
	    	        if (ptrv_find(delete_dirs, filename, (comparison_fn_t) strcmp) < 0)
	    		    ptrv_append(delete_dirs, xstrdup(filename));
                }
                */
            }
        }

        *t = '/';
	    for (; *t == '/'; t++);
    }

    return 0;
}

/* Concatenate two file names, adding slash character in between
 * if necessary. An empty string for file name will be treated as
 * the current directory, only invisible instead of `./'. If both
 * P1 and P2 are empty strings, then the empty string will be
 * returned. P1 and P2 must not be NULL. The returned value will
 * be allocated on the heap and must be freed with free(). NULL
 * will never be returned.
 *
 *	P1	P2	with slash	as is
 * 	""	""	""		""
 *	"foo"	""	"foo/"		"foo"
 * 	"foo/"	""	"foo/"		"foo/"
 *	""	"foo"	"foo/"		"foo"
 *	""	"foo/"	"foo/		"foo/"
 *	"foo"	"foo"	"foo/foo/"	"foo/foo"
 *	"foo"	"foo/"	"foo/foo/"	"foo/foo"
 *	"foo/"	"foo"	"foo/foo/"	"foo/foo/"
 *	"foo/"	"foo/"	"foo/foo"	"foo/foo/"
 */
char *
catfiles_with_trailing_slash(const char *p1, const char *p2)
{
    return xasprintf("%s%s%s%s",
	    p1, p1[0] == '\0' || p1[strlen(p1)-1] == '/' ? "" : "/",
	    p2, p2[0] == '\0' || p2[strlen(p2)-1] == '/' ? "" : "/");
}

char *
catfiles(const char *p1, const char *p2)
{
    return xasprintf("%s%s%s",
	    p1, p1[0] == '\0' || p1[strlen(p1)-1] == '/' ? "" : "/",
	    p2);
}

/* Get environment variable with default value if it is not set.
 * Can be moved info lib directory: Finalize name.
 */
char *
getenv_default(const char *name, char *defvalue)
{
    char *value;
    value = getenv(name);
    return (value == NULL ? defvalue : value);
}

/* Set or remove status flags on the specified file descriptor
 * using the F_SETFL fcntl command. This function will not set
 * call fcntl if the flags are already set (this is determined
 * by issuing the F_GETFL fcntl command). SET determines if the
 * flags MODFLAGS are to be added (true) or removed (false).
 */
bool
fd_set_status_flags(int fd, bool set, int modflags)
{
    int curflags;
    int newflags;

    curflags = fcntl(fd, F_GETFL, 0);
    if (curflags < 0)
    	return false;
    if (set)
    	newflags = curflags | modflags;
    else
    	newflags = curflags & ~modflags;
    if (newflags == curflags)
	return true;

    return fcntl(fd, F_SETFL, newflags) != -1;
}

char *
in_addr_str(struct in_addr addr)
{
    static char buffer[16];
    unsigned char *bytes;
    bytes = (unsigned char *) &addr;
    snprintf(buffer, sizeof(buffer), "%d.%d.%d.%d",
    	    bytes[0], bytes[1], bytes[2], bytes[3]);
    return buffer;
}

char *
sockaddr_in_str(struct sockaddr_in *addr)
{
    static char buffer[22];
    unsigned char *bytes;

    bytes = (unsigned char *) &addr->sin_addr;
    snprintf(buffer, sizeof(buffer), "%d.%d.%d.%d:%u",
    	    bytes[0], bytes[1], bytes[2], bytes[3],
	    ntohs(addr->sin_port));

    return buffer;
}

PtrV *
wordwrap(const char *str, size_t len, size_t first_width, size_t other_width)
{
    size_t width;
    PtrV *out;

    out = ptrv_new();
    width = first_width;
    while (len > width) {
        size_t c, d;

        if (str[width] == ' ') {
            ptrv_append(out, xstrndup(str, width));
            for (c = width+1; c < len && str[c] == ' '; c++);
            str += c;
            len -= c;
        } else {

            for (c = width; c > 0 && str[c-1] != ' '; c--);
            for (d = width+1; d < len && str[d] != ' '; d++);
            if (d-c <= width) {
                ptrv_append(out, xstrndup(str, c-1));
                str += c;
                len -= c;
            } else {
                ptrv_append(out, xstrndup(str, width));
                str += width;
                len -= width;
            }
        }
        width = other_width;
    }
    if (len > 0)
        ptrv_append(out, xstrndup(str, len));

    return out;
}

char *
join_strings(char **strs, int count, char mid)
{
    int c;
    size_t len = count;
    char *out;
    char *p;

    for (c = 0; c < count; c++)
        len += strlen(strs[c]);
    p = out = xmalloc(len);
    for (c = 0; c < count ;c++) {
        p = stpcpy(p, strs[c]);
        *p = mid;
        p++;
    }
    p--;
    *p = '\0';

    return out;
}

struct dirent *
xreaddir(DIR *dh)
{
    errno = 0;
    return readdir(dh);
}

char *
elapsed_time_to_string(time_t elapsed, char *buf)
{
    char *s = buf;
    if (elapsed >= SECONDS_PER_DAY) {
        s += sprintf(s, "%lud", (long) (elapsed / SECONDS_PER_DAY));
        elapsed %= SECONDS_PER_DAY;
    }
    if (elapsed >= SECONDS_PER_HOUR) {
        s += sprintf(s, "%luh", (long) (elapsed / SECONDS_PER_HOUR));
        elapsed %= SECONDS_PER_HOUR;
    }
    if (elapsed >= SECONDS_PER_MINUTE) {
        s += sprintf(s, "%lum", (long) (elapsed / SECONDS_PER_MINUTE));
        elapsed %= SECONDS_PER_MINUTE;
    }
    if (elapsed > 0 || s == buf)
	s += sprintf(s, "%lus", (long) elapsed);
    return buf;
}
