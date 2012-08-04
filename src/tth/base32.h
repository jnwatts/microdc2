#ifndef __BASE32_H
#define __BASE32_H

#if defined(__cplusplus)
extern "C" {
#endif

// user must call free() funtion on pointer returned from these functions

char* base32_encode(const unsigned char* in, int inlen);

#if defined(__cplusplus)
}
#endif

#endif // ifndef __BASE32_H
