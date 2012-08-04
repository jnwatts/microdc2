/* (PD) 2001 The Bitzi Corporation
 * Copyright (C) 2006 Alexey Illarionov <littlesavage@rambler.ru>
 *
 * Please see file COPYING or http://bitzi.com/publicdomain 
 * for more info.
 *
 * tigertree.c - Implementation of the TigerTree algorithm
 *
 * NOTE: The TigerTree hash value cannot be calculated using a
 * constant amount of memory; rather, the memory required grows
 * with the size of input. (Roughly, one more interim value must
 * be remembered for each doubling of the input size.) The
 * default TT_CONTEXT struct size reserves enough memory for
 * input up to 2^64 in length
 *
 * Requires the tiger() function as defined in the reference
 * implementation provided by the creators of the Tiger
 * algorithm. See
 *
 *    http://www.cs.technion.ac.il/~biham/Reports/Tiger/
 *
 */

#include <assert.h>
#include <string.h>
#include "tigertree.h"

//#define _TRACE
#if defined(_TRACE)
#include <stdio.h>
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

#ifdef _WIN32
#undef WORDS_BIGENDIAN
#else
#include "../../config.h"
#endif

#ifdef WORDS_BIGENDIAN
#   define USE_BIG_ENDIAN 1
void tt_endian(byte *s);
#else
#   define USE_BIG_ENDIAN 0
#endif

/* Initialize the tigertree context */
void tt_init(TT_CONTEXT *ctx, unsigned char *tthl, unsigned depth)
{
  ctx->count = 0;
  ctx->node[0] = '\1'; // flag for inner node calculation -- never changed
  ctx->index = 0;   // partial block pointer/block length
  ctx->top = ctx->nodes;
  ctx->tthl = tthl;
  ctx->depth = depth;
}

static void tt_compose(TT_CONTEXT *ctx) {
  byte *node = ctx->top - NODESIZE;
  memmove((ctx->node)+1,node,NODESIZE); // copy to scratch area
  tiger((word64*)(ctx->node),(word64)(NODESIZE+1),(word64*)(ctx->top)); // combine two nodes
#if USE_BIG_ENDIAN
  tt_endian((byte *)ctx->top);
#endif
  memmove(node,ctx->top,TIGERSIZE);           // move up result
  ctx->top -= TIGERSIZE;                      // update top ptr
}

void tt_block(TT_CONTEXT *ctx)
{
    word64 b;
    unsigned depth;

    tiger((word64*)ctx->leaf,(word64)ctx->index+1,(word64*)ctx->top);
#if USE_BIG_ENDIAN
    tt_endian((byte *)ctx->top);
#endif
    ctx->top += TIGERSIZE;
    ++ctx->count;
    b = ctx->count;
    depth = 0;
    while(b == ((b >> 1)<<1)) { // while evenly divisible by 2...
        /*
        if (depth == ctx->depth) {
            TRACE(("depth = %d, b == %lld\n", depth, b));
            memmove(ctx->tthl, ctx->top - 2*TIGERSIZE, 2*TIGERSIZE);
            ctx->tthl += 2*TIGERSIZE;
        }
        */
        tt_compose(ctx);
        b = b >> 1;
        depth++;
    }
}

// no need to call this directly; tt_digest calls it for you
static void tt_final(TT_CONTEXT *ctx)
{
  // do last partial block, unless index is 1 (empty leaf)
  // AND we're past the first block
  if((ctx->index>0)||(ctx->top==ctx->nodes))
    tt_block(ctx);
}

void tt_digest(TT_CONTEXT *ctx, byte *s)
{
    unsigned dth_top0 = 0, dth_top1 = 0;
    word64 tmp, cnt;
    tt_final(ctx);

    /*
    dth_top0 = 0;
    cnt = ctx->count;
    tmp = cnt;
    while(tmp == ((tmp >> 1)<<1)) {
        dth_top0++;
        tmp >>= 1;
    }
    cnt -= ((word64)1 << dth_top0);

    TRACE(("ctx->top-TIGERSIZE - ctx->nodes == %d\n", ctx->top-TIGERSIZE - ctx->nodes));
    */

    while( (ctx->top-TIGERSIZE) > ctx->nodes) {
        /*
        assert(cnt > 0);

        dth_top1 = 0;
        tmp = cnt;
        while (tmp == ((tmp >> 1)<<1)) {
            dth_top1++;
            tmp >>= 1;
        }
        cnt -= ((word64)1 << dth_top1);

        if ((ctx->depth <= dth_top1) && (ctx->depth >= dth_top0)) {
            if ( dth_top1 == ctx->depth) {
                TRACE(("depth = %d, dth_top0 == %d, dth_top1 == %d\n", ctx->depth, dth_top0, dth_top1));
                memmove(ctx->tthl, ctx->top - 2*TIGERSIZE, 2*TIGERSIZE);
                ctx->tthl += 2*TIGERSIZE;
            } else {
                TRACE(("depth = %d, dth_top0 == %d, dth_top1 == %d\n", ctx->depth, dth_top0, dth_top1));
                memmove(ctx->tthl, ctx->top - TIGERSIZE, TIGERSIZE);
                ctx->tthl += TIGERSIZE;
            }
        }
        */
        tt_compose(ctx);
        dth_top0 = dth_top1 + 1;
    }

    memmove(s,ctx->nodes, TIGERSIZE);
}

#if USE_BIG_ENDIAN
void tt_endian(byte *s)
{
  word64 *i;
  byte   *b, btemp;
  word16 *w, wtemp;

  for(w = (word16 *)s; w < ((word16 *)s) + 12; w++)
  {
      b = (byte *)w;
      btemp = *b;
      *b = *(b + 1);
      *(b + 1) = btemp;
  }

  for(i = (word64 *)s; i < ((word64 *)s) + 3; i++)
  {
      w = (word16 *)i;

      wtemp = *w;
      *w = *(w + 3);
      *(w + 3) = wtemp;

      wtemp = *(w + 1);
      *(w + 1) = *(w + 2);
      *(w + 2) = wtemp;
  }
}
#endif

