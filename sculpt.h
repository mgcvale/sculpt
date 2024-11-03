#ifndef SCULPT_H
#define SCLUPT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define SC_DEFAULT_BACKLOG 128
#define SC_DEFAULT_EPOLL_MAXEVENTS 12
#define HOST_BUF_LEN NI_MAXHOST
#define SERV_BUF_LEN NI_MAXSERV
#define SC_DEFAULT_CONN_TIMEOUT 60
#define SC_DEFAULT_CONN_MAX_AGE 300
#define SC_ENDPOINT_LEN 256
// utils

/* Describes a string with len attribute. By nature, this string has a constant value.*/
typedef struct {
    char *buf;
    size_t len;
} sc_str;

/* Creates a sc_str from char buffer. Assumes null-terminated string*/
sc_str sc_mk_str(const char *str);

/* Creates a sc_str from a char buffer with length. Does not assume null-terminated string*/
sc_str sc_mk_str_n(const char *str, size_t len);

/* strncmp for sc_str */
int sc_strcmp(const sc_str str1, const sc_str str2);
/* checks if the prefix is present in the string */
bool sc_strprefix(const sc_str str, const sc_str prefix);


/* describes a linked list of the set endpoints */

struct _endpoint_list {
    sc_str val;
    void (*func)(int);
    bool soft;
    struct _endpoint_list *next;
};

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int));

// headers

/* struct to hold headers of a request */
typedef struct _header_list {
    const char *header;
    struct _header_list *next;
} sc_headers;

sc_headers *sc_header_append(const char *header, sc_headers *list);
void sc_headers_free(sc_headers *headers);
sc_headers *parse_headers(const char *headers_str);

// actual framework
typedef struct _sc_addr_info {
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

typedef struct _sc_conn_mgr {
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
    bool listening;                 // flag to check listening status
} sc_conn_mgr;

sc_addr_info sc_addr_create(int sin_family, int port);
sc_conn_mgr *sc_mgr_create(sc_addr_info mgr, int *err);
int sc_mgr_listen(sc_conn_mgr *mgr);
void sc_mgr_set_backlog(sc_conn_mgr *mgr, int backlog);
void sc_mgr_set_epoll_maxevents(sc_conn_mgr *mgr, int maxevents);
void sc_mgr_finish(sc_conn_mgr *mgr);

int sc_mgr_pool_init(sc_conn_mgr *mgr, int max_conn);
sc_conn *sc_mgr_conn_get_free(sc_conn_mgr *mgr);

void sc_mgr_conn_release(sc_conn_mgr *mgr, sc_conn *conn);
void sc_mgr_conns_cleanup(sc_conn_mgr *mgr);

int sc_mgr_epoll_init(sc_conn_mgr *mgr);
int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms);
int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int));
int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int));

#endif // SCULPT_H
