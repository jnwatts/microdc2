/* quoting.h - Various routines for working with quoted strings
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

#ifndef QUOTING_H
#define QUOTING_H

#include <stdlib.h>

/*#define quote_word(s,q,e) quote_word_full((s),(q),(e),"","",false,false,false,true)*/
char *quote_word_full(const char *string, bool quoted, bool add_end_quote, const char *qc, const char *leading_qc, bool quote_non_print_hex, bool quote_non_print_oct, bool quote_non_print_c, bool quote_wc);
#define dequote_words(s,q,m) dequote_words_full((s),(q),false,true,true,(m))
char *dequote_words_full(const char *str, bool quoted, bool c_hex_unescape, bool c_oct_unescape, bool c_simple_unescape, const char *end);
int get_word_index(const char *str, int pos);
char *get_completion_word_dequoted(const char *str, int pos);
char *get_words_dequoted(const char *str, int index, size_t count);
int find_completion_word_start(const char *str, int pos); /* = find_word_start_backwards */

const char *find_word_start(const char *str, const char *maxend);
const char *find_word_end_termchar(const char *str, const char *maxend, char termchar);
#define find_word_end(str,maxend) find_word_end_termchar(str,maxend,'\0')
const char *find_unquoted_char(const char *str, const char *strmax, char ch);
const char *find_last_unquoted_char(const char *str, const char *strmax, char ch);

const char *find_unquoted_leading_char(const char *str, const char *strmax, char ch);

char *get_subwords_dequoted_termchar(const char *str, const char *strend, int index, size_t count, char termchar);
#define get_subwords_dequoted(str,strend,index,count) get_subwords_dequoted_termchar(str,strend,index,count,'\0')
#define get_subword_dequoted(str,strend,index) get_subwords_dequoted(str,strend,index,1)
#define get_subword_dequoted_termchar(str,strend,index,termchar) get_subwords_dequoted_termchar(str,strend,index,1,termchar)
#define get_words_dequoted(str,index,count) get_subwords_dequoted(str,NULL,index,count)
#define get_words_dequoted_termchar(str,index,count,termchar) get_subwords_dequoted_termchar(str,NULL,index,count,termchar)
#define get_word_dequoted(str,index) get_words_dequoted(str,index,1)
#define get_word_dequoted_termchar(str,index,termchar) get_words_dequoted_termchar(str,index,1,termchar)

char **get_word_array_dequoted(const char *str, const char *strend, int *argc);

int char_is_quoted(char *string, int index);

int count_unquoted_chars(const char *start, const char *end);

#endif
