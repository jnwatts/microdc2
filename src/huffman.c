/* huffman.c - Encoding and decoding of huffman data
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
#include <stdint.h>		/* Gnulib/POSIX/C99 */
#include <stdlib.h>		/* C89 */
#include <stdio.h>		/* C89 */
#include "xalloc.h"		/* Gnulib */
#include "gettext.h"		/* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "common/error.h"
#include "common/strbuf.h"
#include "common/ptrv.h"
#include "common/comparison.h"
#include "microdc.h"

#define BIT_POS_INIT(pos)      ((pos)*8)
#define GET_BIT(data,pos)      (data[(pos)/8] & (1 << ((pos)%8)))
#define SET_BIT(data,pos)      (data[(pos)/8] |= (1 << ((pos)%8)))
#define BYTE_BOUNDARY(pos)     (((pos) + 7) & ~7)

typedef struct _BranchNode BranchNode;
typedef struct _LeafNode LeafNode;
typedef struct _EncodeNode EncodeNode;
typedef struct _BitsNode BitsNode;

struct _BitsNode {
    uint32_t data;
    uint8_t bitcount;
};

struct _EncodeNode {
    uint32_t count;
    EncodeNode *left;
    EncodeNode *right;
    uint8_t value;
};

struct _BranchNode {
    BranchNode *left;
    BranchNode *right;
    int16_t chr;
};
struct _LeafNode {
    uint8_t chr;
    uint8_t len;
};

/* Allocate a new branch node for use in the Huffman decompression code. */
static BranchNode *
new_branch_node(void)
{
    BranchNode *node;
    
    node = xmalloc(sizeof(BranchNode));
    node->left = NULL;
    node->right = NULL;
    node->chr = -1;

    return node;
}

/* Free branch node used in the Huffman decompression code. */
static void
free_branch_node(BranchNode *node)
{
    if (node != NULL) {
        free_branch_node(node->left);
        free_branch_node(node->right);
        free(node);
    }
}

static void
free_encode_node(EncodeNode *node)
{
    if (node != NULL) {
        free_encode_node(node->left);
        free_encode_node(node->right);
        free(node);
    }
}

static int
compare_encode_node(EncodeNode *a, EncodeNode *b)
{
    if (a->count != b->count)
    	return a->count - b->count;
    if (a->left == NULL && b->left == NULL)
    	return -1;
    if (a->left == NULL)
    	return -1;
    return 1;
}

static void
make_huffman_bits(BitsNode bitnodes[256], EncodeNode *node, uint32_t bitcount, uint32_t data)
{
    if (node->left != NULL) {
	make_huffman_bits(bitnodes, node->left, bitcount+1, (data<<1) | 0);
	make_huffman_bits(bitnodes, node->right, bitcount+1, (data<<1) | 1);
    } else {
	bitnodes[node->value].bitcount = bitcount;
	bitnodes[node->value].data = data;
    }
}

static void
add_bits(uint8_t *data, uint64_t *bit_pos, uint32_t bits, uint8_t bitcount)
{
    uint32_t c;

    for (c = 0; c < bitcount; c++) {
    	if ((bits >> (bitcount-c-1)) & 1)
	    SET_BIT(data, *bit_pos);
	(*bit_pos) ++;
    }
}

char *
huffman_encode(const uint8_t *data, uint32_t data_size, uint32_t *out_size)
{
    uint32_t c;
    uint32_t counts[256];
    BitsNode bitnodes[256];
    EncodeNode *rootnode;
    uint8_t parity;
    StrBuf *out;
    uint16_t distinctchars = 0;
    PtrV *tree;
    uint64_t bits;
    uint64_t keybits;
    uint64_t bit_pos;
    uint8_t *bitdata;

    if (data_size == 0) {
    	char *outdata = xmemdup("HE3\xD\0\0\0\0\0\0\0", 11);
	*out_size = 11;
	return outdata;
    }

    tree = ptrv_new();

    memset(counts, 0, sizeof(counts));
    for (c = 0; c < data_size; c++)
    	counts[data[c]]++;

    for (c = 0; c < 256; c++) {
    	if (counts[c] > 0) {
	    EncodeNode *node;

	    node = xmalloc(sizeof(EncodeNode));
	    node->count = counts[c];
	    node->left = NULL;
	    node->right = NULL;
	    node->value = c;
   	    ptrv_insort(tree, node, (comparison_fn_t) compare_encode_node);
	    distinctchars++;
	}
    }

    while (tree->cur > 1) {
    	EncodeNode *node;
	
	node = xmalloc(sizeof(EncodeNode));
	node->left = ptrv_remove_first(tree);
	node->right = ptrv_remove_first(tree);
	node->count = node->left->count + node->right->count;
	node->value = 0;

    	ptrv_insort(tree, node, (comparison_fn_t) compare_encode_node);
    }

    rootnode = ptrv_remove_first(tree);
    ptrv_free(tree);

    memset(bitnodes, 0, sizeof(bitnodes));
    make_huffman_bits(bitnodes, rootnode, 0, 0);

    parity = 0;
    for (c = 0; c < data_size; c++)
    	parity ^= data[c];

    out = strbuf_new();
    strbuf_append(out, "HE3\xD");
    strbuf_append_char(out, parity);
    strbuf_append_data(out, &data_size, sizeof(uint32_t));
    strbuf_append_data(out, &distinctchars, sizeof(uint16_t));
    
    bits = 0;
    keybits = 0;
    for (c = 0; c < 256; c++) {
    	if (counts[c] != 0) {
    	    strbuf_append_char(out, c);
    	    strbuf_append_char(out, bitnodes[c].bitcount);
	    bits += bitnodes[c].bitcount * counts[c];
	    keybits += bitnodes[c].bitcount;
	}
    }

    bits = BYTE_BOUNDARY(bits) + BYTE_BOUNDARY(keybits);
    bitdata = (uint8_t *)calloc(1, bits/8);
    if (!bitdata)
	    return NULL;
    bit_pos = 0;
    for (c = 0; c < 256; c++) {
    	if (counts[c] != 0)
	    add_bits(bitdata, &bit_pos, bitnodes[c].data, bitnodes[c].bitcount);
    }
    bit_pos = BYTE_BOUNDARY(bit_pos);
    for (c = 0; c < data_size; c++) {
    	int ch = data[c];
	add_bits(bitdata, &bit_pos, bitnodes[ch].data, bitnodes[ch].bitcount);
    }

    strbuf_append_data(out, bitdata, bits/8);
    free_encode_node(rootnode);
    *out_size = strbuf_length(out);
    free(bitdata);
    return strbuf_free_to_string(out);
}

/* Decompress a Huffman compressed stream of data.
 * The returned string should be freed with free.
 */
char *
huffman_decode(const uint8_t *data, uint32_t data_size, uint32_t *out_size)
{
    uint32_t data_pos;
    uint32_t unpack_size;
    uint16_t leaf_count;
    uint8_t *output;
    uint32_t output_pos;
    uint32_t bit_pos;
    uint32_t leaf_data_len;
    uint8_t parity;
    BranchNode *root;
    int c;

    if (data_size < 11)
    	return NULL;

    if (data[0] != 'H' || data[1] != 'E')
        return NULL;
    if (data[2] != '3' && data[2] != '0')
        return NULL;
    unpack_size = *(uint32_t *) (data+5);
    leaf_count = *(uint16_t *) (data+9);

    if (data_size < 11 + leaf_count*2)
    	return NULL;

    LeafNode leaves[leaf_count];

    data_pos = 11;
    leaf_data_len = 0;
    for (c = 0; c < leaf_count; c++) {
        leaves[c].chr = data[data_pos++];
        leaves[c].len = data[data_pos++];
	leaf_data_len += leaves[c].len;
    }

    if (data_size < data_pos + BYTE_BOUNDARY(leaf_data_len)/8)
    	return NULL;

    root = new_branch_node();

    bit_pos = BIT_POS_INIT(data_pos);
    for (c = 0; c < leaf_count; c++) {
        BranchNode *node = root;
        int d;

        for (d = 0; d < leaves[c].len; d++) {
            if (GET_BIT(data, bit_pos)) {
                if (node->right == NULL)
                    node->right = new_branch_node();
                node = node->right;
            } else {
                if (node->left == NULL)
                    node->left = new_branch_node();
                node = node->left;
            }
	    bit_pos++;
        }
 
        node->chr = leaves[c].chr;
    }
    bit_pos = BYTE_BOUNDARY(bit_pos);

    output = xmalloc(unpack_size+1);
    output_pos = 0;
    parity = 0;
    for (c = 0; c < unpack_size; c++) {
        BranchNode *node = root;
        while (node->chr == -1) {
	    if (BYTE_BOUNDARY(bit_pos)/8 > data_size) {
	    	free(output);
	    	return NULL;
	    }
            if (GET_BIT(data, bit_pos)) {
                node = node->right;
            } else {
                node = node->left;
            }
	    bit_pos++;
            if (node == NULL) {
                free(output);
                free_branch_node(root);
                return NULL;
            }
        }
        output[output_pos++] = node->chr;
	parity ^= node->chr;
    }
    output[output_pos] = '\0';

    if (parity != data[4])
    	warn(_("Incorrect parity, ignoring\n"));

    free_branch_node(root);

    if (out_size != NULL)
    	*out_size = unpack_size;

    return (char *) output;
}
