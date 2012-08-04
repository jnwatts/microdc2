#include <config.h>

#include <errno.h>  	/* for errstr */
#include <sys/types.h>	/* ? */
#include <sys/stat.h>	/* ? */
#include <unistd.h>		/* POSIX */
#include <fcntl.h>		/* ? */
#include <stdlib.h>		/* C89 */
#include <dirent.h>		/* ? */
#include <sys/time.h>

#include <getopt.h>

#include <stdio.h>
#include <string.h>
#include <libgen.h>

#include <xsize.h>

#include "version-etc.h"	/* Gnulib */
#include "xvasprintf.h"		/* Gnulib */
#include "dirname.h"		/* Gnulib */

#include "tth/tth.h"

#include "tth_file.h"

#define errstr (strerror(errno))

struct dirent *xreaddir(DIR *dh);
char *catfiles(const char *p1, const char *p2);
int process_directory(const char* directory);

enum {
    VERSION_OPT = 256,
    HELP_OPT
};

static const char *short_opts = "rf";
static struct option long_opts[] = {
    { "report", no_argument, NULL, 'r' },
    { "print-files", no_argument, NULL, 'f' },
    { "version", no_argument, NULL, VERSION_OPT },
    { "help", no_argument, NULL, HELP_OPT },
    { 0, }
};

const char version_etc_copyright[] =
    "Copyright (C) 2006 Vladimir Chugunov";

double   avg_speed          = 0;

off_t   directory_count     = 0;
off_t   directory_failed    = 0;
off_t   total_files         = 0;
off_t   existing_files      = 0;
off_t   new_files           = 0;
off_t   removed_files       = 0;
off_t   failed_files        = 0;

int report = 0;
int print_files = 0;

int main(int argc, char* argv[])
{
    char *directory = NULL;
    int i, count_failed = 0;
    struct timeval start_time, end_time;

    int print_help = 0;

    int opt = -1, long_idx = -1;
    while (!print_help && (-1 != (opt = getopt_long(argc, argv, short_opts, long_opts, &long_idx)))) {
        switch (opt) {
            case 'r':
                report = 1;
                break;
            case 'f':
                print_files = 1;
                break;
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
        fprintf(stderr, "Usage: %s [-r|--report] [-f|--print-files] directory [directory...]\n\n", base_name(argv[0]));

        fprintf(stderr,
                "Maintain TTH for microdc shared files.\n\n"
                "Available options:\n"
                "    -r, --report       - reports some statistic at the end of processing\n"
                "    -f, --print-files  - print file names during processing\n"
                "        --version      - print version information\n"
                "        --help         - print this help\n\n");

        return 255;
    }

    gettimeofday(&start_time, NULL);

    for (i = optind; i < argc; i++) {
        directory = argv[i];

        if (process_directory(directory) != 0) {
            count_failed++;
        }
    }

    gettimeofday(&end_time, NULL);

    if (report) {
        unsigned long elapsed_sec = end_time.tv_sec - start_time.tv_sec;
        unsigned long elapsed_usec = 0;
        if (end_time.tv_usec > start_time.tv_usec) {
            elapsed_usec = end_time.tv_usec - start_time.tv_usec;
        } else {
            elapsed_sec -= 1;
            elapsed_usec = 1000000 + end_time.tv_usec - start_time.tv_usec;
        }

        printf("%lld directories processed in %ld hours %02ld minutes %02ld.%03ld seconds:\n",
               directory_count, (elapsed_sec / 3600), ((elapsed_sec / 60) % 60), (elapsed_sec % 60), (elapsed_usec / 1000));
        printf("FILES:   total:%8lld, existing:%8lld, new:%8lld\n"
               "       removed:%8lld,   failed:%8lld\n",
               total_files, existing_files, new_files, removed_files, failed_files);
        printf("AVERAGE SPEED: %10.6f KB/sec\n", avg_speed);
    }

    if (count_failed == (argc-1)) {
        return 3;
    } else if (count_failed > 0) {
        return 4;
    }

    return 0;
}

int process_directory(const char* path)
{
    struct dirent *ep;
    DIR *dp = NULL, *tth_dp = NULL;
    char* tth_path = NULL;

    dp = opendir(path);
    if (dp == NULL) {
        if (print_files) {
    	    fprintf(stderr, "%s: Cannot open directory - %s\n", path, errstr);
        }
        directory_failed ++;
	    return errno;
    }
    
    directory_count ++;

    tth_path = catfiles(path, tth_directory_name);
    tth_dp = opendir(tth_path);
    if (tth_dp == NULL) {
        if (0 == mkdir(tth_path, S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)) {
            tth_dp = opendir(tth_path);
            if (tth_dp == NULL) {
                if (print_files) {
    	            fprintf(stderr, "%s: Cannot open directory - %s\n", tth_path, errstr);
                }
            }
        } else {
            if (print_files) {
    	        fprintf(stderr, "%s: Cannot create directory - %s\n", tth_path, errstr);
            }
        }
        if (tth_dp == NULL) {
            free(tth_path);
            closedir(dp);
	        return errno;
        }
    }

    while ((ep = xreaddir(dp)) != NULL) {
    	struct stat st;
	    char *fullname;

        if (IS_SPECIAL_DIR(ep->d_name))
	        continue;

	    /* If we ran into looped symlinked dirs, stat will stop (errno=ELOOP). */

    	fullname = catfiles(path, ep->d_name);
    	if (stat(fullname, &st) < 0) {
            if (print_files) {
	            fprintf(stderr, "%s: Cannot get file status - %s\n", fullname, errstr);
            }
	        free(fullname);
	        continue;
	    }

	    if (S_ISDIR(st.st_mode)) {
	        process_directory(fullname);
	    }
	    else if (S_ISREG(st.st_mode)) {
            struct stat tth_st;
            int create = 0, stat_result, tth_fd = -1;
            char *tth_fname = xasprintf("%s%s%s%s", tth_path, tth_path[0] == '\0' || tth_path[strlen(tth_path)-1] == '/' ? "" : "/", ep->d_name, ".tth");

            total_files ++;

            stat_result = stat(tth_fname, &tth_st);
            if (stat_result < 0 && errno == ENOENT) { // file not found
                create = 1;
            } else if (stat_result == 0) {
                tth_fd = open(tth_fname, O_RDONLY);
                if (tth_fd >= 0) {
                    uint64_t fsize;
                    time_t mtime, ctime;
                    char tth[39];
                    if (read(tth_fd, &fsize, sizeof(fsize)) != sizeof(fsize) || st.st_size != fsize || 
                        read(tth_fd, &mtime, sizeof(mtime)) != sizeof(mtime) || st.st_mtime != mtime || 
                        read(tth_fd, &ctime, sizeof(ctime)) != sizeof(ctime) || st.st_ctime != ctime ||
                        read(tth_fd, tth, sizeof(tth)) != sizeof(tth)) {
                        printf("%s: existing TTH is old or currupted\n", fullname);
                        create = 1;
                    }
                    close(tth_fd);
                    tth_fd = -1;
                }
            } else {
                // error occured - just continue
            }

            if (create != 0) {
                struct timeval file_start_time;
                struct timeval file_end_time;

                if (print_files) {
                    printf("%s...", fullname);
                    fflush(stdout);
                }

                gettimeofday(&file_start_time, NULL);

                unsigned char* p_tth = tth(fullname, 0, st.st_size);
                int failed = 0;

                tth_fd = open(tth_fname, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
                if (tth_fd >= 0) {
                    uint64_t fsize = st.st_size;
                    time_t mtime = st.st_mtime,
                           ctime = st.st_ctime;
                    if (write(tth_fd, &fsize, sizeof(fsize)) != sizeof(fsize) ||
                        write(tth_fd, &mtime, sizeof(mtime)) != sizeof(mtime) ||
                        write(tth_fd, &ctime, sizeof(ctime)) != sizeof(ctime) ||
                        write(tth_fd, p_tth, strlen(p_tth)) != strlen(p_tth)) {
                        failed = 1;
                    }
                    close(tth_fd);
                }
                if (failed != 0) {
                    unlink(tth_fname);
                    failed_files ++;
                } else {
                    new_files ++;
                }
                free(p_tth);

                gettimeofday(&file_end_time, NULL);

                double speed = ((st.st_size/1024)*1000000)/(double)((file_end_time.tv_sec*1000000+file_end_time.tv_usec)-(file_start_time.tv_sec*1000000+file_start_time.tv_usec));
                if (avg_speed == 0.) {
                    avg_speed = speed;
                } else {
                    avg_speed = (avg_speed + speed) / 2;
                }

                if (print_files) {
                    printf("done (spd=%10.4fKB/sec, avg=%10.4fKB/sec)\n", speed, avg_speed);
                    fflush(stdout);
                }
            } else {
                existing_files ++;
            }
            free(tth_fname);
        } else {
            if (print_files) {
	            fprintf(stderr, "%s: Not a regular file or directory, ignoring\n", fullname);
            }
	    }
	    free(fullname);
    }

    while ((ep = xreaddir(tth_dp)) != NULL) {
    	struct stat st;
	    char *fullname, *tth_name;
        int len = strlen(ep->d_name);
        if (len <= 4 || strcmp(ep->d_name+len-4, ".tth") != 0) {
            continue;
        }

	    /* If we ran into looped symlinked dirs, stat will stop (errno=ELOOP). */

    	tth_name = catfiles(tth_path, ep->d_name);
    	if (stat(tth_name, &st) < 0) {
            if (print_files) {
	            fprintf(stderr, "%s: Cannot get file status - %s\n", tth_name, errstr);
            }
	        free(tth_name);
	        continue;
	    }

    	fullname = catfiles(path, ep->d_name);

        //ptintf("%s: TTH %s\n", fullname, tth_name)
        len = strlen(fullname);
        if (S_ISREG(st.st_mode) ) {
            fullname[len-4] = '\0';
    	    if (stat(fullname, &st) < 0 && errno == ENOENT) {
                if (print_files) {
	                printf("%s: removed file. Removing TTH file %s\n", fullname, tth_name);
                    fflush(stdout);
                }
                unlink(tth_name);
                removed_files ++;
	        }
        }
        free(tth_name);
        free(fullname);
    }

    free(tth_path);
    closedir(dp);
    closedir(tth_dp);
    return 0;
}

char *
catfiles(const char *p1, const char *p2)
{
    return xasprintf("%s%s%s",
	    p1, p1[0] == '\0' || p1[strlen(p1)-1] == '/' ? "" : "/",
	    p2);
}

struct dirent *
xreaddir(DIR *dh)
{
    errno = 0;
    return readdir(dh);
}

