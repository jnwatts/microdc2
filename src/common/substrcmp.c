/* substrcmp.c - The substrcmp function
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

#include "substrcmp.h"

int
substrcmp(const char *s1, const char *s2, size_t s2len)
{
    while (s2len > 0) {
    	if (*s1 != *s2)
	    return *s1 - *s2;
	s1++;
	s2++;
	s2len--;
    }
    return *s1; /* strncmp would return 0 here */
}
