/* microdc.h - Header file for the project
 *
 * Copyright (C) 2004, 2005 Oskar Liljeblad
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifndef MICRODC_H
#define MICRODC_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <netinet/in.h>
#include <time.h>
#include <netdb.h>		/* struct addrinfo */
#include <fcntl.h>		/* POSIX: O_NONBLOCK */
#include <dirent.h>		/* POSIX: DIR, struct dirent */
#include <iconv.h>		/* Gnulib, GNU libc: iconv_t */
#include "common/byteq.h"
#include "common/ptrv.h"
#include "common/error.h"
#include "common/hmap.h"
#include "common/msgq.h"

#define DC_CLIENT_BASE_KEY 5
#define DC_HUB_TCP_PORT 411
#define DC_CLIENT_UDP_PORT 412
#define DC_USER_MAX_CONN 2

#define SEARCH_TIME_THRESHOLD 60        /* Add no more results to searches after this many seconds elapsed */

typedef enum {
    DC_CPL_DIR	= 1 << 0,	/* complete directories or symlinks to directories */
    DC_CPL_REG	= 1 << 1,	/* complete regular files or symlinks to regular files */
    DC_CPL_EXE	= 1 << 2,	/* complete regular files or symlinks to regular files that are executable according to access */
    DC_CPL_DOT	= 1 << 3,	/* always complete files and dirs starting with `.', even if file part of `word' does not start with `.' */
} DCFSCompletionFlags;

typedef enum {
    DC_DF_DEBUG			= 1 << 0, /* Various debug messages */
    DC_DF_JOIN_PART		= 1 << 1, /* Joins and Quits messages */
    DC_DF_PUBLIC_CHAT		= 1 << 2, /* Public chat */
    DC_DF_SEARCH_RESULTS	= 1 << 3, /* Incoming search results */
    DC_DF_UPLOAD		= 1 << 4, /* "Upload complete" message (not errors) */
    DC_DF_DOWNLOAD		= 1 << 5, /* "Download complete" message (not errors) */
    DC_DF_CONNECTIONS		= 1 << 6, /* User connections and normal disconnections */
    DC_DF_COMMON		= 1 << 7, /* Common messages always displayed (XXX: get rid of this?) */
} DCDisplayFlag;

typedef enum {
    DC_TF_NORMAL,		/* Normal file transfer */
    DC_TF_LIST,			/* Transfer of MyList.DcLst */
} DCTransferFlag;

typedef enum {
    DC_DIR_UNKNOWN,
    DC_DIR_SEND,
    DC_DIR_RECEIVE,
} DCTransferDirection;

typedef enum {
    DC_ACTIVE_UNKNOWN,	    	/* unknown, the default */
    DC_ACTIVE_KNOWN_ACTIVE, 	/* we got a ConnectToMe from user */
    DC_ACTIVE_RECEIVED_PASSIVE,	/* we got a RevConnectToMe from user */
    DC_ACTIVE_SENT_PASSIVE, 	/* we sent a RevConnectToMe to user, but haven't received anything yet */
    DC_ACTIVE_SENT_ACTIVE, 	/* we sent a ConnectToMe to user, but haven't received a connection yet */
} DCActiveState;

typedef enum {
    DC_USER_DISCONNECTED,
    DC_USER_CONNECT,	    	/* Waiting for connect() to complete or socket to be writable */
    DC_USER_MYNICK = 35,	/* Waiting for $MyNick */
    DC_USER_LOCK,  	    	/* Waiting for $Lock */
    DC_USER_DIRECTION,     	/* Waiting for $Direction */
    DC_USER_SUPPORTS,     	/* Waiting for $Supports */
    DC_USER_KEY,                /* Waiting for $Key */
    DC_USER_GET,   	    	/* Waiting for $Get (only when sending) */
    DC_USER_SEND_GET,  	    	/* Waiting for $Send or $Get (only when sending) */
    DC_USER_FILE_LENGTH,   	/* Waiting for $FileLength (only when receiving) */
    DC_USER_DATA_RECV,  	/* Waiting for file data (only when receiving) */
    DC_USER_DATA_SEND,
} DCUserState;

typedef enum {
    DC_HUB_DISCONNECTED,
    DC_HUB_LOOKUP,		/* Waiting for getaddrinfo lookup to complete */
    DC_HUB_CONNECT,	    	/* Waiting for connect() to complete or socket to be writable */
    DC_HUB_LOCK,	    	/* Waiting for $Lock */
    DC_HUB_HELLO,		/* Waiting for $Hello */
    DC_HUB_LOGGED_IN,		/* Correctly logged in */
} DCHubState;

/* The order of fields in DCFileType is important for sorting
 * completion entries as well.
 */
typedef enum {
    DC_TYPE_DIR,
    DC_TYPE_REG,
} DCFileType;

typedef enum {
    DC_LS_LONG_MODE = 1,
    DC_LS_TTH_MODE  = 2,
} DCLsMode;

typedef enum {
    DC_MSG_SCREEN_PUT,
    DC_MSG_WANT_DOWNLOAD,
    DC_MSG_VALIDATE_DIR,
    DC_MSG_VALIDATE_NICK,
    DC_MSG_GET_MY_NICK,
    DC_MSG_CHECK_DOWNLOAD,	/* get information on next download, allocate slot. */
    DC_MSG_CHECK_UPLOAD,	/* check that upload is allowed, allocate slot. */
    DC_MSG_UPLOAD_ENDED,	/* free slot and print info about upload. */
    DC_MSG_DOWNLOAD_ENDED,	/* free slot and mark download as done or failed. */
    DC_MSG_TRANSFER_START,
    DC_MSG_TRANSFER_STATUS,
    DC_MSG_TERMINATING,		/* the user process decided to terminate. */
} DCUserMsgId;

typedef enum {
    DC_SEARCH_ANY,
    DC_SEARCH_AUDIO,
    DC_SEARCH_COMPRESSED,
    DC_SEARCH_DOCUMENTS,
    DC_SEARCH_EXECUTABLES,
    DC_SEARCH_PICTURES,
    DC_SEARCH_VIDEO,
    DC_SEARCH_FOLDERS,
    DC_SEARCH_CHECKSUM,     /* DC++: TTH at the moment */
} DCSearchDataType;

typedef enum {
    DC_QS_QUEUED,	/* also meaning, "not touched" */
    DC_QS_PROCESSING,	/* currently being downloaded or about to be downloaded */
    DC_QS_DONE,		/* succesfully downloaded (XXX or already complete) */
    DC_QS_ERROR,	/* error occured during download */
} DCQueuedStatus;

typedef enum {
    DC_ADCGET_FILE,	/* Upload by filename */
    DC_ADCGET_TTH,	/* Upload by file root */
    DC_ADCGET_TTHL	/* Upload tth leaves */
} DCAdcgetType;

typedef struct _DCUserConn DCUserConn;
typedef struct _DCUserInfo DCUserInfo;
typedef struct _DCFileList DCFileList;
typedef struct _DCCompletionEntry DCCompletionEntry;
typedef struct _DCCompletionInfo DCCompletionInfo;
typedef struct _DCSearchSelection DCSearchSelection;
typedef struct _DCSearchString DCSearchString;
typedef struct _DCSearchRequest DCSearchRequest;
typedef struct _DCUDPMessage DCUDPMessage;
typedef struct _DCSearchResponse DCSearchResponse;
typedef struct _DCQueuedFile DCQueuedFile;
typedef struct _DCVariable DCVariable;
typedef struct _DCLookup DCLookup; /* defined in lookup.c */
typedef struct _DCFileListParse DCFileListParse; /* defined in filelist-in.c */

typedef void (*DCCompletorFunction)(DCCompletionInfo *ci);
typedef void (*DCBuiltinCommandHandler)(int argc, char **argv);
typedef void (*ScreenWriter)(DCDisplayFlag flag, const char *format, va_list args);
typedef void (*DCLookupCallback)(int rc, struct addrinfo *result_ai, void *data);
/* This callback is responsible for freeing node when no longer needed. */
typedef void (*DCFileListParseCallback)(DCFileList *node, void *data);

struct _DCSearchResponse {
    uint32_t refcount;
    DCUserInfo *userinfo;
    char *filename; // local namespace
    DCFileType filetype;
    uint64_t filesize;
    uint32_t slots_free;
    uint32_t slots_total;
    char *hub_name;
    struct sockaddr_in hub_addr;
};

struct _DCUDPMessage {
    struct sockaddr_in addr;
    uint32_t len;
    char data[0];
};

struct _DCSearchString {
    char *str;
    uint32_t len;
    uint16_t delta[256];
};

struct _DCSearchSelection {
    uint64_t size_min;
    uint64_t size_max;
    DCSearchDataType datatype;
    uint32_t patterncount;
    DCSearchString *patterns;
};

struct _DCSearchRequest {
    DCSearchSelection selection;
    time_t issue_time;
    PtrV *responses;
};

struct _DCCompletionInfo {
    const char *line;
    int ws;             /* may be modified */
    int we;             /* may not be modified. */
    char *word;         /* dequoted word */
    char *word_full;	/* word not touched */
    int word_index;
    PtrV *results;
};

struct _DCCompletionEntry {
    /* What to display for this completion result.
     * This should not be escaped or quoted, regardless
     * of the value of quoted.
     */
    char *display;
    /* display_fmt is a printf-like format which will
     * be passed display for first and only argument when
     * creating the final string to display. This field
     * is usually just "%s".
     */
    const char *display_fmt;
    /* What to place in the command line for this result.
     */
    char *input;
    /* input_fmt is a printf-like format which will be
     * passed input for the first and only argument when
     * creating the final input string.
     */
    const char *input_fmt;
    /* If input_single_fmt is non-NULL, it will be
     * used as the printf-like format if input_fmt is the
     * only match.
     */
    const char *input_single_fmt;
    /* If finalize is true, then the completion will end if this
     * this entry is the single match. It basicly means that
     * a space will be appended so that the caret will be moved
     * to a new word. If the word is quoted, a closing quote
     * will be added as well.
     */
    bool finalize;
    /* quoted should be true if input is already quoted.
     * if quoted is true and input starts with a double quote,
     * input must also end in a double quote regardless of
     * finalize.
     */
    bool quoted;
    /* These fields are used for sorting purposes.
     * This is currently only used by remote and local fs
     * completion generators.
     */
    union {
        DCFileType file_type;
    } sorting;
};

struct _DCFileList {
    DCFileList *parent;
    char *name;
    DCFileType type;
    uint64_t size;	/* total size of all contained files for DC_TYPE_DIR */
    union {
    	struct {
            char    has_tth;
	        char    tth[39];
            time_t  mtime;
	    } reg;
	    struct {
	        char *real_path;
	        HMap *children;
	    } dir;
    };
};

struct _DCQueuedFile {
    char *filename;  /* XXX: should make this relative, not absolute */
    char *base_path; /* XXX: so that catfiles(base_path, filename) works. */
    DCTransferFlag flag;
    DCQueuedStatus status;
    uint64_t length;
};

struct _DCUserInfo {
    char *nick;
    char *description;
    char *speed;
    uint8_t level;
    char *email;
    uint64_t share_size;
    DCActiveState active_state;
    PtrV *download_queue;
    uint16_t slot_granted;
    uint32_t refcount;
    bool info_quered;
    bool is_operator;

    DCUserConn *conn[DC_USER_MAX_CONN];	/* valid elements: conn[0..conn_count-1] */
    int conn_count; /* valid range: 0 <= conn_count <= DC_USER_MAX_CONN */
};

struct _DCUserConn {
    char *name;     	/* key of connection */
    bool disconnecting;
    DCUserInfo *info;	/* if info!=NULL then info->conn must contain this DCUserConn. */
    DCTransferDirection dir;
    pid_t pid;
    //int main_socket;
    //IPC *ipc;
    MsgQ *get_mq;
    MsgQ *put_mq;	/* Note: The main process may only put message on put_mq as a result of
                         * a message just received on get_mq.
                         */
    bool occupied_slot; /* true if used_*_slots were increased for this user connection */
    bool occupied_minislot; /* true if used_mini_slots were increased for this user connection */
    uint32_t queue_pos;
    bool queued_valid;		/* true unless the QueuedFile by queue_pos has been removed */
    char *transfer_file;
    char *local_file;
    bool transferring;
    uint64_t transfer_start;	/* the byte where the transfer started */
    uint64_t transfer_pos;	/* current byte position */
    uint64_t transfer_total;	/* total number of bytes to transfer */
    time_t transfer_time;
    /*bool we_connected;*/
};

struct _DCVariable {
    char *name;
    char *(*getter)(DCVariable *var);
    void (*setter)(DCVariable *var, int argc, char **argv); /* argc >= 1 */
    void *value;
    DCCompletorFunction completor;
    void *type_details;
    char *help_string;
};


extern ScreenWriter screen_writer;
extern char *log_filename;

extern DCHubState hub_state;
extern ByteQ *hub_recvq;
extern ByteQ *hub_sendq;
extern HMap *hub_users;
extern HMap *user_conns;
extern struct sockaddr_in local_addr;
extern struct in_addr force_listen_addr;
extern int hub_socket;
extern bool running;
extern HMap *pending_userinfo;
extern uint32_t display_flags;
extern uint32_t log_flags;

extern uint16_t listen_port;
extern char *my_tag;
extern char *my_nick;
extern char *my_description;
extern char *my_speed;
extern char *my_email;
extern uint64_t my_share_size;
extern char *download_dir;
extern char *listing_dir;
extern bool is_active;
extern bool auto_reconnect;
extern uint32_t my_ul_slots;
extern fd_set read_fds;
extern fd_set write_fds;
extern char *my_password;
extern PtrV *delete_files;  /* XXX: use LList? */
extern PtrV *delete_dirs;   /* XXX: use LList? */
extern uint32_t minislot_count;
extern uint32_t minislot_size;
extern int used_mini_slots;
extern int used_ul_slots;
extern int used_dl_slots;

extern DCFileList *browse_list; /* list of user we browse */
extern DCUserInfo *browse_user; /* user we browse OR WISH to browse, NULL if possibly browsing ourself */
extern bool browsing_myself;
extern char *browse_path;
extern char *browse_path_previous; /* recorded for `cd -' purposes */

extern pid_t shell_child;

/* hub.c */
void hub_input_available(void);
bool hub_putf(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
bool hub_connect_user(DCUserInfo *ui);
DCUserInfo *user_info_new(const char *nick);
DCUserInfo *find_hub_user(const char *nick);
void user_info_free(DCUserInfo *ui);
void hub_new(const char *hostname, uint16_t port);
void hub_connect(struct sockaddr_in *addr);
void hub_disconnect(void);
void hub_reconnect(void);
void hub_now_writable(void);
void check_hub_activity();
bool send_my_info(void);
extern struct sockaddr_in hub_addr;
extern char *hub_name;
void say_user_completion_generator(DCCompletionInfo *ci);
void user_completion_generator(DCCompletionInfo *ci);
void user_or_myself_completion_generator(DCCompletionInfo *ci);
void user_with_queue_completion_generator(DCCompletionInfo *ci);
void hub_reload_users();
void hub_set_connected(bool state);

/* command.c */
void command_execute(const char *line);
void default_completion_selector(DCCompletionInfo *ci);
void update_prompt(void);
void command_init(void);
void command_finish(void);
void browse_list_parsed(DCFileList *node, void *data);

/* screen.c */
#define screen_putf(f,...) flag_putf(DC_DF_COMMON, (f), ## __VA_ARGS__)
void flag_putf(DCDisplayFlag flag, const char *format, ...) __attribute__ ((format (printf, 2, 3)));
void screen_erase_and_new_line(void);
void screen_finish(void);
void screen_suspend(void);
void screen_wakeup(bool print_newline_first);
void screen_prepare(void);
void screen_redisplay_prompt();
void screen_read_input(void);
void screen_get_size(int *rows, int *cols);
void set_screen_prompt(const char *prompt, ...) __attribute__ ((format (printf, 1, 2)));
bool set_log_file(const char *new_filename, bool verbose);
void sorted_list_completion_generator(const char *base, PtrV *results,
    void *items, size_t item_count, size_t item_size, size_t key_offset); /* completion */
DCCompletionEntry *new_completion_entry(const char *input, const char *display); /* completion */
DCCompletionEntry *new_completion_entry_full(char *input, char *display, const char *input_fmt, const char *display_fmt, bool finalize, bool quoted);
void free_completion_entry(DCCompletionEntry *entry); /* completion */
void get_file_dir_part(const char *word,char **dir_part,const char **file_part); /* completion */
void fill_completion_info(DCCompletionInfo *ci);
char *quote_string(const char *str, bool dquotes, bool finalize);
char *filename_quote_string(const char *str, bool dquotes, bool finalize);
int completion_entry_display_compare(const void *e1, const void *e2);
int completion_entry_int_userdata_compare(const void *e1, const void *e2);

/* huffman.c */
char *huffman_decode(const uint8_t *data, uint32_t data_size, uint32_t *out_size);
char *huffman_encode(const uint8_t *data, uint32_t data_size, uint32_t *out_size);

/* user.c */
void user_main(int get_fd[2], int put_fd[2], struct sockaddr_in *addr, int sock);

/* main.c */
/*bool get_user_conn_status(DCUserConn *uc);*/
bool set_active(bool newactive, uint16_t newport);
bool add_share_dir(const char *dir);
bool del_share_dir(const char *dir);
bool has_user_conn(DCUserInfo *info, DCTransferDirection dir);
uint32_t get_user_conn_count(DCUserInfo *info);
bool get_package_file(const char *name, char **outname);
void transfer_completion_generator(DCCompletionInfo *ci);
void user_conn_cancel(DCUserConn *uc);
void warn_file_error(int res, bool write, const char *filename);
void warn_socket_error(int res, bool write, const char *subject, ...);
void add_search_result(struct sockaddr_in *addr, char *results, uint32_t resultlen);
void free_queued_file(DCQueuedFile *qf); /* XXX: move transfer.c? */
DCUserConn *user_connection_new(struct sockaddr_in *addr, int socket);
/*void user_disconnect(DCUserConn *uc);*/
char *user_conn_status_to_string(DCUserConn *uc, time_t now);
extern uint64_t bytes_received;
extern uint64_t bytes_sent;

/* fs.c */
DCFileList *new_file_node(const char *name, DCFileType type, DCFileList *parent);
void rename_node(DCFileList *node, const char* new_name);
void set_child_node(DCFileList *parent, DCFileList *child);
void filelist_free(DCFileList *fl);
void filelist_list(DCFileList *fl, int mode);
DCFileList *filelist_lookup(DCFileList *node, const char *filename);
char *filelist_get_path(DCFileList *node);
char *filelist_get_path_with_trailing_slash(DCFileList *node);
char *resolve_upload_file(DCUserInfo *ui, DCAdcgetType ul_type, const char *name, DCTransferFlag* flag, uint64_t* size);
char *resolve_download_file(DCUserInfo *ui, DCQueuedFile *queued);
bool filelist_create(const char *dir);
char *translate_local_to_remote(const char *localname);
char *translate_remote_to_local(const char *remotename);
void local_fs_completion_generator(DCCompletionInfo *ci, DCFSCompletionFlags flags);
void local_path_completion_generator(DCCompletionInfo *ci);
void local_dir_completion_generator(DCCompletionInfo *ci);
void remote_path_completion_generator(DCCompletionInfo *ci);
void remote_dir_completion_generator(DCCompletionInfo *ci);
char *apply_cwd(const char *path); /* XXX: move transfer.c? */
extern DCFileList *our_filelist;
extern time_t      our_filelist_last_update;
void filelist_list_recursively(DCFileList *node, char *basepath);
void remote_wildcard_expand(char *matchpath, bool *quotedptr, const char *basedir, DCFileList *basenode, PtrV *results);
bool has_leading_slash(const char *str);
void dir_to_filelist(DCFileList *parent, const char *path);
bool write_filelist_file(DCFileList* root, const char* prefix);
    
/* xml_flist.c */
int write_xml_filelist(int fd, DCFileList* root);
int write_bzxml_filelist(int fd, DCFileList* root);
DCFileList* filelist_xml_open(const char* filename);
DCFileList* filelist_bzxml_open(const char* filename);

/* connection.c */
char *decode_lock(const char *lock, size_t locklen, uint32_t basekey);
char *escape_message(const char *str);
char *unescape_message(const char *str);
void dump_command(const char *header, const char *buf, size_t len);

/* util.c */
int safe_rename(const char *oldpath, const char *newpath);
int ilog10(uint64_t c);
int mkdirs_for_file(char *filename);
char *catfiles(const char *p1, const char *p2);
char *catfiles_with_trailing_slash(const char *p1, const char *p2);
bool fd_set_status_flags(int fd, bool set, int modflags);
#define fd_set_nonblock_flag(f,s) fd_set_status_flags(f,s,O_NONBLOCK)
char *getenv_default(const char *name, char *defvalue);

#include "tth_file.h"

#define COMPARE_RETURN(a,b) { if ((a) < (b)) return -1; if ((a) > (b)) return 1; }
#define COMPARE_RETURN_FUNC(f) { int _c = (f); if (_c != 0) return _c; }
char *sockaddr_in_str(struct sockaddr_in *addr);
char *in_addr_str(struct in_addr addr);
bool parse_ip_and_port(char *source, struct sockaddr_in *addr, uint16_t defport);
#define xquotestr(s) 		(quotearg_alloc((s), SIZE_MAX, NULL))
#define quotearg_mem(m,l)	(quotearg_n_style_mem(0, escape_quoting_style, (m), (l)))
char *join_strings(char **strs, int count, char mid);
PtrV *wordwrap(const char *str, size_t len, size_t first_width, size_t other_width);
struct dirent *xreaddir(DIR *dh);
    
#define LONGEST_ELAPSED_TIME 22 /* 123456789012dNNhNNmNNs */
char *elapsed_time_to_string(time_t elapsed, char *buf);

/* search.c */
int parse_search_selection(char *str, DCSearchSelection *data);
bool perform_inbound_search(DCSearchSelection *data, DCUserInfo *ui, struct sockaddr_in *addr);
extern PtrV *our_searches;
bool add_search_request(char *args);
void handle_search_result(char *buf, uint32_t len);
void free_search_request(DCSearchRequest *sr);
char *search_selection_to_string(DCSearchSelection *sr);
void search_string_new(DCSearchString *sp, const char *p, int len);
void search_hash_new(DCSearchString *sp, const char *p, int len);
void search_string_free(DCSearchString *sp);

/* variables.c */
void cmd_set(int argc, char **argv);
void set_command_completion_selector(DCCompletionInfo *ci);

/* lookup.c */
extern MsgQ *lookup_request_mq;
extern MsgQ *lookup_result_mq;
extern pid_t lookup_child;
bool lookup_init(void);
void lookup_request_fd_writable(void);
void lookup_result_fd_readable(void);
DCLookup *add_lookup_request(const char *node, const char *service, const struct addrinfo *hints, DCLookupCallback callback, void *userdata);
void cancel_lookup_request(DCLookup *lookup);
void lookup_finish(void);

/* filelist-in.c */
extern MsgQ *parse_request_mq;
extern MsgQ *parse_result_mq;
extern pid_t parse_child;
bool file_list_parse_init(void);
void file_list_parse_finish(void);
DCFileListParse *add_parse_request(DCFileListParseCallback callback, const char *filename, void *userdata);
void cancel_parse_request(DCFileListParse *parse);
void parse_result_fd_readable(void);
void parse_request_fd_writable(void);
void* data_to_filelist(void *dataptr, DCFileList **outnode);
void  filelist_to_data(DCFileList *node, void **dataptr, size_t *sizeptr);

/* local_flist.c */
extern MsgQ *update_request_mq;
extern MsgQ *update_result_mq;
extern pid_t update_child;
extern char* update_status;
extern time_t filelist_refresh_timeout;
bool local_file_list_update_init(void);
bool local_file_list_init(void);
void local_file_list_update_finish(void);
bool update_request_add_shared_dir(const char* dir);
bool update_request_del_shared_dir(const char* dir);
bool update_request_set_listing_dir(const char* dir);
bool update_request_set_hub_charset(const char* charset);
bool update_request_set_fs_charset(const char* charset);
bool update_request_set_filelist_refresh_timeout(time_t seconds);
/*
DCFileListParse *add_parse_request(DCFileListParseCallback callback, const char *filename, void *userdata);
void cancel_parse_request(DCFileListParse *parse);
*/
void update_result_fd_readable(void);
void update_request_fd_writable(void);

/* charsets.c */
#include "charsets.h"
EXPORT_CHARSET(main);
EXPORT_CHARSET(hub);
EXPORT_CHARSET(fs);
EXPORT_CHARSET(log);
EXPORT_CONST_CHARSET(utf8);

void set_main_charset(const char* charset);
void set_hub_charset(const char* charset);
void set_fs_charset(const char* charset);
void set_log_charset(const char* charset);

EXPORT_ICONV_CONVERSION(main, hub);
EXPORT_ICONV_CONVERSION(hub, main);
EXPORT_ICONV_CONVERSION(main, fs);
EXPORT_ICONV_CONVERSION(fs, main);
EXPORT_ICONV_CONVERSION(hub, fs);
EXPORT_ICONV_CONVERSION(fs, hub);
EXPORT_ICONV_CONVERSION(main, log);
EXPORT_ICONV_CONVERSION(log, main);

#if defined(HAVE_LIBXML2)
EXPORT_ICONV_CONVERSION(utf8, fs);
EXPORT_ICONV_CONVERSION(fs, utf8);
EXPORT_ICONV_CONVERSION(utf8, main);
EXPORT_ICONV_CONVERSION(main, utf8);
EXPORT_ICONV_CONVERSION(utf8, hub);
EXPORT_ICONV_CONVERSION(hub, utf8);
#endif


#endif
