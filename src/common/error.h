/* error.h - Generic functions for error management and reporting
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

#ifndef COMMON_ERROR_H
#define COMMON_ERROR_H

#include <stdarg.h> 	/* for ... */
#include <errno.h>  	/* for errstr */
#include <string.h> 	/* for strerror */
#include <stdio.h>	/* for FILE */

/* Current error message as a string. */
#define errstr (strerror(errno))

typedef int (*vprintf_fn_t)(const char *format, va_list ap);
typedef int (*vfprintf_fn_t)(FILE *stream, const char *format, va_list ap);

extern vprintf_fn_t warn_writer;

int default_warn_writer(const char *format, va_list ap);
void die(const char *format, ...) __attribute__ ((noreturn, format (printf, 1, 2)));
void warn(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

#endif
