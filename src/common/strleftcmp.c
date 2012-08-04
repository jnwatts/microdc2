/* strleftcmp.c - The strleftcmp function
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

#include "strleftcmp.h"

/* Determine if 'full' starts with 'base'.
 *
 * XXX: Rename this starts_with? strendcmp(full, base)?
 * XXX: Add ends_with? strstartcmp(full, base)?
 */
int
strleftcmp(const char *base, const char *full)
{
    while (*base) {
	if (*base != *full)
    	    return *base - *full;
	base++;
	full++;
    }
    return 0;
}
