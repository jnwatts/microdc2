/* tempdir.c - Get path of temporary directory
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

static bool
direxists(const char *dir)
{
    struct stat sb;
    return stat(dir, &sb) == 0 && S_ISDIR(sb.st_mode);
}

/* This does in no way replace the mkdtemp function provided by Gnulib.
 * mkdtemp still needs to know which directory to create the temporary
 * directory inside - it doesn't figure that out by itself.
 */
char *
tempdir(void)
{
    char *tmpdir;

    if (getuid() == geteuid() && getgid() == getegid()) {
        tmpdir = getenv("TMPDIR");
        if (tmpdir != NULL && direxists(tmpdir))
            return tmpdir;
    }
    tmpdir = P_tmpdir;
    if (tmpdir != NULL && direxists(tmpdir))
        return tmpdir;

    tmpdir = "/tmp";
    if (direxists(tmpdir))
        return tmpdir;

    errno = ENOENT;
    return NULL;
}
