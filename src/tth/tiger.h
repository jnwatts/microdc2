/* (PD) 2001 The Bitzi Corporation
 * Please see file COPYING or http://bitzi.com/publicdomain 
 * for more info.
 *
 * $Id: tiger.h,v 1.1 2006/11/16 11:21:20 chugunov Exp $
 */
#ifndef TIGER_H
#define TIGER_H

#ifdef _WIN32

#if defined (_INTEGRAL_MAX_BITS) && (_INTEGRAL_MAX_BITS >= 64)
  typedef unsigned __int64 word64;
#else
  #error __int64 type not supported
#endif
typedef unsigned long int tword;

#else
#include <sys/types.h>
typedef u_int64_t  word64;
typedef u_int32_t  word32;
typedef u_int16_t  word16;
#endif

#ifndef __BYTE__
#define __BYTE__
typedef unsigned char  byte;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

void tiger(word64 *str, word64 length, word64 *res);

#if defined(__cplusplus)
}
#endif

#endif
