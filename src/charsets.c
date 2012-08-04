#include <config.h>

#include <string.h>
#include <stdlib.h>		/* C89 */
#include <langinfo.h>		/* POSIX (XSI) */

#include "xalloc.h"
#include "iconvme.h"		/* Gnulib */
#include "microdc.h"

#include "charsets.h"

DECLARE_CHARSET(hub);
DECLARE_CHARSET(fs);
DECLARE_CHARSET(main);
DECLARE_CHARSET(log);
DECLARE_CONST_CHARSET(utf8, "UTF-8");

/* conversion functions */

DECLARE_ICONV_CONVERSION(main, hub);
DECLARE_ICONV_CONVERSION(hub, main);
DECLARE_ICONV_CONVERSION(main, fs);
DECLARE_ICONV_CONVERSION(fs, main);
DECLARE_ICONV_CONVERSION(hub, fs);
DECLARE_ICONV_CONVERSION(fs, hub);
DECLARE_ICONV_CONVERSION(main, log);
DECLARE_ICONV_CONVERSION(log, main);

#if defined(HAVE_LIBXML2)

DECLARE_ICONV_CONVERSION(utf8, fs);
DECLARE_ICONV_CONVERSION(fs, utf8);
DECLARE_ICONV_CONVERSION(utf8, main);
DECLARE_ICONV_CONVERSION(main, utf8);
DECLARE_ICONV_CONVERSION(utf8, hub);
DECLARE_ICONV_CONVERSION(hub, utf8);

#endif

/* charset update functions */

void set_main_charset(const char* charset);
void set_hub_charset(const char* charset);
void set_fs_charset(const char* charset);

void set_main_charset(const char* charset)
{
    if ((charset == NULL && main_charset == NULL) ||
        (charset != NULL && main_charset != NULL && strcasecmp(charset, main_charset) == 0)) {
        return;
    } else {
        /* clearing the conversion to/from this charset */
        ICONV_CLOSE(main, hub);
        ICONV_CLOSE(hub, main);
        ICONV_CLOSE(main, fs);
        ICONV_CLOSE(fs, main);
        ICONV_CLOSE(main, log);
        ICONV_CLOSE(log, main);
#if defined(HAVE_LIBXML2)
        ICONV_CLOSE(main, utf8);
        ICONV_CLOSE(utf8, main);
#endif

        if (main_charset != NULL) {
            free(main_charset);
            main_charset = NULL;
        }

        if (charset != NULL) {
            if (*charset == '\0') {
                main_charset = xstrdup(nl_langinfo(CODESET));
            } else {
                main_charset = xstrdup(charset);
            }

            ICONV_OPEN(main, hub);
            ICONV_OPEN(hub, main);
            ICONV_OPEN(main, fs);
            ICONV_OPEN(fs, main);
            ICONV_OPEN(main, log);
            ICONV_OPEN(log, main);
#if defined(HAVE_LIBXML2)
            ICONV_OPEN(main, utf8);
            ICONV_OPEN(utf8, main);
#endif
        }
    }
}

void set_hub_charset(const char* charset)
{
    if ((charset == NULL && hub_charset == NULL) ||
        (charset != NULL && hub_charset != NULL && strcasecmp(charset, hub_charset) == 0)) {
        return;
    } else {
        /* clearing the conversion to/from this charset */
        ICONV_CLOSE(main, hub);
        ICONV_CLOSE(hub, main);
        ICONV_CLOSE(hub, fs);
        ICONV_CLOSE(fs, hub);
#if defined(HAVE_LIBXML2)
        ICONV_CLOSE(hub, utf8);
        ICONV_CLOSE(utf8, hub);
#endif

        if (hub_charset != NULL) {
            free(hub_charset);
            hub_charset = NULL;
        }

        if (charset != NULL) {
            if (*charset == '\0') {
                hub_charset = xstrdup(nl_langinfo(CODESET));
            } else {
                hub_charset = xstrdup(charset);
            }

            ICONV_OPEN(main, hub);
            ICONV_OPEN(hub, main);
            ICONV_OPEN(hub, fs);
            ICONV_OPEN(fs, hub);
#if defined(HAVE_LIBXML2)
            ICONV_OPEN(hub, utf8);
            ICONV_OPEN(utf8, hub);
#endif
        }
    }
}

void set_fs_charset(const char* charset)
{
    if ((charset == NULL && fs_charset == NULL) ||
        (charset != NULL && fs_charset != NULL && strcasecmp(charset, fs_charset) == 0)) {
        return;
    } else {
        /* clearing the conversion to/from this charset */
        ICONV_CLOSE(fs, hub);
        ICONV_CLOSE(hub, fs);
        ICONV_CLOSE(main, fs);
        ICONV_CLOSE(fs, main);
#if defined(HAVE_LIBXML2)
        ICONV_CLOSE(fs, utf8);
        ICONV_CLOSE(utf8, fs);
#endif

        if (fs_charset != NULL) {
            free(fs_charset);
            fs_charset = NULL;
        }

        if (charset != NULL) {
            if (*charset == '\0') {
                fs_charset = xstrdup(nl_langinfo(CODESET));
            } else {
                fs_charset = xstrdup(charset);
            }

            ICONV_OPEN(fs, hub);
            ICONV_OPEN(hub, fs);
            ICONV_OPEN(main, fs);
            ICONV_OPEN(fs, main);
#if defined(HAVE_LIBXML2)
            ICONV_OPEN(fs, utf8);
            ICONV_OPEN(utf8, fs);
#endif
        }
    }
}

void set_log_charset(const char* charset)
{
    if ((charset == NULL && log_charset == NULL) ||
        (charset != NULL && log_charset != NULL && strcasecmp(charset, log_charset) == 0)) {
        return;
    } else {
        /* clearing the conversion to/from this charset */
        ICONV_CLOSE(main, log);
        ICONV_CLOSE(log, main);

        if (log_charset != NULL) {
            free(log_charset);
            log_charset = NULL;
        }

        if (charset != NULL) {
            if (*charset == '\0') {
                log_charset = xstrdup(nl_langinfo(CODESET));
            } else {
                log_charset = xstrdup(charset);
            }

            ICONV_OPEN(main, log);
            ICONV_OPEN(log, main);
        }
    }
}

