#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdbool.h>
#include <errno.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "cast.h"


const char *template = "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s";

cst_addr_info cst_addr_create(int sin_family, int port) {
    cst_addr_info addr_mgr;
    addr_mgr._sock_addr.sin_family = sin_family;
    addr_mgr._sock_addr.sin_port = htons(port);
    addr_mgr._sock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr_mgr.port = port;
    return addr_mgr;
}

cst_conn_mgr *cst_conn_create(cst_addr_info addr_mgr, int *err) {
    *err = CST_OK;
    cst_conn_mgr *mgr = malloc(sizeof(cst_conn_mgr));
    if (mgr == NULL) {
        perror("Error: memory allocation for cst_conn_mgr");
        *err = CST_MALLOC_ERR;
        return NULL;
    }

    mgr->addr_info = addr_mgr;
    mgr->backlog = CST_DEFAULT_BACKLOG;
    mgr->max_events = CST_DEFAULT_EPOLL_MAXEVENTS;
    mgr->listening = false;
    mgr->epoll_fd = -1;

    mgr->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mgr->fd < 0) {
        perror("Error: error creating socket for conn_mgr");
        free(mgr);
        *err = CST_SOCKET_CREATION_ERR;
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(mgr->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        perror("Error: failed to set socket options");
        *err = CST_SOCKET_SETOPT_ERR;
        goto error;
    }

    if (bind(mgr->fd, (struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info))) {
        perror("Error: Failed to bind server to the address");
        *err = CST_SOCKET_BIND_ERR;
        goto error;
    }

    return mgr;

    error:
    close(mgr->fd);
    free(mgr);
    return NULL;
}

void cst_conn_set_backlog(cst_conn_mgr *mgr, int backlog) {
    mgr->backlog = backlog;
}

void cst_conn_set_epoll_maxevents(cst_conn_mgr *mgr, int maxevents) {
    mgr->max_events = maxevents;
}

int cst_conn_listen(cst_conn_mgr *mgr) {
    if (listen(mgr->fd, mgr->backlog) < 0) {
        perror("Error: error in listen()");
        return CST_SOCKET_LISTEN_ERR;
    }

    int rc = getnameinfo((struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info),
                        mgr->host_buf, sizeof(mgr->host_buf),
                        mgr->service_buf, sizeof(mgr->service_buf), 0);
    if (rc != 0) {
        fprintf(stderr, "Warning: %s\n", gai_strerror(rc));
        fprintf(stderr, "Server is listening on unknown URL\n");
        return CST_SOCKET_GETNAMEINFO_ERR;
    }

    printf("\nServer is listening on http://%s%s:%d\n", mgr->host_buf, mgr->service_buf, mgr->addr_info.port);

    mgr->listening = true;
    return CST_OK;
}

void cst_conn_finish(cst_conn_mgr *mgr) {
    close(mgr->fd);
    if (mgr->epoll_fd != -1) {
        close(mgr->epoll_fd);
    }
    if (mgr->events != NULL) {
        free(mgr->events);
    }
    free(mgr);
}

int cst_conn_epoll_init(cst_conn_mgr *mgr) {
    mgr->epoll_fd = epoll_create1(0);
    if (mgr->epoll_fd == -1) {
        return CST_EPOLL_CREATION_ERR;
    }

    mgr->epoll_event.events = EPOLLIN;
    mgr->epoll_event.data.fd = mgr->fd;
    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->fd, &mgr->epoll_event) == -1) {
        return CST_EPOLL_CTL_ERR;
    }

    mgr->events = malloc(sizeof(struct epoll_event) * mgr->max_events);
    if (mgr->events == NULL) {
        return CST_MALLOC_ERR;
    }

    //TODO: set epoll nonblocking flags

    return CST_OK;
}

int cst_conn_poll(cst_conn_mgr *mgr) {
    // wait for new events from epoll
    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, -1); // -1 for indeterminate timeout
    if (n == -1) {
        return CST_EPOLL_WAIT_ERR;
    }
        
    for (int i = 0; i < n; i++) {
        if (mgr->events[i].data.fd == mgr->fd) {
            // This is a new connection, so handle 
    
            int a = T;

            // accept the new connection first
            socklen_t addr_len = sizeof(mgr->addr_info._sock_addr);
            int client_fd = accept(mgr->fd,
                    (struct sockaddr*)&mgr->addr_info._sock_addr, &addr_len);
            
            if (client_fd == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "Accept error: %d\n", errno);
                    return CST_ACCEPT_ERR;
                }
                continue;
            }

            // set the conenction as non-blocking
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags != -1) {
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            }

            // add the new connection to the epoll monitoring system so it is notified when new requests arrive
            struct epoll_event event;
            event.events = EPOLLIN;
            event.data.fd = client_fd;
            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, client_fd, &event); //TODO: check if this returns error
        } else { 
            // This is not a new connection as it doesn' have the same file descriptor as the server's
            // So we just pass the responsibility of it to the handler function (rn we will respond with hello world cuz thats yet to be implemented)
            
            int client_fd = mgr->events[i].data.fd;

            if (mgr->events[i].events & EPOLLIN) {
                // handle the meassage and the response
                char buf[1024];
                const char *hello = "Hello, World!\n";
                char msg[1024];
                snprintf(msg, sizeof(msg), template, strlen(hello), hello);
                
                // Read request
                size_t valread = read(client_fd, buf, sizeof(buf) - 1);
                if (valread > 0) {
                    buf[valread] = '\0';
                    printf("Request: %s\n", buf);
                    
                    // Send response
                    send(client_fd, msg, strlen(msg), 0);
                    printf("Sent response\n");
                }
            }

            // when they disconnect, close their fd
            if (mgr->events[i].events & (EPOLLERR | EPOLLHUP)) {
                close(client_fd);
            }
        }
    }

    return CST_OK;
}
