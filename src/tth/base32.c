#include <config.h>

#include <stdlib.h>

#include "base32.h"

char base32_alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

char* base32_encode(const unsigned char* buffer, int len)
{
    unsigned char* digest = NULL;
    if (len > 0) {
        int i, j = 0;
        int bits_remain = 0;
        unsigned short value = 0;
        unsigned long digest_len = len*8;
        digest_len = 1 + ((digest_len % 5) == 0 ? (digest_len / 5) : (digest_len / 5) + 1);

        digest = (unsigned char*)malloc(digest_len);
        if (digest != NULL) {
            for (i = 0; i < len; i++) {
                value = (value << 8) | buffer[i];
                bits_remain += 8;
                while (bits_remain > 5 && j < 1023) {
                    int idx = (value >> (bits_remain-5)) & 0x1F;
                    digest[j++] = base32_alphabet[idx];
                    bits_remain -= 5;
                }
            }
            if (bits_remain > 0) {
                int idx = (value << (5-bits_remain)) & 0x1F;
                digest[j++] = base32_alphabet[idx];
            }
            digest[j] = '\0';
        }
    }
    return digest;
}

#if 0
char* base32_decode(const unsigned char* buffer, int len)
{
    int i, j = 0;
    int bits_remain = 0;
    unsigned short value = 0;

    for (i = 0; i < len; i++) {
        value |= (buffer[i] << bits_remain);
        bits_remain += 8;
        while (bits_remain > 5 && j < 1023) {
            base32_digest[j++] = (value & 0x1F) < 26 ? 'A'+(value & 0x1F) : '2' + (value & 0x1F);
            value >>= 5;
            bits_remain -= 5;
        }
    }
    if (bits_remain > 0) {
        base32_digest[j++] = (value & 0x1F) < 26 ? 'A'+(value & 0x1F) : '2' + (value & 0x1F);
    }
    base32_digest[j] = '\0';
    return base32_digest;
}

#endif
