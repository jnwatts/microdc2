/* error.c - Generic functions for error management and reporting
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
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include "error.h"

/* This function will be called by warn and die to write text to the screen
 * (or log file, etc).
 */
vprintf_fn_t warn_writer = default_warn_writer;

/* The default warning writer, which writes warnings to stderr.
 */
int
default_warn_writer(const char *format, va_list ap)
{
    return vfprintf(stderr, format, ap);
}

/* Print a message to screen and terminate the program.
 */
void
die(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    warn_writer(format, args);
    va_end(args);

    exit(EXIT_FAILURE);
}

/* Print a message to screen.
 */
void
warn(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    warn_writer(format, args);
    va_end(args);
}
