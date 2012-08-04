/* bksearch.c - Binary search with key indexing support
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

#include <config.h>
#include <stdlib.h>
#include <stdbool.h>
#include "bksearch.h"

static const void *
bsearchpartial(const void *key, const void *base, size_t l, size_t u, size_t size,
	       comparison_fn_t cmp, bool matches_above)
{
    while (l < u) {
	size_t idx;
	const void *p;
	int comparison;

	idx = (l + u) / 2;
	p = (const void *) (((const char *) base) + (idx * size));
	comparison = (*cmp)(key, p);
	if ((comparison == 0) == matches_above)
	    u = idx;
	else
	    l = idx + 1;
    }
    if (!matches_above)
	l--;

    return (const void *) (((const char *) base) + (l * size));
}


static const void *
bksearchpartial(const void *key, const void *base, size_t l, size_t u, size_t size,
                size_t keyoffs, comparison_fn_t cmp, bool matches_above)
{
    while (l < u) {
	size_t idx;
	const void *p;
	int comparison;

	idx = (l + u) / 2;
	p = (const void *) (((const char *) base) + (idx * size));
	comparison = (*cmp)(key, *(const void **) (((const char *) p) + keyoffs));
	if ((comparison == 0) == matches_above)
	    u = idx;
	else
	    l = idx + 1;
    }
    if (!matches_above)
	l--;

    return (const void *) (((const char *) base) + (l * size));
}

/* Find first and last match in an array using binary search.
 */
bool
bsearchrange(const void *key, const void *base, size_t nmemb, size_t size,
	     comparison_fn_t cmp, const void **first_match,
             const void **last_match)
{
    const void *match;
    size_t idx;

    match = bsearch(key, base, nmemb, size, cmp);
    if (match == NULL) {
	if (first_match != NULL)
	    *first_match = NULL;
	if (last_match != NULL)
	    *last_match = NULL;
	return false;
    }
    idx = (((const char *) match) - ((const char *) base)) / size;
    if (first_match != NULL)
	*first_match = bsearchpartial(key, base, 0, idx, size, cmp, true);
    if (last_match != NULL)
	*last_match = bsearchpartial(key, base, idx+1, nmemb, size, cmp, false);
    return true;
}

/* Find first and last match in an array using binary search.
 * See bksearch for details regarding the keyoffs argument.
 */
bool
bksearchrange(const void *key, const void *base, size_t nmemb, size_t size,
              size_t keyoffs, comparison_fn_t cmp, const void **first_match,
	      const void **last_match)
{
    const void *match;
    size_t idx;

    match = bksearch(key, base, nmemb, size, keyoffs, cmp);
    if (match == NULL) {
	if (first_match != NULL)
	    *first_match = NULL;
	if (last_match != NULL)
	    *last_match = NULL;
	return false;
    }
    idx = (((const char *) match) - ((const char *) base)) / size;
    if (first_match != NULL)
	*first_match = bksearchpartial(key, base, 0, idx, size, keyoffs, cmp, true);
    if (last_match != NULL)
	*last_match = bksearchpartial(key, base, idx+1, nmemb, size, keyoffs, cmp, false);
    return true;
}

/* Find a match in an array using binary search.
 * The keyoffs argument specified offset to key pointer in each member.
 * The key pointer will be passed as second argument to the comparison
 * function cmp.
 */
const void *
bksearch(const void *key, const void *base, size_t nmemb, size_t size,
         size_t keyoffs, comparison_fn_t cmp)
{
    size_t l, u, idx;
    const void *p;
    int comparison;

    l = 0;
    u = nmemb;
    while (l < u) {
	idx = (l + u) / 2;
	p = (const void *) (((const char *) base) + (idx * size));
	comparison = (*cmp)(key, *(const void **) (((const char *) p) + keyoffs));
	if (comparison < 0)
	    u = idx;
	else if (comparison > 0)
	    l = idx + 1;
	else
	    return p;
    }
    
    return NULL;
}
