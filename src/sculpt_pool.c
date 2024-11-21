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


int sc_mgr_conn_pool_init(sc_conn_mgr *mgr, int max_conns) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");

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
    if (mgr == NULL || mgr->conn_count >= mgr->max_conn_count) return NULL;

    // pop first free conn from list
    sc_conn *conn = mgr->free_conns;
    mgr->free_conns = mgr->free_conns->next;

    // clear previous conn state
    // init new conn
    time_t current_time = time(NULL);
    conn->last_active = current_time;
    conn->creation_time = current_time;
    conn->state = CONN_ACTIVE;
    conn->fd = -1; // fd will be invalid until it is set

    __atomic_fetch_add(&mgr->conn_count, 1, __ATOMIC_SEQ_CST);

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

void sc_mgr_conn_pool_destroy(sc_conn_mgr *mgr) {
    // close all active connections from array
    for (int i = 0; i < mgr->max_conn_count; i++) {
        sc_conn *conn = &mgr->conn_pool[i];
        if (conn->state == CONN_ACTIVE) {
            close(conn->fd);
        }
        //free(conn);
    }
    
    // free pools
    free(mgr->conn_pool);
    mgr->conn_pool = NULL;
    mgr->free_conns = NULL;

}
