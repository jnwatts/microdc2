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

#ifndef PTRV_H
#define PTRV_H

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include "comparison.h"

typedef void (*PtrVForeachCallback)(void *);

typedef struct _PtrV PtrV;

struct _PtrV {
    void **buf;     /* Pointer buffer */
    uint32_t cur;   /* Current number of pointers used in buffer */
    uint32_t max;   /* Number of pointers (used or not) in buffer */
};

/* The good ones */
PtrV *ptrv_new(void);
void ptrv_free(PtrV *pv);
void ptrv_append(PtrV *pv, void *value);
void ptrv_foreach(PtrV *pv, PtrVForeachCallback callback);
void ptrv_clear(PtrV *pv);

/* The bad ones */
void ptrv_remove_range(PtrV *pv, uint32_t start, uint32_t end); /* XXX: rename ptrv_remove_n? */
void *ptrv_remove(PtrV *pv, uint32_t pos);
#define ptrv_remove_first(p)    ptrv_remove(p, 0)
void ptrv_insort(PtrV *pv, void *element, comparison_fn_t comparator);
void ptrv_sort(PtrV *pv, comparison_fn_t comparator);
#define ptrv_prepend(p,v) ptrv_prepend_n(p,1,v);
void ptrv_prepend_n(PtrV *pv, uint32_t count, void *value);

int32_t ptrv_find(PtrV *pv, void *element, comparison_fn_t comparator);

#endif
