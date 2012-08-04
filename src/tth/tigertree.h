/* (PD) 2001 The Bitzi Corporation
 * Copyright (C) 2006 Alexey Illarionov <littlesavage@rambler.ru>
 * Please see file COPYING or http://bitzi.com/publicdomain 
 * for more info.
 *
 */
#include "tiger.h"

/* tiger hash result size, in bytes */
#define TIGERSIZE 24

/* size of each block independently tiger-hashed, not counting leaf 0x00 prefix */
#define BLOCKSIZE 1024

/* size of input to each non-leaf hash-tree node, not counting node 0x01 prefix */
#define NODESIZE (TIGERSIZE*2)

/* default size of interim values stack, in TIGERSIZE
 * blocks. If this overflows (as it will for input
 * longer than 2^64 in size), havoc may ensue. */
#define STACKSIZE TIGERSIZE*56

typedef struct tt_context {
  word64 count;                   /* total blocks processed */
#if 0
  unsigned char leaf[1+BLOCKSIZE]; /* leaf in progress */
  unsigned char *block;            /* leaf data */
#endif
  unsigned char	*leaf;		  /* leaf in progress */
  unsigned char node[1+NODESIZE]; /* node scratch space */
  int index;                      /* index into block */
  unsigned char *top;             /* top (next empty) stack slot */
  unsigned char *tthl;		  /* index into buf for tth leaves */
  unsigned  depth;		  /* num of generations in tree to discard */
  unsigned char nodes[STACKSIZE]; /* stack of interim node values */
} TT_CONTEXT;

#if defined(__cplusplus)
extern "C" {
#endif
void tt_init(TT_CONTEXT *ctx, unsigned char *tthl, unsigned depth);
//void tt_update(TT_CONTEXT *ctx, unsigned char *buffer, word32 len);
void tt_block(TT_CONTEXT *ctx);
void tt_digest(TT_CONTEXT *ctx, unsigned char *hash);
void tt_copy(TT_CONTEXT *dest, TT_CONTEXT *src);
#if defined(__cplusplus)
}
#endif
