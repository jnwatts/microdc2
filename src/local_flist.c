#include <config.h>

#include <assert.h>		/* ? */
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/signal.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <inttypes.h>		/* ? */

#include "gettext.h"            /* Gnulib/GNU gettext */
#define _(s) gettext(s)
#define N_(s) gettext_noop(s)
#include "xvasprintf.h"		/* Gnulib */
#include "xalloc.h"		/* Gnulib */
#include "xstrndup.h"		/* Gnulib */
#include "human.h"		/* Gnulib */
#include "minmax.h"		/* Gnulib */
#include "dirname.h"		/* Gnulib */

#include "common/msgq.h"
#include "common/byteq.h"

#include "microdc.h"

//#define _TRACE
#if defined(_TRACE)
#define TRACE(x)    printf x; fflush(stdout);
#else
#define TRACE(x)
#endif

/* hash.c */
extern MsgQ *hash_request_mq;
extern MsgQ *hash_result_mq;
extern pid_t hash_child;
bool hash_init(void);
void hash_finish(void);

typedef enum {
    FILELIST_UPDATE_COMPLETE = 0,       /* RESPONSE ONLY      complete filelist - we have to replace the previous one (if any) with a new one */
    FILELIST_UPDATE_ADD_DIR_NAME,       /* REQUEST/RESPONSE   insert the new shared directory to the directory list */
    FILELIST_UPDATE_DEL_DIR_NAME,       /* REQUEST/RESPONSE   remove the shared directory from the directory list */
    FILELIST_UPDATE_STATUS,             /* RESPONSE ONLY      report the status to the main application */
    FILELIST_UPDATE_ERROR,              /* RESPONSE ONLY      report the error to the main application */
    FILELIST_UPDATE_LISTING_DIR,        /* REQUEST ONLY       main application informs about listing_dir change */
    FILELIST_UPDATE_MAIN_CHARSET,       /* REQUEST ONLY       main application informs about main_charset change */
    FILELIST_UPDATE_HUB_CHARSET,        /* REQUEST ONLY       main application informs about hub_charset change */
    FILELIST_UPDATE_FS_CHARSET,         /* REQUEST ONLY       main application informs about fs_charset change */
    FILELIST_UPDATE_REFRESH_INTERVAL,   /* REQUEST ONLY       main application informs about filelist_refresh_timeout change */

    FILELIST_UPDATE_INSERT,	            /* RESPONSE ONLY NOT USED     add new entries to the existing filelist */
    FILELIST_UPDATE_DELETE,             /* RESPONSE ONLY NOT USED     delete the entries from the existing filelist */
} UpdateType;


time_t    filelist_refresh_timeout = 600;
time_t    filelist_hash_refresh_timeout = 600;

MsgQ *update_request_mq = NULL;
MsgQ *update_result_mq = NULL;
pid_t update_child;
int   incoming_update_type = -1;
char* update_status = NULL;

static const char* filelist_name = "filelist";
static const char* new_filelist_name = "new-filelist";
static const char* filelist_prefix = "new-";

static const uint32_t    filelist_signature = ('M') | ('D' << 8) | ('C' << 16) | ('2' << 24);
static const uint32_t    filelist_min_supported_version   = 1;
static const uint32_t    filelist_max_supported_version   = 1;

#define ENOTFILELIST    (1 << 16)
#define EWRONGVERSION   (ENOTFILELIST + 1)

int compare_pointers(void* p1, void* p2)
{
    return p1 != p2;
}

bool is_already_shared_inode(DCFileList* root, dev_t dev, ino_t ino)
{
    HMapIterator it;
    bool result = false;

    if (root->dir.real_path != NULL) {
        struct stat st;
        if (stat(root->dir.real_path, &st) < 0) {
            /* fprintf(stderr, "cannot stat %s\n", root->dir.real_path); */
        } else if (dev == st.st_dev && ino == st.st_ino) {
            /* this directory is already in the our filelist */
            return true;
        }
    }

    hmap_iterator(root->dir.children, &it);
    while (it.has_next(&it) && !result) {
        DCFileList *node = it.next(&it);
        if (node->type == DC_TYPE_DIR) {
            result = is_already_shared_inode(node, dev, ino);
        }
    }
    return result;
}

bool is_already_shared(DCFileList* root, const char* dir)
{
    struct stat st;
    if (stat(dir, &st) < 0) {
        /* fprintf(stderr, "cannot stat %s\n", root->dir.real_path); */
        return false;
    }

    return is_already_shared_inode(root, st.st_dev, st.st_ino);
}

DCFileList* read_local_file_list(const char* path)
{
    struct stat st;
    DCFileList *root = NULL;

    if (stat(path, &st) < 0) {
        if (errno != ENOENT) {
            TRACE(("cannot stat %s: %d, %s\n", path, errno, errstr));
            return NULL;
        }
    } else if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
        return NULL;
    }

    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        void* mapped = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        unsigned char* data = mapped;

        if (*((uint32_t*)data) != filelist_signature) {
            errno = ENOTFILELIST;
        } else {
            data += sizeof(uint32_t);
            if (*((uint32_t*)data) < filelist_min_supported_version ||
                *((uint32_t*)data) > filelist_max_supported_version) {
                errno = EWRONGVERSION;
            } else {
                data += sizeof(uint32_t);
                data_to_filelist(data, &root);
            }
        }

        munmap(mapped, st.st_size);
        close(fd);
    } else {
        // file not found - just create empty root
        root = new_file_node("", DC_TYPE_DIR, NULL);
    }

    return root;
}

bool write_local_file_list(const char* path, DCFileList* root)
{
    bool result = false;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    if (fd >= 0) {
        unsigned char* data = NULL;
        size_t         data_size = 0;
        size_t offset = 0;
        size_t size = sizeof(filelist_signature);

        if (write(fd, &filelist_signature, sizeof(filelist_signature)) != sizeof(filelist_signature) ||
            write(fd, &filelist_max_supported_version, sizeof(filelist_max_supported_version)) != sizeof(filelist_max_supported_version))
            goto cleanup;

        filelist_to_data(root, (void**)&data, &data_size);

        offset = 0;
        size = data_size;

        /*
        do {
            size_t written = write(fd, data + offset, size);
            if (written >= 0) {
                offset += written;
                size -= written;
            }
        } while ((size < data_size) && (errno == 0 || errno == EAGAIN || errno == EINTR));
        */
        size = write(fd, data + offset, size);
        free(data);
        result = (size == data_size);

cleanup:
        close(fd);
    }
    return result;
}

/* translate file name from filesystem charset to main charset */
static void
fs_to_main_filelist(DCFileList* node)
{
    HMapIterator it;
    hmap_iterator(node->dir.children, &it);
    while (it.has_next(&it)) {
        DCFileList *child = it.next(&it);
        char* main_name = fs_to_main_string(child->name);
        rename_node(child, main_name);
        free(main_name);
        if (child->type == DC_TYPE_DIR) {
            fs_to_main_filelist(child);
        }
    }
}

static bool
lookup_filelist_changes(DCFileList* node, PtrV* hash_files)
{
    struct stat st;
    bool result = false; /* initially no chages detected */
    HMapIterator it;
    if (node->type == DC_TYPE_DIR) {
        if (node->dir.real_path != NULL) {
            PtrV* deleted = NULL;
            int i;

            struct dirent *ep = NULL;
            DIR *dp = NULL;

            hmap_iterator(node->dir.children, &it);
            while (it.has_next(&it)) {
                DCFileList *child = it.next(&it);
                char* fullname = catfiles(node->dir.real_path, child->name);
                if (stat(fullname, &st) < 0) {
                    if (errno == ENOENT) {
                        /* 
                            file was removed
                            we have to delete from the file list
                        */
                        if (deleted == NULL) {
                            deleted = ptrv_new();
                        }

                        ptrv_append(deleted, child->name);

                    }
                }
                free(fullname);
            }
            if (deleted != NULL) {
                for (i = 0; i < deleted->cur; i++) {
                    DCFileList* child = hmap_remove(node->dir.children, (const char*)deleted->buf[i]);
                    node->size -= child->size;

                    /*
                    TRACE((stderr, "removing 0x%08X (%s)\n", child, child == NULL ? "null" : child->name));
                    */

                    filelist_free(child);
                    result = true;
                }
                ptrv_free(deleted);
                deleted = NULL;
            }

            /* now we are looking for new items */
            dp = opendir(node->dir.real_path);
            if (dp != NULL) {
                while ((ep = xreaddir(dp)) != NULL) {
                    char* fullname;
                    DCFileList* child;

                    if (IS_SPECIAL_DIR(ep->d_name))
	                    continue;

    	            fullname = catfiles(node->dir.real_path, ep->d_name);
    	            if (stat(fullname, &st) < 0) {
                        /*
	                    fprintf(stderr, "%s: Cannot get file status - %s\n", fullname, errstr);
                        */
	                    free(fullname);
                        continue;
                    }

                    child = hmap_get(node->dir.children, ep->d_name);

                    if (child != NULL) {
                        if (child->type == DC_TYPE_REG) {
                            if (st.st_mtime != child->reg.mtime || child->size != st.st_size) {
                                child->reg.has_tth = false;
                                child->reg.mtime = st.st_mtime;
                                child->size = st.st_size;
                                if (ptrv_find(hash_files, child, (comparison_fn_t)compare_pointers) < 0) {
                                    ptrv_append(hash_files, child);
                                    result = true;
                                }
                            } else if (child->reg.has_tth == 0) {
                                if (ptrv_find(hash_files, child, (comparison_fn_t)compare_pointers) < 0) {
                                    ptrv_append(hash_files, child);
                                }
                            }
                        }
                    } else {
                        result = true;
	                    if (S_ISDIR(st.st_mode)) {
	                        child = new_file_node(ep->d_name, DC_TYPE_DIR, node);
                            child->dir.real_path = fullname;
                            fullname = NULL;
                        } else if (S_ISREG(st.st_mode)) {
	                        child = new_file_node(ep->d_name, DC_TYPE_REG, node);

	                        child->size = st.st_size;
                            child->reg.has_tth = 0;
                            memset(child->reg.tth, 0, sizeof(child->reg.tth));
                            child->reg.mtime = st.st_mtime;

                            if (ptrv_find(hash_files, child, (comparison_fn_t)compare_pointers) < 0) {
                                ptrv_append(hash_files, child);
                                /*
                                TRACE(stderr, "adding %s for hashing\n", fullname);
                                */
                            } else {
                                assert(false);
                            }

                        }
                    }
                    if (fullname != NULL)
                        free(fullname);

                }
                closedir(dp);
            }
        }

        node->size = 0;
        hmap_iterator(node->dir.children, &it);
        while (it.has_next(&it)) {
            DCFileList *child = it.next(&it);
            if (child->type == DC_TYPE_DIR) {
                // nanosleep here
                /*
                pause.tv_sec = 0;
                pause.tv_nsec = 1000000;
                nanosleep(&pause, &remain);
                */
                bool r = lookup_filelist_changes(child, hash_files);
                result = result || r;
            }
            node->size += child->size;
        }
    }

    /*
    TRACE((stderr, "%s returns %s\n", __FUNCTION__, result ? "true" : "false"));
    */

    return result;
}

bool report_status(MsgQ* status_mq, const char* fmt, ...)
{
    char* msg = NULL;
    va_list args;

    if (fmt != NULL) {
        va_start(args, fmt);
        msg = xvasprintf(fmt, args);
        va_end(args);
    }

    msgq_put(status_mq, MSGQ_INT, FILELIST_UPDATE_STATUS, MSGQ_END);
    msgq_put(status_mq, MSGQ_STR, msg, MSGQ_END);
    free(msg);
    if (msgq_write_all(status_mq) < 0) {
        /*
        fprintf(stderr, "status queue msgq_write_all error\n");
        fflush(stderr);
        */
        return false;
    }
    return true;
}

bool report_error(MsgQ* status_mq, const char* fmt, ...)
{
    char* msg = NULL;
    va_list args;

    va_start(args, fmt);
    msg = xvasprintf(fmt, args);
    va_end(args);

    msgq_put(status_mq, MSGQ_INT, FILELIST_UPDATE_ERROR, MSGQ_END);
    msgq_put(status_mq, MSGQ_STR, msg, MSGQ_END);
    free(msg);
    if (msgq_write_all(status_mq) < 0) {
        /*
        fprintf(stderr, "status queue msgq_write_all error\n");
        fflush(stderr);
        */
        return false;
    }
    return true;
}

bool send_filelist(MsgQ* status_mq, DCFileList* root)
{
    void *data;
    size_t size;

    write_filelist_file(root, filelist_prefix);

    msgq_put(status_mq, MSGQ_INT, FILELIST_UPDATE_COMPLETE, MSGQ_END);

    filelist_to_data(root, &data, &size);

    msgq_put(status_mq, MSGQ_BLOB, data, size, MSGQ_END);
    free(data);
    if (msgq_write_all(status_mq) < 0) {
        return false;
    }
    return true;
}

DCFileList* hash_request(PtrV* hash_files, MsgQ* request_mq, MsgQ* status_mq)
{
    DCFileList* hashing = NULL;
    if (hash_files->cur > 0) {
        char* filename;
        hashing = hash_files->buf[0];
        filename = catfiles(hashing->parent->dir.real_path, hashing->name);
        msgq_put(request_mq, MSGQ_STR, filename, MSGQ_END);

        //TRACE(("%s:%d: request hash for %s (%s)\n", __FUNCTION__, __LINE__, hashing->name, filename));
        if (msgq_write_all(request_mq) < 0) {
            /*
            fprintf(stderr, "hash queue msgq_write_all error\n");
            fflush(stderr);
            */
            hashing = NULL;
        }

        report_status(status_mq, "Calculating TTH for %s", filename);
        free(filename);
    }
    return hashing;
}

static void
__attribute__((noreturn))
local_filelist_update_main(int request_fd[2], int result_fd[2])
{
    PtrV *hash_files = NULL;
    DCFileList *hashing = NULL;
    time_t hash_start = 0;
    bool update_hash = false;
    bool initial = true;

    MsgQ *request_mq;
    MsgQ *result_mq;
    struct sigaction sigact;

    fd_set readable, writable;
    int max_fd = -1;
    struct timeval tv;

    DCFileList *root = NULL;
    /*HMapIterator it;*/
    char* flist_filename = NULL, *new_flist_filename = NULL;
    int  update_type = -1;

    close(request_fd[1]);
    close(result_fd[0]);
    request_mq = msgq_new(request_fd[0]);
    result_mq = msgq_new(result_fd[1]);

    hash_files = ptrv_new();

    if (!hash_init()) {
        goto cleanup;
    }

    /* Inability to register these signals is not a fatal error. */
    sigact.sa_flags = SA_RESTART;
    sigact.sa_handler = SIG_IGN;
#ifdef HAVE_STRUCT_SIGACTION_SA_RESTORER
    sigact.sa_restorer = NULL;
#endif
    sigaction(SIGINT, &sigact, NULL);
    sigaction(SIGTERM, &sigact, NULL);
    sigaction(SIGUSR1, &sigact, NULL);
    sigaction(SIGCHLD, &sigact, NULL);
    sigaction(SIGPIPE, &sigact, NULL);
    
    FD_ZERO(&readable);
    FD_ZERO(&writable);

    if (!get_package_file(filelist_name, &flist_filename) || !get_package_file(new_filelist_name, &new_flist_filename)) {
        goto cleanup;
    }

    if (NULL == (root = read_local_file_list(flist_filename))) {
        if (errno == ENOTFILELIST) {
            report_error(result_mq, "Cannot load FileList - %s: Invalid file format\n", flist_filename);
        } else if (errno == EWRONGVERSION) {
            report_error(result_mq, "Cannot load FileList - %s: Version isn't supported\n", flist_filename);
        } else {
            report_error(result_mq, "Cannot load FileList - %s: %s\n", flist_filename, errstr);
        }
        goto cleanup;
    }

    if (!send_filelist(result_mq, root)) {
        goto cleanup;
    }

    // now we start monitoring the shared directories
    update_type = -1;

    FD_SET(request_mq->fd, &readable);
    max_fd = request_mq->fd;
    FD_SET(hash_result_mq->fd, &readable);
    max_fd = MAX(hash_result_mq->fd, max_fd);

    while (true) {
        tv.tv_sec   = filelist_refresh_timeout;
        tv.tv_usec  = 0;

        fd_set r_ready = readable, w_ready = writable;
        int selected = 0;
        if (!initial) {
            selected = select(max_fd+1, &r_ready, &w_ready, NULL, &tv);
        } else {
            initial = false;
        }
        if (selected > 0) {
            if (FD_ISSET(hash_result_mq->fd, &r_ready)) {
                int res = msgq_read(hash_result_mq);
                if (res == 0 || (res < 0 && errno != EAGAIN)) {
                    /*
                    fprintf(stderr, "LOCAL_FLIST: hash msgq_read failed: %d, %s\n", errno, errstr);
                    fflush(stderr);
                    */
                    break;
                }
                while (msgq_has_complete_msg(hash_result_mq)) {
                    char* hash;
                    msgq_get(hash_result_mq, MSGQ_STR, &hash, MSGQ_END);
                    //TRACE(("%s:%d: hashing == 0x%08X, hash == 0x%08X\n", __FUNCTION__, __LINE__, hashing, hash));
                    if (hashing != NULL) {
                        DCFileList* h = ptrv_remove_first(hash_files);
                        assert(hashing == h);
                        if (hash != NULL) {
                            int len = MIN(sizeof(h->reg.tth), strlen(hash));
                            memcpy(h->reg.tth, hash, len);
                            h->reg.has_tth = 1;
                            update_hash = true;
                        }
                        hashing = NULL;
                    }
                    if (hash != NULL)
                        free(hash);
                    if (hash_files->cur > 0) {
                        /*
                        fprintf(stderr, "before hash_request\n");
                        fflush(stderr);
                        */
                        hashing = hash_request(hash_files, hash_request_mq, result_mq);
                        /*
                        fprintf(stderr, "after hash_request\n");
                        fflush(stderr);
                        fprintf(stderr, "request TTH for %08X (%s)\n", hashing, hashing != NULL ? hashing->name : "NULL");
                        fflush(stderr);
                        */
                    }
                    time_t now = time(NULL);
                    if (update_hash && ((hashing == NULL && hash_files->cur == 0) || (now - hash_start) > filelist_hash_refresh_timeout)) {
                        hash_start = now;
                        if (write_local_file_list(new_flist_filename, root)) {
                            rename(new_flist_filename, flist_filename);
                        } else {
                            unlink(new_filelist_name);
                        }

                        if (!send_filelist(result_mq, root)) {
                            break;
                        }
                        update_hash = false;
                    }
                    if (hashing == NULL && !initial) {
                        report_status(result_mq, NULL);
                    }
                }
            }
            if (FD_ISSET(request_mq->fd, &r_ready)) {
                int res = msgq_read(request_mq);
                if (res == 0 || (res < 0 && errno != EAGAIN)) {
                    break;
                }
                while (msgq_has_complete_msg(request_mq)) {
                    if (update_type < 0) {
                        /* read update type */
                        msgq_get(request_mq, MSGQ_INT, &update_type, MSGQ_END);
                    } else {
                        if (update_type == FILELIST_UPDATE_REFRESH_INTERVAL) {
                            time_t interval = 0;
                            msgq_get(request_mq, MSGQ_INT, &interval, MSGQ_END);
                            if (interval != 0) {
                                filelist_refresh_timeout = interval;
                            }
                        } else {
                            char *name;
                            int len = 0;
                        
                            msgq_get(request_mq, MSGQ_STR, &name, MSGQ_END);

                            len = strlen(name);
                            if (name[len-1] == '/')
                                name[len-1] = 0;

                            switch (update_type) {
                            case FILELIST_UPDATE_ADD_DIR_NAME:
                                if (is_already_shared(root, name)) {
                                    // report error here
                                    report_error(result_mq, "%s directory is already shared as subfolder of existing shared tree\n", name);
                                } else {
                                    char* bname = xstrdup(base_name(name));

                                    if (hmap_contains_key(root->dir.children, bname)) {
                                        /* we already have the shared directory with the same name */
                                        report_error(result_mq, "%s directory cannot be shared as %s because there is already shared directory with the same name\n", name, bname);
                                    } else {
                                        DCFileList* node = new_file_node(bname, DC_TYPE_DIR, root);
                                        node->dir.real_path = xstrdup(name);
                                        selected = 0;
                                    }
                                    free(bname);
                                }
                                break;
                            case FILELIST_UPDATE_DEL_DIR_NAME:
                                //selected = 0;
                                {
                                    char* bname = xstrdup(base_name(name));

                                    DCFileList* node = hmap_get(root->dir.children, bname);
                                    if (node != NULL && node->type == DC_TYPE_DIR) {
                                        if (strcmp(node->dir.real_path, name) == 0) {
                                            node = hmap_remove(root->dir.children, bname);
                                            filelist_free(node);
                                            if (write_local_file_list(new_flist_filename, root)) {
                                                rename(new_flist_filename, flist_filename);
                                            } else {
                                                unlink(new_filelist_name);
                                            }

                                            if (!send_filelist(result_mq, root)) {
                                                goto cleanup;
                                            }
                                        } else {
                                            report_error(result_mq, "%s directory is not shared\n");
                                        }
                                    }
                                    free(bname);
                                }
                                break;
                            case FILELIST_UPDATE_LISTING_DIR:
                                if (listing_dir != NULL) {
                                    free(listing_dir);
                                }
                                listing_dir = xstrdup(name);
                                if (!send_filelist(result_mq, root)) {
                                    goto cleanup;
                                }
                                break;
                            case FILELIST_UPDATE_HUB_CHARSET:
                                set_hub_charset(name);
                                if (!send_filelist(result_mq, root)) {
                                    goto cleanup;
                                }
                                break;
                            case FILELIST_UPDATE_FS_CHARSET:
                                set_fs_charset(name);
                                if (!send_filelist(result_mq, root)) {
                                    goto cleanup;
                                }
                                break;
                            default:
                                /*
                                fprintf(stderr, "unknown message type %d\n", update_type);
                                fflush(stderr);
                                */
                                goto cleanup;
                                break;
                            }
                            free(name);
                        }
                        update_type = -1;
                    }
                }
            }
        }
        if (selected == 0) {
            // just look through shared directories for new or deleted files
            if (hashing == NULL && !initial)
                report_status(result_mq, "Refreshing FileList");

            if (lookup_filelist_changes(root, hash_files)) {
                if (write_local_file_list(new_flist_filename, root)) {
                    rename(new_flist_filename, flist_filename);
                } else {
                    unlink(new_filelist_name);
                }

                if (!send_filelist(result_mq, root)) {
                    break;
                }
            }
            if (hashing == NULL && !initial)
                report_status(result_mq, NULL);
            if (hashing == NULL && hash_files->cur > 0) {
                hashing = hash_request(hash_files, hash_request_mq, result_mq);
                if (hashing != NULL) {
                    hash_start = time(NULL);
                }
            }
        } else if (selected < 0) {
            /*
            fprintf(stderr, "select error: %d, %s\n", errno, errstr);
            fflush(stderr);
            */
            if (errno != EINTR) {
                // error occurs
                break;
            }
        }
    }

    /* msgq_read_complete_msg may have failed if it returned < 0.
     * But we can't print any errors from this process (it would
     * interfere with the readline-managed display, so just exit
     * gracefully.
     */

cleanup:
    hash_finish();

    filelist_free(root);

    ptrv_free(hash_files);

    free(flist_filename);
    free(new_flist_filename);
    msgq_free(request_mq);
    msgq_free(result_mq);
    close(request_fd[0]);
    close(result_fd[1]);
    exit(EXIT_SUCCESS);
}

bool local_file_list_update_init(void)
{
    int request_fd[2];
    int result_fd[2];
    
    if (pipe(request_fd) != 0 || pipe(result_fd) != 0) {
        warn(_("Cannot create pipe pair - %s\n"), errstr);
        return false;
    }
    if (!fd_set_nonblock_flag(request_fd[1], true)
            || !fd_set_nonblock_flag(result_fd[0], true)) {
        warn(_("Cannot set non-blocking flag - %s\n"), errstr);
        return false;
    }

    update_child = fork();
    if (update_child < 0) {
        warn(_("Cannot create process - %s\n"), errstr);
        return false;
    }
    if (update_child == 0) {
        setpriority(PRIO_PROCESS, 0, 16);
        local_filelist_update_main(request_fd, result_fd);
        // we never reach this place
    }
    
    close(request_fd[0]);
    close(result_fd[1]);
    update_request_mq = msgq_new(request_fd[1]);
    update_result_mq = msgq_new(result_fd[0]);
    FD_SET(update_result_mq->fd, &read_fds);

    return true;
}

bool process_new_file_list(MsgQ* result_mq)
{
    void *data;
    size_t size;
    DCFileList *node;

    msgq_get(result_mq, MSGQ_BLOB, &data, &size, MSGQ_END);
    data_to_filelist(data, &node);
    free(data);

    fs_to_main_filelist(node);

    our_filelist_last_update = time(NULL);
    if (our_filelist != NULL) {
        filelist_free(our_filelist);
    }
    our_filelist = node;
    my_share_size = our_filelist->size;

    char sizebuf[LONGEST_HUMAN_READABLE+1];
	screen_putf(_("Sharing %" PRIu64 " %s (%s) totally\n"), my_share_size, ngettext("byte", "bytes", my_share_size),
	    human_readable(my_share_size, sizebuf, human_suppress_point_zero|human_autoscale|human_base_1024|human_SI|human_B, 1, 1));

#if 0
    struct timeval start, end;

    gettimeofday(&start, NULL);
#endif
    /*bool result = write_filelist_file(our_filelist);*/
    char *dc_flist_from    = xasprintf("%s%s%sMyList.DcLst", listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", filelist_prefix),
#if defined(HAVE_LIBXML2)
         *xml_flist_from   = xasprintf("%s%s%sfiles.xml",    listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", filelist_prefix),
         *bzxml_flist_from = xasprintf("%s%s%sfiles.xml.bz2",listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/", filelist_prefix),
         *xml_flist_to     = xasprintf("%s%sfiles.xml",      listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/"),
         *bzxml_flist_to   = xasprintf("%s%sfiles.xml.bz2",  listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/"),
#endif
         *dc_flist_to      = xasprintf("%s%sMyList.DcLst",   listing_dir, listing_dir[0] == '\0' || listing_dir[strlen(listing_dir)-1] == '/' ? "" : "/");

    rename(   dc_flist_from,    dc_flist_to);
    if (ptrv_find(delete_files, dc_flist_to, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(dc_flist_to));
    if (ptrv_find(delete_files, dc_flist_from, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(dc_flist_from));
    free(dc_flist_from);    free(dc_flist_to);
#if defined(HAVE_LIBXML2)
    rename(  xml_flist_from,   xml_flist_to);
    rename(bzxml_flist_from, bzxml_flist_to);
    if (ptrv_find(delete_files, xml_flist_to, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(xml_flist_to));
    if (ptrv_find(delete_files, xml_flist_from, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(xml_flist_from));
    if (ptrv_find(delete_files, bzxml_flist_to, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(bzxml_flist_to));
    if (ptrv_find(delete_files, bzxml_flist_from, (comparison_fn_t) strcmp) < 0)
    	ptrv_append(delete_files, xstrdup(bzxml_flist_from));
    free(xml_flist_from);   free(xml_flist_to);
    free(bzxml_flist_from); free(bzxml_flist_to);
#endif

#if 0
    gettimeofday(&end, NULL);

    long sec  = end.tv_usec < start.tv_usec ? end.tv_sec - start.tv_sec - 1 : end.tv_sec - start.tv_sec;
    long usec = end.tv_usec < start.tv_usec ? 1000000 + end.tv_sec - start.tv_sec : end.tv_sec - start.tv_sec;

    fprintf(stderr, "write_filelist_file() completes in %ld.%06ld\n", sec, usec);
#endif
    if (hub_state >= DC_HUB_LOGGED_IN && !send_my_info())
    	return false;

    return true;
}

bool local_file_list_init(void)
{
    // read initial file list
    int res = 0;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(update_result_mq->fd, &readable);

    screen_putf(_("Loading local FileList..."));
    int update_type = -1;
    bool exit = false;
    while (!exit) {
        fd_set ready = readable;
        res = select(update_result_mq->fd+1, &ready, NULL, NULL, NULL);
        if (res > 0) {
            res = msgq_read(update_result_mq);
            if (res <= 0) {
                if (errno != EAGAIN && errno != EINTR) {
                    exit = true;
                }
            } else {
                while (msgq_has_complete_msg(update_result_mq)) {
                    if (update_type < 0) {
                        msgq_get(update_result_mq, MSGQ_INT, &update_type, MSGQ_END);
                    } else {
                        exit = true;
                        break;
                    }
                }
            }
        } else {
            exit = true;
        }
    }

    if (res <= 0) {
        warn_socket_error(res, false, "update result pipe");
        return false;
    }
    if (update_type != FILELIST_UPDATE_COMPLETE) {
        screen_putf(_("error\n"));
        if (update_type == FILELIST_UPDATE_ERROR) {
            char* msg;
            msgq_get(update_result_mq, MSGQ_STR, &msg, MSGQ_END);
            screen_putf(_("%s\n"), msg);
            free(msg);
        } else {
            warn(_("unknown messge\n"));
        }
        return false;
    }

    screen_putf(_("done\n"));
    return process_new_file_list(update_result_mq);
}

bool
update_request_add_shared_dir(const char* dir)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_ADD_DIR_NAME, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_STR, dir, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

bool
update_request_del_shared_dir(const char* dir)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_DEL_DIR_NAME, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_STR, dir, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

bool
update_request_set_listing_dir(const char* dir)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_LISTING_DIR, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_STR, dir, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

bool
update_request_set_hub_charset(const char* charset)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_HUB_CHARSET, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_STR, charset, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

bool
update_request_set_fs_charset(const char* charset)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_FS_CHARSET, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_STR, charset, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

bool
update_request_set_filelist_refresh_timeout(time_t seconds)
{
    msgq_put(update_request_mq, MSGQ_INT, FILELIST_UPDATE_REFRESH_INTERVAL, MSGQ_END);
    msgq_put(update_request_mq, MSGQ_INT, seconds, MSGQ_END);
    if (msgq_write_all(update_request_mq) < 0)
        return false;
    return true;
}

void
update_request_fd_writable(void)
{
    int res;
    
    res = msgq_write(update_request_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, true, "update request pipe");
        running = false;
        return;
    }
    if (!msgq_has_partial_msg(update_request_mq))
        FD_CLR(update_request_mq->fd, &write_fds);
}

void
update_result_fd_readable(void)
{
    int res;

    res = msgq_read(update_result_mq);
    if (res == 0 || (res < 0 && errno != EAGAIN)) {
        warn_socket_error(res, false, "update result pipe");
        running = false;
        return;
    }
    while (msgq_has_complete_msg(update_result_mq)) {
        if (incoming_update_type < 0) {
            /* read update type */
            msgq_get(update_result_mq, MSGQ_INT, &incoming_update_type, MSGQ_END);
        } else {
            switch (incoming_update_type) {
            case FILELIST_UPDATE_COMPLETE:
                process_new_file_list(update_result_mq);
                break;
            case FILELIST_UPDATE_STATUS:
                if (update_status != NULL) {
                    free(update_status);
                }
                msgq_get(update_result_mq, MSGQ_STR, &update_status, MSGQ_END);
                break;
            case FILELIST_UPDATE_ERROR:
                {
                    char* err;
                    msgq_get(update_result_mq, MSGQ_STR, &err, MSGQ_END);
                    warn(_("filelist_update: %s\n"), err);
                    free(err);
                }
                break;
            default:
                assert(false);
                break;
            }
            incoming_update_type = -1;
        }
    }
}

void
local_file_list_update_finish(void)
{
    if (update_request_mq != NULL) {
        close(update_request_mq->fd);
        msgq_free(update_request_mq);
    }
    if (update_result_mq != NULL) {
        close(update_result_mq->fd);
        msgq_free(update_result_mq);
    }
    if (update_status != NULL) {
        free(update_status);
        update_status = NULL;
    }
}
