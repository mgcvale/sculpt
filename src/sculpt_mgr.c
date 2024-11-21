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
        perror("[Sculpt] Error: memory allocation for sc_conn_mgr");
        *err = SC_MALLOC_ERR;
        return NULL;
    }

    mgr->addr_info = addr_mgr;
    mgr->backlog = SC_DEFAULT_BACKLOG;
    mgr->max_events = SC_DEFAULT_EPOLL_MAXEVENTS;
    mgr->listening = false;
    mgr->epoll_fd = -1;
    mgr->endpoints = NULL;
    mgr->ll = SC_LL_NORMAL;

    mgr->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mgr->fd < 0) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: error creating socket for conn_mgr");
        free(mgr);
        *err = SC_SOCKET_CREATION_ERR;
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(mgr->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: failed to set socket options");
        *err = SC_SOCKET_SETOPT_ERR;
        goto error;
    }

    if (bind(mgr->fd, (struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info))) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: Failed to bind server to the address");
        *err = SC_SOCKET_BIND_ERR;
        goto error;
    }

    return mgr;

    error:
        close(mgr->fd);
        free(mgr);
        return NULL;
}

void sc_mgr_backlog_set(sc_conn_mgr *mgr, int backlog) {
    mgr->backlog = backlog;
}

void sc_mgr_epoll_maxevents_set(sc_conn_mgr *mgr, int maxevents) {
    mgr->max_events = maxevents;
}

void sc_mgr_ll_set(sc_conn_mgr *mgr, int ll) {
    mgr->ll = ll;
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
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Warning: %s; ", gai_strerror(rc));
        sc_error_log(mgr, SC_LL_MINIMAL, "Server is listening on unknown URL\n");
        return SC_SOCKET_GETNAMEINFO_ERR;
    }

    sc_log(mgr, SC_LL_MINIMAL, "\n[Sculpt] Server is listening on http://%s%s:%d\n", mgr->host_buf, mgr->service_buf, mgr->addr_info.port);

    mgr->listening = true;
    return SC_OK;
}

void sc_mgr_finish(sc_conn_mgr *mgr) {
    if (!mgr) {
        return;
    }
    int ll = mgr->ll;

    sc_mgr_conn_pool_destroy(mgr);
    sc_log(mgr, SC_LL_DEBUG, "[Sculpt]freed conn pool\n");

    // close epoll fd and free events array
    if (mgr->epoll_fd >= 0) {
        close(mgr->epoll_fd);
        mgr->epoll_fd = -1;
    }
    free(mgr->events);
    mgr->events = NULL; // !! dangling pointers

    sc_log(mgr, SC_LL_DEBUG, "[Sculpt] freed epoll\n");

    // close server socket
    if (mgr->fd >= 0) {
        close(mgr->fd);
        mgr->fd = -1;
    }
    sc_log(mgr, SC_LL_DEBUG, "[Sculpt]freed server socket\n");
    

    // free endpoints list
    while(mgr->endpoints) {
        struct _endpoint_list *next = mgr->endpoints->next;
        free(mgr->endpoints);
        mgr->endpoints = next;
    }
    free(mgr);
}

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int, sc_http_msg, sc_headers*)) {
    struct _endpoint_list *new = malloc(sizeof(struct _endpoint_list));
    if (new == NULL) {
        return NULL;
    }

    new->soft = soft;
    new->func = func;
    sc_str val = sc_str_ref_n(endpoint, strlen(endpoint));
    new->val = val;
    new->next = list;
    return new;
}

int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, false, f);
    if (mgr->endpoints == NULL) {
       return SC_MALLOC_ERR;
    }
    return SC_OK;
}

int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, true, f);
    sc_log(mgr, SC_LL_DEBUG, "[Sculpt]Endpoint added: %s", mgr->endpoints->val.buf);
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}
