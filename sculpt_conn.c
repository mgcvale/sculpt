#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdbool.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "sculpt.h"

#define RETURN_ERROR_IF(condition, error_code, message) \
    do { \
        if (condition) { \
            perror(message); \
            return error_code; \
        } \
    } while (0)


sc_addr_info sc_addr_create(int sin_family, int port) {
    sc_addr_info addr_mgr;
    addr_mgr._sock_addr.sin_family = sin_family;
    addr_mgr._sock_addr.sin_port = htons(port);
    addr_mgr._sock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr_mgr.port = port;
    return addr_mgr;
}

sc_conn_mgr *sc_mgr_create(sc_addr_info addr_mgr, int *err) {
    *err = SC_OK;
    sc_conn_mgr *mgr = malloc(sizeof(sc_conn_mgr));
    if (mgr == NULL) {
        perror("Error: memory allocation for sc_conn_mgr");
        *err = SC_MALLOC_ERR;
        return NULL;
    }

    mgr->addr_info = addr_mgr;
    mgr->backlog = SC_DEFAULT_BACKLOG;
    mgr->max_events = SC_DEFAULT_EPOLL_MAXEVENTS;
    mgr->listening = false;
    mgr->epoll_fd = -1;
    mgr->endpoints = NULL;

    mgr->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mgr->fd < 0) {
        perror("Error: error creating socket for conn_mgr");
        free(mgr);
        *err = SC_SOCKET_CREATION_ERR;
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(mgr->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        perror("Error: failed to set socket options");
        *err = SC_SOCKET_SETOPT_ERR;
        goto error;
    }

    if (bind(mgr->fd, (struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info))) {
        perror("Error: Failed to bind server to the address");
        *err = SC_SOCKET_BIND_ERR;
        goto error;
    }

    return mgr;

    error:
    close(mgr->fd);
    free(mgr);
    return NULL;
}

void sc_mgr_set_backlog(sc_conn_mgr *mgr, int backlog) {
    mgr->backlog = backlog;
}

void sc_mgr_set_epoll_maxevents(sc_conn_mgr *mgr, int maxevents) {
    mgr->max_events = maxevents;
}

int sc_mgr_listen(sc_conn_mgr *mgr) {
    if (listen(mgr->fd, mgr->backlog) < 0) {
        perror("Error: error in listen()");
        return SC_SOCKET_LISTEN_ERR;
    }

    int rc = getnameinfo((struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info),
                        mgr->host_buf, sizeof(mgr->host_buf),
                        mgr->service_buf, sizeof(mgr->service_buf), 0);
    if (rc != 0) {
        fprintf(stderr, "Warning: %s\n", gai_strerror(rc));
        fprintf(stderr, "Server is listening on unknown URL\n");
        return SC_SOCKET_GETNAMEINFO_ERR;
    }

    printf("\nServer is listening on http://%s%s:%d\n", mgr->host_buf, mgr->service_buf, mgr->addr_info.port);

    mgr->listening = true;
    return SC_OK;
}

void sc_mgr_finish(sc_conn_mgr *mgr) {
    close(mgr->fd);
    if (mgr->epoll_fd != -1) {
        close(mgr->epoll_fd);
    }
    if (mgr->events != NULL) {
        free(mgr->events);
    }
    free(mgr);
}

int sc_mgr_epoll_init(sc_conn_mgr *mgr) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "NULL manager provided");

    // using EPOLL_CLOEXEC to prevent fd leaks across exec()
    mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    RETURN_ERROR_IF(mgr->epoll_fd == -1, SC_EPOLL_CREATION_ERR, "epoll_create1 failed");

    int flags = fcntl(mgr->fd, F_GETFL);
    RETURN_ERROR_IF(flags == -1, SC_FCNTL_ERR, "Failed to get socket flags");
    
    RETURN_ERROR_IF(fcntl(mgr->fd, F_SETFL, flags | O_NONBLOCK) == -1,
                   SC_FCNTL_ERR, "Failed to set non-blocking mode");

    mgr->epoll_event.events = EPOLLIN | EPOLLRDHUP; // no edge triggered mode
    mgr->epoll_event.data.fd = mgr->fd;
    RETURN_ERROR_IF(epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->fd, &mgr->epoll_event) == -1,
                   SC_EPOLL_CTL_ERR, "epoll_ctl failed");

    mgr->events = calloc(mgr->max_events, sizeof(struct epoll_event)); 
    RETURN_ERROR_IF(!mgr->events, SC_MALLOC_ERR, "Failed to allocate events array");return SC_OK;
}

int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "The mgr pointer cant be null");
    sc_mgr_conns_cleanup(mgr);
    
    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, timeout_ms);
    if (n == -1) {
        if (errno == EINTR) return SC_OK;
        return SC_EPOLL_WAIT_ERR;
    }

    for (int i = 0; i < n; i++) {

    
       // handle errors with the epoll event
        if (mgr->events[i].events & EPOLLERR) {
            sc_conn *conn = mgr->events[i].data.ptr;
            perror("Error with epoll, closing connection...");
            if (conn) { 
                sc_mgr_conn_release(mgr, conn);
                close(conn->fd);
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);

            }
            continue;
        }


        if (mgr->events[i].data.fd == mgr->fd) {
            fprintf(stdout, "Creating new connection\n");
            socklen_t addr_len = sizeof(mgr->addr_info._sock_addr);

            // new connection, check capacity before proceeding
            if (mgr->conn_count >= mgr->max_conn_count) {
                perror("No avaliable connections found! Sending 503 response");
                int client_fd = accept(mgr->fd, (struct sockaddr*)&mgr->addr_info._sock_addr, &addr_len);
                if (client_fd != -1) {
                     static const char *msg = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 21\r\n\r\nServer at capacity\r\n";
                    send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);
                    close(client_fd);
                }
                continue;
            }

            // try to find an unused connection
            sc_conn *conn = sc_mgr_conn_get_free(mgr);
            if (!conn) {
                perror("Failed to find free connection on sc_mgr_conn_get_free()\n");
                continue;
            }

            // valid connection was found, so we accept the request
             conn->fd = accept(mgr->fd, (struct sockaddr*)&mgr->addr_info._sock_addr, &addr_len);

            if (conn->fd == -1) {
                perror("Error on Accept");
                sc_mgr_conn_release(mgr, conn);
                if (errno != EAGAIN && errno != EWOULDBLOCK) { // if the error is not because it would block or cuz it is unacailable, we don't return the function
                    fprintf(stderr, "Fatal: Accept error: %d\n", errno);
                    return SC_ACCEPT_ERR;
                }
                continue;
            }

            // set connection as non-blocking because we used accept instead of accept4
            if (fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK) == -1) {
                perror("Error setting non-blocking mode");
                sc_mgr_conn_release(mgr, conn);
                close(conn->fd);
                continue;
            }  

            struct epoll_event event = {
                .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT, // no edge triggered mode
                .data.ptr = conn
            };

            // add the event to epoll 
            if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, conn->fd, &event) == -1) {
                perror("Failed to add connection to epoll");
                sc_mgr_conn_release(mgr, conn);
                close(conn->fd);
                continue;
            }
            fprintf(stdout, "Connection successfully established\n");
        } else {
            // existing connection handling
            fprintf(stdout, "Request on existing connection\n");
            sc_conn *conn = mgr->events[i].data.ptr;
            if (!conn) {
                perror("Critical: Error gathering connection struct from epoll event");
                continue;
            }
            fprintf(stdout, "Gathered connection data: fd: %d, last_active: %lo, creation time: %lo, state: %d\n", conn->fd, conn->last_active, conn->creation_time, conn->state);
            
            if (mgr->events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                sc_mgr_conn_release(mgr, conn);
                continue;
            }

            char buf[1024] = {0};

            if (mgr->events[i].events & EPOLLIN) {
                conn->last_active = time(NULL);
                
                // TODO: read in a loop until EAGAIN for better performance (less epoll_wait() calls)
                ssize_t valread = read(conn->fd, buf, sizeof(buf) - 1); // because we are using LT mode, we can just read once

                if (valread == -1) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                        close(conn->fd);
                        sc_mgr_conn_release(mgr, conn); 
                    }
                    continue;
                }

                if (valread == 0) { // no data to read, just close the connection
                    epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->fd);
                    sc_mgr_conn_release(mgr, conn);
                    continue;
                }

                // now we read data
                buf[valread] = '\0';
                int keep_alive = 1;
                if (strstr(buf, "Connection: close")) {
                    keep_alive = 0;
                }

                // prepare default response
                
                const char *hello = "Hello, World!\n";
                char msg[1024];
                char headers[256];
                snprintf(headers, sizeof(headers),
                    "Content-length: %zu\r\n%s", strlen(hello),
                    keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n"
                );
                snprintf(msg, sizeof(msg), "HTTP/1.1 200 OK\r\n%s\r\n%s", headers, hello);
                
                if (send(conn->fd, msg, strlen(msg), 0) == -1) {
                    perror("Error sending response");
                } else {
                    fprintf(stdout, "Sent response\n");
                }

                // check for keep alive
                if (!keep_alive) {
                    fprintf(stdout, "Connection close requested");
                    sc_mgr_conn_release(mgr, conn);
                    close(conn->fd);
                    epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                }
            } else {
                perror("Error reading from client");
            }
        }
    }

    return SC_OK;
}

int sc_mgr_pool_init(sc_conn_mgr *mgr, int max_conns) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "NULL manager provided");

    mgr->max_conn_count = max_conns;
    mgr->conn_count = 0;

    mgr->conn_pool = calloc(max_conns, sizeof(sc_conn));
    if (mgr->conn_pool == NULL) {
        return SC_MALLOC_ERR;
    }

    mgr->free_conns = &mgr->conn_pool[0]; // free conns will be the same as con poolat the start
    // link list until max_conns - 1 to avoid segfault
    for(int i = 0; i < max_conns - 1; i++) {
        mgr->conn_pool[i].next = &mgr->conn_pool[i + 1];
        mgr->conn_pool[i].state = CONN_IDLE;
    }
    mgr->conn_pool[max_conns - 1].next = NULL;
    mgr->conn_pool[max_conns - 1].state = CONN_IDLE;

    mgr->conn_timeout = SC_DEFAULT_CONN_TIMEOUT;
    mgr->conn_max_age = SC_DEFAULT_CONN_MAX_AGE;

    return SC_OK;
}

sc_conn *sc_mgr_conn_get_free(sc_conn_mgr *mgr) {
    if (!mgr->free_conns) { // no free connections
        return NULL;
    }

    // pop first free conn from list
    sc_conn *conn = mgr->free_conns;
    mgr->free_conns = mgr->free_conns->next;

    // set flags n stuff
    conn->state = CONN_ACTIVE;
    conn->last_active = time(NULL);
    conn->creation_time = conn->last_active;

    mgr->conn_count++;
    
    return conn;
}

void sc_mgr_conn_release(sc_conn_mgr *mgr, sc_conn *conn) {
    if (!conn) return;

    // reset the connection
    conn->state = CONN_CLOSING;
    conn->last_active = time(NULL);

    // add connection back to free connection stack
    conn->next = mgr->free_conns;
    mgr->free_conns = conn;
    
    __atomic_fetch_sub(&mgr->conn_count, 1, __ATOMIC_SEQ_CST); // decrement the mgr conn count
}

void sc_mgr_conns_cleanup(sc_conn_mgr *mgr) {
    time_t now = time(NULL);

    for (size_t i = 0; i < mgr->max_conn_count; i++) {
        sc_conn *conn = &mgr->conn_pool[i];
        
        // check time limits
        if ((CONN_ACTIVE == conn->state) &&
        (now - conn->last_active > mgr->conn_timeout || now - conn->creation_time > mgr->conn_max_age)) {
            
            // close the fd
            shutdown(conn->fd, SHUT_RDWR);
            close(conn->fd);
            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
            sc_mgr_conn_release(mgr, conn);
        }
    }
}

int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, false, f);
    if (mgr->endpoints == NULL) {
       return SC_MALLOC_ERR;
    }
    return SC_OK;
}

int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, true, f);
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}
