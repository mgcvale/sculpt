#ifndef CAST_H
#define CAST_H

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

#define CST_OK 0
#define CST_SOCKET_BIND_ERR -1
#define CST_SOCKET_LISTEN_ERR -2
#define CST_SOCKET_GETNAMEINFO_ERR -3
#define CST_MALLOC_ERR -4
#define CST_SOCKET_SETOPT_ERR -5
#define CST_SOCKET_CREATION_ERR -6
#define CST_EPOLL_CREATION_ERR -7
#define CST_EPOLL_CTL_ERR -8
#define CST_EPOLL_WAIT_ERR -9
#define CST_ACCEPT_ERR -10
#define CST_DEFAULT_BACKLOG 128
#define CST_DEFAULT_EPOLL_MAXEVENTS 12
#define HOST_BUF_LEN NI_MAXHOST
#define SERV_BUF_LEN NI_MAXSERV
#define T 2

// utils

/* Describes a string with len attribute */
typedef struct {
    char *buf;
    size_t len;
} cst_str;

/* Creates a cst_str from char buffer. Assumes null-terminated string*/
cst_str cst_mk_str(const char *str);

/* Creates a cst_str from a char buffer with length. Does not assume null-terminated string*/
cst_str cst_mk_str_n(const char *str, size_t len);

/* strncmp for cst_str */
int cst_strcmp(const cst_str str1, const cst_str str2);
/* checks if the prefix is present in the string */
bool cst_strprefix(const cst_str str, const cst_str prefix);


// headers

/* struct to hold headers of a request */
typedef struct _header_list {
    const char *header;
    struct _header_list *next;
} cst_headers;

cst_headers *cst_header_append(const char *header, cst_headers *list);
void cst_headers_free(cst_headers *headers);

// actual framework

typedef struct _cst_addr_info {
    struct sockaddr_in _sock_addr;
    int port;
} cst_addr_info;

typedef struct _cst_conn_mgr {
    cst_addr_info addr_info;
    int fd;
    int backlog;
    char host_buf[HOST_BUF_LEN];
    char service_buf[SERV_BUF_LEN];
    int epoll_fd;
    size_t max_events;
    struct epoll_event epoll_event;
    struct epoll_event *events;
    bool listening;
} cst_conn_mgr;

cst_addr_info cst_addr_create(int sin_family, int port);
cst_conn_mgr *cst_conn_create(cst_addr_info mgr, int *err);
int cst_conn_listen(cst_conn_mgr *mgr);
void cst_conn_set_backlog(cst_conn_mgr *mgr, int backlog);
void cst_conn_set_epoll_maxevents(cst_conn_mgr *mgr, int maxevents);
void cst_conn_finish(cst_conn_mgr *mgr);

int cst_conn_epoll_init(cst_conn_mgr *mgr);
int cst_conn_poll(cst_conn_mgr *mgr);

#endif // CAST_H
