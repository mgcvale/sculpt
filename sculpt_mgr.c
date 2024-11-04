#include "sculpt.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>

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
    if (!mgr) {
        return;
    }

    sc_mgr_conn_pool_destroy(mgr);
    printf("freed conn pool\n");

    // close epoll fd and free events array
    if (mgr->epoll_fd >= 0) {
        close(mgr->epoll_fd);
        mgr->epoll_fd = -1;
    }
    free(mgr->events);
    mgr->events = NULL; // !! dangling pointers

    printf("freed epoll\n");

    // close server socket
    if (mgr->fd >= 0) {
        close(mgr->fd);
        mgr->fd = -1;
    }
    printf("freed server socket\n");

    // free endpoints list
    while(mgr->endpoints) {
        struct _endpoint_list *next = mgr->endpoints->next;
        free(mgr->endpoints);
        mgr->endpoints = next;
    }
    free(mgr);
}

int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, false, f);
    if (mgr->endpoints == NULL) {
       return SC_MALLOC_ERR;
    }
    return SC_OK;
}

int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg)) {
    printf("Binding: %s\n", endpoint);
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, true, f);
    printf("Endpoint added: %s", mgr->endpoints->val.buf);
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}
