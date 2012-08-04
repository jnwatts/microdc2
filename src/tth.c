#include <config.h>

#include <errno.h>  	/* for errstr */
#include <sys/types.h>	/* ? */
#include <sys/stat.h>	/* ? */
#include <unistd.h>		/* POSIX */
#include <fcntl.h>		/* ? */
#include <stdlib.h>		/* C89 */

#include <getopt.h>

#include <stdio.h>
#include <string.h>

#include "version-etc.h"	/* Gnulib */
#include "dirname.h"		/* Gnulib */

#include "tth/tth.h"

#define errstr (strerror(errno))

enum {
    VERSION_OPT = 256,
    HELP_OPT
};

static const char *short_opts = "v";
static struct option long_opts[] = {
    { "version", no_argument, NULL, VERSION_OPT },
    { "help", no_argument, NULL, HELP_OPT },
    { 0, }
};

const char version_etc_copyright[] =
    "Copyright (C) 2006 Vladimir Chugunov";

int main(int argc, char* argv[])
{
    int print_help = 0, i;

    int opt = -1, long_idx = -1;
    while (!print_help && (-1 != (opt = getopt_long(argc, argv, short_opts, long_opts, &long_idx)))) {
        switch (opt) {
            case VERSION_OPT:
	            version_etc(stdout, NULL, base_name(argv[0]), VERSION, "Vladimir Chugunov", NULL);
	            exit(EXIT_SUCCESS);
            case HELP_OPT:
                print_help = 1;
                break;
            default:
                fprintf(stderr, "unknown option value %d\n", opt);
                break;
        }
    }

    if (argc < 2 || print_help) {
        fprintf(stderr, "Usage: %s file [file...]\n\n", base_name(argv[0]));

        fprintf(stderr,
                "Calculate Tiger Tree Hash.\n\n"
                "Available options:\n"
                "        --version      - print version information\n"
                "        --help         - print this help\n\n");

        return 255;
    }

    for (i = optind; i < argc; i++) {
        char* filename = argv[i];
        char* tthl = NULL;
        size_t tthl_size;
        char* hash = tth(filename, &tthl, &tthl_size);
        if (tthl != NULL) {
            free(tthl);
        }
        if (tth != NULL) {
            printf("%40s %s\n", hash, filename);
            free(hash);
        } else {
            printf("Cannot process file %s - %s\n", filename, errstr);
        }
        fflush(stdout);
    }

    return 0;
}
