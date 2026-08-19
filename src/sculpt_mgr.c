#include "sculpt.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <string.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

sc_conn_mgr *sc_mgr_create(int *err) {
    *err = SC_OK;
    sc_conn_mgr *mgr = calloc(1, sizeof(sc_conn_mgr));
    if (mgr == NULL) {
        perror("[Sculpt] Error: memory allocation for sc_conn_mgr");
        if (err != NULL) *err = SC_MALLOC_ERR;
        return NULL;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sockaddr_in sock_addr;
    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(SC_DEFAULT_PORT);
    sock_addr.sin_addr.s_addr = htonl(SC_DEFAULT_S_ADDR);
    mgr->sock_addr = sock_addr;
    mgr->backlog = SC_DEFAULT_BACKLOG;
    mgr->max_events = SC_DEFAULT_EPOLL_MAXEVENTS;
    mgr->listening = false;
    mgr->epoll_fd = -1;
    mgr->endpoints = NULL;
    mgr->ll = SC_LL_NORMAL;
    mgr->recycle_conns = true;
    mgr->protocol = SC_PROTOCOL_HTTP;
    mgr->conn_pool_size = SC_DEFAULT_CONN_POOL_SIZE;

    mgr->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mgr->fd < 0) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: error creating socket for conn_mgr");
        free(mgr);
        if (err != NULL) *err = SC_SOCKET_CREATION_ERR;
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(mgr->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: failed to set socket options");
        if (err != NULL) *err = SC_SOCKET_SETOPT_ERR;
        goto error;
    }

    mgr->mgr_initialized = true;
    return mgr;

    error:
        close(mgr->fd);
        free(mgr);
        return NULL;
}

static char *ll_to_str(int ll) {
    switch (ll) {
        case SC_LL_NONE:
            return "SC_LL_NONE";
        case SC_LL_MINIMAL:
            return "SC_LL_MINIMAL";
        case SC_LL_NORMAL:
            return "SC_LL_NORMAL";
        case SC_LL_DEBUG:
            return "SC_LL_DEBUG";
        default:
            return "Unkwon log level";
    }
}

void sc_mgr_state_log(sc_conn_mgr *mgr, FILE *file) {
    const char *format = ""
        "\n\n[SCULPT STATE LOG]\n"
        "\tlog level: %s\n"
        "\tbacklog size: %zu\n"
        "\tepoll max events: %zu\n"
        "\trecycle conns: %s\n"
        "\tmax connection pool capacity: %zu\n"
        "\tmax connection idle time before closing: %zus\n"
        "\tprotocol: %s\n\n";
    fprintf(file, format,
            ll_to_str(mgr->ll),
            mgr->backlog,
            mgr->max_events,
            mgr->recycle_conns ? "true" : "false",
            mgr->conn_pool_size,
            mgr->conn_timeout,
            mgr->protocol == SC_PROTOCOL_HTTP ? "http" : "custom"
        );
}

void sc_mgr_backlog_set(sc_conn_mgr *mgr, int backlog) {
    mgr->backlog = backlog;
}

void sc_mgr_epoll_maxevents_set(sc_conn_mgr *mgr, int maxevents) {
    mgr->max_events = maxevents;
}

void sc_mgr_ll_set(sc_conn_mgr *mgr, int ll) {
    if (ll < SC_LL_NONE || ll > SC_LL_DEBUG) {
        if (ll > SC_LL_NONE) {
            fprintf(stderr, "[W] Tried to set invalid log level: got %d; min is %d (SC_LL_NONE); max is %d (SC_LL_DEBUG).\n", ll, SC_LL_NONE, SC_LL_DEBUG);
        }
        ll = MIN(MAX(ll, SC_LL_NONE), SC_LL_DEBUG);
    }

    mgr->ll = ll;
}

void sc_mgr_conn_recycling_set(sc_conn_mgr *mgr, int recycle) {
    mgr->recycle_conns = recycle;
}

void sc_mgr_conn_pool_size_set(sc_conn_mgr *mgr, int pool_size) {
    if (mgr->pool_initialized) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[W] You tried to set the connection pool size to %d *after* the connection pool had already been initialized, which sculpt doesn't support. Set it before calling sc_mgr_conn_pool_init or sc_mgr_init_all. The size will be kept at %d.\n", pool_size, mgr->conn_pool_size);
        return;
    }
    mgr->conn_pool_size = pool_size;
}

void sc_mgr_addr_set(sc_conn_mgr *mgr, const char *addr) {
    if (mgr->listening) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[W] You tried to set the address *after* sculpt was already listening to requests, so this change will make no effect. All configs must be set right after calling sc_mgr_create() to avoid this kind of error.\n");
        return;
    }

    if (inet_pton(AF_INET, addr, &mgr->sock_addr.sin_addr) <= 0) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[W] The address provided `%s` provided is invalid. Thus, the address wasn't changed.\n", addr);
    }
}

void sc_mgr_port_set(sc_conn_mgr *mgr, unsigned short port) {
    if (mgr->listening) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[W] You tried to set the port *after* sculpt was already listening to requests, so this change will make no effect. All configs must be set right after calling sc_mgr_create() to avoid this kind of error.\n");
        return;
    }

    mgr->sock_addr.sin_port = htons(port);
}

int sc_mgr_listen(sc_conn_mgr *mgr) {
    FPRINTF_RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");
    FPRINTF_RETURN_ERROR_IF(!mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] initialize sc_conn_mgr via sc_mgr_create before calling sc_mgr_listen");

    if (bind(mgr->fd, (struct sockaddr *)&mgr->sock_addr, sizeof(mgr->sock_addr))) {
        sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error: Failed to bind server to the address");
        return SC_SOCKET_BIND_ERR;
    }

    if (listen(mgr->fd, mgr->backlog) < 0) {
        perror("Error: error in listen()");
        return SC_SOCKET_LISTEN_ERR;
    }

    printf("Binding to: %s:%d\n", inet_ntoa(mgr->sock_addr.sin_addr), ntohs(mgr->sock_addr.sin_port));
    int rc = getnameinfo((struct sockaddr *)&mgr->sock_addr, sizeof(mgr->sock_addr),
                        mgr->host_buf, sizeof(mgr->host_buf),
                        mgr->service_buf, sizeof(mgr->service_buf), 0);
    if (rc != 0) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Warning: %s; ", gai_strerror(rc));
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Server is listening on unknown URL\n");
        return SC_SOCKET_GETNAMEINFO_ERR;
    }

    sc_log(mgr, SC_LL_MINIMAL, "\n[Sculpt] Server is listening on http://%s:%d\n", mgr->host_buf, ntohs(mgr->sock_addr.sin_port));

    mgr->listening = true;
    return SC_OK;
}

void sc_mgr_finish(sc_conn_mgr *mgr) {
    if (!mgr || !mgr->mgr_initialized) {
        // mgr wasn't initialized. We don't have to log anything - we can just
        // fail silently, considering the manager wasn't initialized anyway
        return;
    }

    if (mgr->pool_initialized) {
        sc_mgr_conn_pool_destroy(mgr);
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] freed conn pool\n");
    } else {
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] didn't free conn pool because it wasn't initialized\n");
    }

    // close epoll fd and free events array
    // we should close epoll's fd even if epoll_initialized is false - cause it may be present even when that flag is false.

    if (mgr->epoll_fd >= 0) {
        close(mgr->epoll_fd);
        mgr->epoll_fd = -1;
    }

    if (mgr->epoll_initialized) {
        free(mgr->events);
        mgr->events = NULL;
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] freed epoll\n");
    } else {
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] didn't free epoll because it wasn't initialized; closed epoll's fd nonetheless\n");
    }

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

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int, sc_http_msg, sc_headers*, void*)) {
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

int sc_mgr_bind_hard(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*, void*)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, false, f);
    if (mgr->endpoints == NULL) {
       return SC_MALLOC_ERR;
    }
    return SC_OK;
}

int sc_mgr_bind_soft(sc_conn_mgr *mgr, const char *endpoint, void (*f)(int, sc_http_msg, sc_headers*, void*)) {
    mgr->endpoints = _endpoint_add(mgr->endpoints, endpoint, true, f);
    sc_log(mgr, SC_LL_DEBUG, "[Sculpt]Endpoint added: %s", mgr->endpoints->val.buf);
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}
