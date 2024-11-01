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

#include "sculpt.h"


sc_addr_info sc_addr_create(int sin_family, int port) {
    sc_addr_info addr_mgr;
    addr_mgr._sock_addr.sin_family = sin_family;
    addr_mgr._sock_addr.sin_port = htons(port);
    addr_mgr._sock_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr_mgr.port = port;
    return addr_mgr;
}

sc_conn_mgr *sc_conn_create(sc_addr_info addr_mgr, int *err) {
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

void sc_conn_set_backlog(sc_conn_mgr *mgr, int backlog) {
    mgr->backlog = backlog;
}

void sc_conn_set_epoll_maxevents(sc_conn_mgr *mgr, int maxevents) {
    mgr->max_events = maxevents;
}

int sc_conn_listen(sc_conn_mgr *mgr) {
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

void sc_conn_finish(sc_conn_mgr *mgr) {
    close(mgr->fd);
    if (mgr->epoll_fd != -1) {
        close(mgr->epoll_fd);
    }
    if (mgr->events != NULL) {
        free(mgr->events);
    }
    free(mgr);
}

int sc_conn_epoll_init(sc_conn_mgr *mgr) {
    mgr->epoll_fd = epoll_create1(0);
    if (mgr->epoll_fd == -1) {
        return SC_EPOLL_CREATION_ERR;
    }

    mgr->epoll_event.events = EPOLLIN;
    mgr->epoll_event.data.fd = mgr->fd;
    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->fd, &mgr->epoll_event) == -1) {
        return SC_EPOLL_CTL_ERR;
    }

    mgr->events = malloc(sizeof(struct epoll_event) * mgr->max_events);
    if (mgr->events == NULL) {
        return SC_MALLOC_ERR;
    }

    //TODO: set epoll nonblocking flags

    return SC_OK;
}

int sc_conn_poll(sc_conn_mgr *mgr) {
    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, -1);
    if (n == -1) {
        return SC_EPOLL_WAIT_ERR;
    }

    for (int i = 0; i < n; i++) {
        if (mgr->events[i].data.fd == mgr->fd) {
            // New connection
            socklen_t addr_len = sizeof(mgr->addr_info._sock_addr);
            int client_fd = accept(mgr->fd,
                (struct sockaddr*)&mgr->addr_info._sock_addr, &addr_len);
            
            if (client_fd == -1) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "Accept error: %d\n", errno);
                    return SC_ACCEPT_ERR;
                }
                continue;
            }

            // Set connection as non-blocking
            int flags = fcntl(client_fd, F_GETFL, 0);
            if (flags != -1) {
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);
            }

            struct epoll_event event;
            event.events = EPOLLIN | EPOLLET;  // set as edge triggered
            event.data.fd = client_fd;
            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, client_fd, &event);
        } else {
            int client_fd = mgr->events[i].data.fd;
            char buf[1024] = {0};

            if (mgr->events[i].events & EPOLLIN) {
                size_t valread = read(client_fd, buf, sizeof(buf) - 1);
                if (valread > 0) {
                    buf[valread] = '\0';
                    printf("Request: %s\n", buf);

                    // Determine connection persistence
                    int keep_alive = 1;
                    if (strstr(buf, "Connection: close")) {
                        keep_alive = 0;
                    }

                    // Prepare response
                    const char *hello = "Hello, World!\n";
                    char msg[1024];
                    char headers[256];
                    snprintf(headers, sizeof(headers), 
                        "Content-Length: %zu\r\n%s", 
                        strlen(hello), 
                        keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n"
                    );
                    snprintf(msg, sizeof(msg), "HTTP/1.1 200 OK\r\n%s\r\n%s", headers, hello);

                    send(client_fd, msg, strlen(msg), 0);
                    printf("Sent response\n");

                    // Only close if explicitly requested
                    if (!keep_alive) {
                        close(client_fd);
                        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    }
                } else if (valread == 0) {
                    // Connection closed by client
                    close(client_fd);
                    epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                }
            }

            // Only close on actual errors
            if (mgr->events[i].events & (EPOLLERR)) {
                close(client_fd);
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
            }
        }
    }

    return SC_OK;
}

int sc_conn_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, false, f);
    if (mgr->endpoints == NULL) {
       return SC_MALLOC_ERR;
    }
    return SC_OK;
}

int sc_conn_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, true, f);
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}
