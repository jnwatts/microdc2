/* bksearch.h - Binary search with key indexing support
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

#ifndef BKSEARCH_H
#define BKSEARCH_H

#include <stdlib.h>
#include "comparison.h"

const void *bksearch(const void *key, const void *base, size_t nmemb,
		    size_t size, size_t keyoffs, comparison_fn_t cmp);
bool bksearchrange(const void *key, const void *base, size_t nmemb,
                   size_t size, size_t keyoffs, comparison_fn_t cmp,
		   const void **first_match, const void **last_match);
bool bsearchrange(const void *key, const void *base, size_t nmemb,
                   size_t size, comparison_fn_t cmp,
		   const void **first_match, const void **last_match);

#endif
