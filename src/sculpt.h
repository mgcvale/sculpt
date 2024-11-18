#ifndef SCULPT_H
#define SCULPT_H

#include <stdio.h>
#include <netdb.h>
#include <stdbool.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <fcntl.h>

#define SC_OK 0
#define SC_SOCKET_BIND_ERR -1
#define SC_SOCKET_LISTEN_ERR -2
#define SC_SOCKET_GETNAMEINFO_ERR -3
#define SC_MALLOC_ERR -4
#define SC_SOCKET_SETOPT_ERR -5
#define SC_SOCKET_CREATION_ERR -6
#define SC_EPOLL_CREATION_ERR -7
#define SC_EPOLL_CTL_ERR -8
#define SC_EPOLL_WAIT_ERR -9
#define SC_ACCEPT_ERR -10
#define SC_BAD_ARGUMENTS_ERR -11
#define SC_FCNTL_ERR -12
#define SC_SEND_ERR -13
#define SC_CONN_CLOSED -14
#define SC_READ_ERR -15
#define SC_FINISHED -16
#define SC_BUFFER_OVERFLOW_ERR -17
#define SC_MALFORMED_HEADER_ERR -18

#define SC_DEFAULT_BACKLOG 128
#define SC_DEFAULT_EPOLL_MAXEVENTS 12
#define HOST_BUF_LEN NI_MAXHOST
#define SERV_BUF_LEN NI_MAXSERV
#define SC_DEFAULT_CONN_TIMEOUT 60
#define SC_DEFAULT_CONN_MAX_AGE 300
#define SC_ENDPOINT_LEN 256
#define HEADER_BUF_SIZE 1024
#define METHOD_BUF_SIZE 16
#define URL_BUF_SIZE 128
#define SC_CONTINUE 1
#define SC_MAX_HEADER_ERROR_COUNT 12

#define SC_LL_NONE 0
#define SC_LL_MINIMAL 1
#define SC_LL_NORMAL 2
#define SC_LL_DEBUG 3

// utils

/* Describes a string with len attribute. The string can either be kept as a copy of the memory passed in the mk methods, or as a reference.*/
typedef struct {
    char *buf;
    size_t len;
} sc_str;

/* Creates a sc_str from char buffer, REFERENCING its contents. Assumes null-terminated string*/
sc_str sc_str_ref(const char *str);

/* Creates a sc_str from a char buffer with length, REFERENCING its contents. Does not assume null-terminated string*/
sc_str sc_str_ref_n(const char *str, size_t len);

/* Creates a sc_str from a char buffer, COPYING its contents. Assumes null-terminated string*/
/* Note: this method requires freeing the string after use. */
sc_str sc_str_copy(const char *str);

/* Creates a sc_str from a char buffer with length, COPYING its contents. Does not assume null-terminated string*/
/* Note: this method requires freeing the string after use. */
sc_str sc_str_copy_n(const char *str, size_t len);

/* Assumes string was created with sc_str_copy function. */
void sc_str_free(sc_str *str);

/* strncmp for sc_str */
int sc_strcmp(const sc_str str1, const sc_str str2);
/* checks if the prefix is present in the string */
bool sc_strprefix(const sc_str str, const sc_str prefix);

/* describes the basic necessary info about an http message for virtually any rquest */ 
typedef struct {
    sc_str uri;
    sc_str method;
} sc_http_msg;

/* struct to hold headers of a request */
typedef struct _header_list {
    sc_str header;
    struct _header_list *next;
} sc_headers;

sc_headers *sc_header_append(const char *header, sc_headers *list);
void sc_headers_free(sc_headers *headers);
void sc_header_free(sc_headers *headers);
sc_headers *parse_headers(const char *headers_str);


/* describes a linked list of the set endpoints */

// headers

// actual framework
typedef struct {
    struct sockaddr_in _sock_addr;
    int port;
} sc_addr_info;

typedef struct sc_conn {
    int fd;
    time_t last_active;         // when connection was last used
    time_t creation_time;     // when connection was created
    enum {
        CONN_IDLE,
        CONN_ACTIVE,
        CONN_CLOSING
    } state;
    struct sc_conn *next;
} sc_conn;

typedef struct {
    sc_addr_info addr_info;         
    int fd;                         // server file descriptor
    int backlog;                    // server backlog count
    char host_buf[HOST_BUF_LEN];    // hostname buffer
    char service_buf[SERV_BUF_LEN]; // service buffer

    // Connection pool management  
    sc_conn *conn_pool;             // main connection pool
    sc_conn *free_conns;            // free connection pool
    int max_conn_count;             // max connection count
    int conn_count;         // current connection count
    time_t conn_timeout;            // max connection idle time before closing
    time_t conn_max_age;            // max connection lifetime

    // epoll
    int epoll_fd;       // epoll file descriptor
    struct epoll_event *events;
    size_t max_events;              // max number of epoll events
    struct epoll_event epoll_event; // server epoll event

    // misc
    struct _endpoint_list *endpoints; //linked list of endpoints
    bool listening;     // flag to check listening status
    int ll;
} sc_conn_mgr;

sc_addr_info sc_addr_create(int sin_family, int port);
sc_conn_mgr *sc_mgr_create(sc_addr_info mgr, int *err);
int sc_mgr_listen(sc_conn_mgr *mgr);
int sc_mgr_epoll_init(sc_conn_mgr *mgr);
int sc_mgr_conn_pool_init(sc_conn_mgr *mgr, int max_conn);

void sc_mgr_backlog_set(sc_conn_mgr *mgr, int backlog);
void sc_mgr_epoll_maxevents_set(sc_conn_mgr *mgr, int maxevents);
void sc_mgr_ll_set(sc_conn_mgr *mgr, int ll);

void sc_mgr_finish(sc_conn_mgr *mgr);
void sc_mgr_conn_pool_destroy(sc_conn_mgr *mgr);

sc_conn *sc_mgr_conn_get_free(sc_conn_mgr *mgr);
void sc_mgr_conn_release(sc_conn_mgr *mgr, sc_conn *conn);
void sc_mgr_conns_cleanup(sc_conn_mgr *mgr);

int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms);

// sending and recieving data utils

int sc_easy_send(int fd, int code, const char *code_str, const char *content_type, const char *body, sc_headers *headers);
char *sc_easy_request_build(int code, const char *code_str, const char *body, sc_headers *headers);
int sc_easy_send2(int fd, int code, const char *code_str, const char *body, sc_headers *headers);

struct _endpoint_list {
    sc_str val;
    void (*func)(int, sc_http_msg, sc_headers*);
    bool soft;
    struct _endpoint_list *next;
};

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int, sc_http_msg, sc_headers*));
int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*));
int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*));





// logging

void sc_log(sc_conn_mgr *mgr, int ll, const char *format, ...);
void sc_error_log(sc_conn_mgr *mgr, int ll, const char *format, ...);
void sc_perror(sc_conn_mgr *mgr, int ll, const char *format);

#endif // SCULPT_H
