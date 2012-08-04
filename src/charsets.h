#ifndef __CHARSETS_H
#define __CHARSETS_H

//#define DEBUG_DUMP_TRANSLATION
#if defined(DEBUG_DUMP_TRANSLATION)
#include <stdio.h>

#define DUMP_TRANSLATION(from, to, from_str, to_str)    \
    fprintf(stderr, "%08lX: %s -> %s: %s -> %s\n", (unsigned long)from##_to_##to##_iconv, from##_charset, to##_charset, from_str, to_str); \
    fflush(stderr)

#define ERROR_TRANSLATION_OPEN(from, to)    \
    if (from##_to_##to##_iconv == (iconv_t)-1) {                       \
        fprintf(stderr, "%s:%d: cannot create translation from %s to %s\n", __FUNCTION__, __LINE__, from##_charset, to##_charset); \
        fflush(stderr);                                                \
    }
#else
#define DUMP_TRANSLATION(from, to, from_str, to_str)

#define ERROR_TRANSLATION_OPEN(from, to)
#endif

#if !HAVE_ICONV
typedef int iconv_t;
#endif

#define no_iconv ((const iconv_t) -1)

#define EXPORT_CHARSET(set)                     extern char* set##_charset
#define EXPORT_CONST_CHARSET(set)               extern const char* set##_charset
#define DECLARE_CHARSET(set)                    char* set##_charset = 0
#define DECLARE_CONST_CHARSET(set, value)       const char* set##_charset = value

#define EXPORT_CONVERSION(from, to)                                     \
    char* from##_to_##to##_string(const char* str);                     \
    char* try_##from##_to_##to##_string(const char* str);               

#if HAVE_ICONV
#define DECLARE_CONVERSION(from, to)                                    \
    char* from##_to_##to##_string(const char* str)                      \
    {                                                                   \
        char *converted = 0;                                            \
        if (str != 0) {                                                 \
            if (from##_to_##to##_iconv != (iconv_t)-1) {                \
                converted = iconv_alloc(from##_to_##to##_iconv, str);   \
            }                                                           \
            if (converted == 0) {                                       \
                converted = xstrdup(str);                               \
                DUMP_TRANSLATION(from, to, str, "FAILED");              \
            } else {                                                    \
                DUMP_TRANSLATION(from, to, str, converted);             \
            }                                                           \
        }                                                               \
        return converted;                                               \
    }                                                                   \
    char* try_##from##_to_##to##_string(const char* str)                \
    {                                                                   \
        char *converted = 0;                                            \
        if (str != 0) {                                                 \
            if (from##_to_##to##_iconv != (iconv_t)-1) {                \
                converted = iconv_alloc(from##_to_##to##_iconv, str);   \
            }                                                           \
        }                                                               \
        return converted;                                               \
    }
#else
#define DECLARE_CONVERSION(from, to)                                    \
    char* from##_to_##to##_string(const char* str)                      \
    {                                                                   \
        return xstrdup(str);                                            \
    }                                                                   \
    char* try_##from##_to_##to##_string(const char* str)                \
    {                                                                   \
        return 0;                                                       \
    }
#endif

#define EXPORT_ICONV_CONVERSION(from, to)                               \
    EXPORT_CONVERSION(from, to)

#define DECLARE_ICONV_CONVERSION(from, to)                              \
    iconv_t from##_to_##to##_iconv = (iconv_t)no_iconv;                 \
    DECLARE_CONVERSION(from, to)

#if HAVE_ICONV

#define ICONV_OPEN(from, to) \
    if (from##_charset != 0 && to##_charset != 0) { \
        from##_to_##to##_iconv = iconv_open(to##_charset, from##_charset); \
        ERROR_TRANSLATION_OPEN(from, to);                                  \
    }

#define ICONV_CLOSE(from, to)   \
    if (from##_to_##to##_iconv != no_iconv) { \
        iconv_close(from##_to_##to##_iconv);  \
        from##_to_##to##_iconv = no_iconv;    \
    }

#else // HAVE_ICONV

#define ICONV_OPEN(from, to)
#define ICONV_CLOSE(from, to)

#endif // HAVE_ICONV

#endif // ifndef __CHARSETS_H
