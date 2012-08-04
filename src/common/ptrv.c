/* ptrv.c - A vector of address pointers
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
#include <string.h>
#include <sys/param.h>
#include <stdlib.h>
#include "xalloc.h"
#include "ptrv.h"

/* Create a new ptrv with specified size.
 */
static PtrV *
ptrv_new_with_size(uint32_t initial_size)
{
    PtrV *pv;

    pv = xmalloc(sizeof(PtrV));
    pv->cur = 0;
    pv->max = MAX(1, initial_size);
    pv->buf = xmalloc(pv->max * sizeof(void *));

    return pv;
}

/* Create a new ptrv that initially is as small as possible.
 */
PtrV *
ptrv_new(void)
{
    return ptrv_new_with_size(1);
}

/* Free the ptrv.
 */
void
ptrv_free(PtrV *pv)
{
    if (pv != NULL) {
	free(pv->buf);
	free(pv);
    }
}

/* Append a pointer to the ptrv.
 */
void
ptrv_append(PtrV *pv, void *value)
{
    if (pv->cur >= pv->max) {
        pv->max = MAX(1, pv->max*2);
        pv->buf = xrealloc(pv->buf, pv->max * sizeof(void *));
    }
    pv->buf[pv->cur] = value;
    pv->cur++;
}

/* Prepend a few values to the ptrv.
 */
void
ptrv_prepend_n(PtrV *pv, uint32_t count, void *value)
{
    uint32_t c;

    if (pv->cur+count > pv->max) {
        pv->max = MAX(pv->cur+count, pv->max*2);
        pv->buf = xrealloc(pv->buf, pv->max*sizeof(void *));
    }
    memmove(pv->buf+count, pv->buf, pv->cur*sizeof(void *));

    for (c = 0; c < count; c++)
	pv->buf[c] = value;
    pv->cur += count;
}

/* Remove a range of pointers from the ptrv.
 */
void
ptrv_remove_range(PtrV *pv, uint32_t start, uint32_t end)
{
    if (end < pv->cur)
        memmove(pv->buf+start, pv->buf+end, (pv->cur - end) * sizeof(void *));

    pv->cur -= end - start;
}

/* Remove the pointer from the ptrv.
 */
void *
ptrv_remove(PtrV *pv, uint32_t pos)
{
    void *value = NULL;
    if (pos >= 0 && pos < pv->cur) {
        value = pv->buf[pos];
        pv->cur--;
        if ((pv->cur-pos) > 0)
            memmove(pv->buf+pos, pv->buf+pos+1, (pv->cur-pos) * sizeof(void *));
    }

    return value;
}

/* Call some function (callback) for each pointer in the ptrv.
 */
void
ptrv_foreach(PtrV *pv, PtrVForeachCallback callback)
{
    unsigned c;

    for (c = 0; c < pv->cur; c++)
        callback(pv->buf[c]);
}

/* Remove all pointers from the ptrv.
 */
void
ptrv_clear(PtrV *pv)
{
    pv->cur = 0;
}

int32_t
ptrv_find(PtrV *pv, void *element, comparison_fn_t comparator)
{
    uint32_t c;

    for (c = 0; c < pv->cur; c++) {
	if (comparator(element, pv->buf[c]) == 0)
	    return c;
    }

    return -1;
}

void
ptrv_sort(PtrV *pv, comparison_fn_t comparator)
{
    qsort(pv->buf, pv->cur, sizeof(void *), comparator);
}

void
ptrv_insort(PtrV *pv, void *element, comparison_fn_t comparator)
{
    uint32_t c;

    /* XXX: this is slow but stable... */
    for (c = 0; c < pv->cur; c++) {
    	int cmp;

	cmp = comparator(element, pv->buf[c]);
	if (cmp <= 0) {
	    if (pv->cur >= pv->max) {
	        pv->max = MAX(1, pv->max * 2);
        	pv->buf = xrealloc(pv->buf, pv->max * sizeof(void *));
	    }
	    memmove(pv->buf+c+1, pv->buf+c, sizeof(void *)*(pv->cur-c));
    	    pv->buf[c] = element;
	    pv->cur++;
	    return;
	}
    }

    ptrv_append(pv, element);
}
