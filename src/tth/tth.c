/*
 * tth.c
 * This file is part of microdc
 *
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "tigertree.h"
#include "base32.h"


//#define _TRACE
#if defined(_TRACE)
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

const size_t default_block_level    = 6;
const int    max_block_count        = (1 << 6);

#define MAXHASHES ( ((word64)1) << 9  )
/* blocksize for one leaf is 65536 bytes */
#define STARTDEPTH 6

int calc_block_level(word64 filesize, int max_block_count)
{
    int level;
    for (level = default_block_level; (((word64)1) << (level+10)) * max_block_count < filesize; level++);
    return level;
}

char* tth(const char* filename, char **tthl, size_t *tthl_len)
{
    char *tth;
    size_t numbytes;
    unsigned char root[24];
    unsigned char *cur;
    TT_CONTEXT tt;
    unsigned char buf[1 + 512 * BLOCKSIZE];
    struct stat sb;
    unsigned leaf_cnt, level;
    size_t leaf_blocksize;

    int fd = open(filename, O_RDONLY);
    if ((fd == -1) || ( fstat(fd, &sb) == -1)) {
        return NULL;
    }

    level = calc_block_level(sb.st_size, max_block_count);
    leaf_blocksize = 1 << (level+10);

    //TRACE(("level == %d, leaf_blocksize == %d\n", level, leaf_blocksize));

    /* get memry for leaves */
    leaf_cnt = sb.st_size / leaf_blocksize;
    if (sb.st_size % leaf_blocksize)
	    leaf_cnt++;

    //TRACE(("leaf_cnt == %d\n", leaf_cnt));

    /*
    if (leaf_cnt >= 2) {
        *tthl_len = leaf_cnt * TIGERSIZE;
        *tthl = malloc(leaf_cnt * TIGERSIZE);
        if (*tthl == NULL)
            return NULL;
    } else */
    {
        *tthl_len = 0;
        *tthl = NULL;
    }

    tt_init(&tt, *tthl, level);
    tt.leaf = buf;
    buf[0] = '\0';

    while ( (numbytes = read(fd, &buf[1], sizeof(buf) - 1) ) > 0) {
        tt.index = BLOCKSIZE;
        for (cur = &buf[1]; cur + BLOCKSIZE <= &buf[numbytes + 1]; cur += BLOCKSIZE) {
            tt.leaf = cur - 1;
            tt.leaf[0] = '\0';
            tt_block(&tt);
        }
        tt.index = numbytes - (cur - &buf[1]);
        tt.leaf = cur - 1;
        tt.leaf[0] = '\0';
    }

    close(fd);

    tt_digest(&tt, root);

    if (*tthl_len) {
        if (*tthl_len != (tt.tthl - *(unsigned char **)tthl)) {
            fprintf(stderr, "*tthl_len == %d, (tt.tthl - *(unsigned char **)tthl) == %d\n", *tthl_len, (tt.tthl - *(unsigned char **)tthl));
            fflush(stderr);
        }
        assert(*tthl_len == (tt.tthl - *(unsigned char **)tthl));
    }

    tth = base32_encode(root, sizeof(root));

    return tth;
}
