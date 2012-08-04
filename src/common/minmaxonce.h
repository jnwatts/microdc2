/* minmaxonce.h - min and max macros evaluating arguments once (for GCC)
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

#ifndef MINMAXONCE_H
#define MINMAXONCE_H

#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef min
#define min(a,b)	({ typeof(a) _a = a; typeof(b) _b = b; _a < _b ? _a : _b; })
#endif

#ifndef max
#define max(a,b)	({ typeof(a) _a = a; typeof(b) _b = b; _a > _b ? _a : _b; })
#endif

#endif
