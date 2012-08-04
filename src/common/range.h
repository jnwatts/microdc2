/* range.h - Parsing of simple range expressions
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
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef COMMON_RANGE_H
#define COMMON_RANGE_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*RangeCallback)(uint32_t start, uint32_t end, void *userdata);

bool foreach_in_range(const char *range, uint32_t start, uint32_t end, RangeCallback callback, void *userdata);

#endif
