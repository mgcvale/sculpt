#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdbool.h>
#include <time.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "sculpt.h"

static bool check_pool_state(sc_conn_mgr *mgr) {
    if (!mgr || !mgr->mgr_initialized || !mgr->pool_initialized) {
        if (mgr && mgr->mgr_initialized) sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] An error occurred on sc_mgr_conn_* because some sculpt component was not properly initialized\n");
        else fprintf(stderr, "[Sculpt] An error occurred on sc_mgr_conn_* because some sculpt component was not properly initialized\n");
        return false;
    }
    return true;
}

int sc_mgr_conn_pool_init(sc_conn_mgr *mgr) {
    FPRINTF_RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");
    FPRINTF_RETURN_ERROR_IF(!mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] Initialize sc_conn_mgr via sc_mgr_create before calling sc_mgr_conn_pool_init");
    FPRINTF_RETURN_ERROR_IF(mgr->pool_initialized, SC_BAD_STATE_ERR, "[Sculpt] Connection pool is already initialized; skipping sc_mgr_conn_pool_init");
    FPRINTF_RETURN_ERROR_IF(mgr->conn_pool_size == 0, SC_BAD_STATE_ERR, "[Sculpt] Connection pool size can't be 0. Change the value of conn_pool_size on your config file or initialization code.");

    mgr->conn_count = 0;

    mgr->conn_pool = calloc(mgr->conn_pool_size, sizeof(sc_conn));
    if (mgr->conn_pool == NULL) {
        return SC_MALLOC_ERR;
    }

    mgr->free_conns = &mgr->conn_pool[0]; // free conns will be the same as con poolat the start
    // link list until max_conns - 1 to avoid segfault
    for(int i = 0; i < mgr->conn_pool_size - 1; i++) {
        mgr->conn_pool[i].next = &mgr->conn_pool[i + 1];
        mgr->conn_pool[i].state = CONN_IDLE;
    }
    mgr->conn_pool[mgr->conn_pool_size- 1].next = NULL;
    mgr->conn_pool[mgr->conn_pool_size - 1].state = CONN_IDLE;

    mgr->conn_timeout = SC_DEFAULT_CONN_TIMEOUT;
    mgr->conn_max_age = SC_DEFAULT_CONN_MAX_AGE;

    mgr->pool_initialized = true;
    return SC_OK;
}

sc_conn *sc_mgr_conn_get_free(sc_conn_mgr *mgr) {
    if (!check_pool_state(mgr)) {
        return NULL;
    }

    if (mgr->conn_count >= mgr->conn_pool_size) {
        // all connections are being used.
        // If the recycle_conns parameter is false, we return null. Else, we free the last active conn and return it.
        if (!mgr->recycle_conns) {
            sc_log(mgr, SC_LL_DEBUG, "recycle_conns is false; returning NULL on conn_get_free\n");
            return NULL; 
        }
        
        sc_log(mgr, SC_LL_DEBUG, "All connections are being used; releasing the oldest inactive one.\n");
        
        sc_conn *oldest = &mgr->conn_pool[0];
        time_t oldest_time = time(NULL);
        for (size_t i = 0; i < mgr->conn_pool_size; i++) {
            sc_conn *conn = &mgr->conn_pool[i];
            if (conn->state == CONN_ACTIVE && conn->last_active < oldest_time) {
                oldest_time = conn->last_active;
                oldest = conn;
            }
        }

        // release the oldest connection
        sc_mgr_conn_release(mgr, oldest);
    }

    if (!mgr->free_conns) return NULL;

    // pop first free conn from list
    sc_conn *conn = mgr->free_conns;
    mgr->free_conns = mgr->free_conns->next;

    // clear previous conn state
    // init new conn
    time_t current_time = time(NULL);
    conn->last_active = current_time;
    conn->creation_time = current_time;
    conn->state = CONN_ACTIVE;
    conn->persistent = false;
    conn->fd = -1; // fd will be invalid until it is set

    __atomic_fetch_add(&mgr->conn_count, 1, __ATOMIC_SEQ_CST);

    return conn;
}

void sc_mgr_conn_pool_release(sc_conn_mgr *mgr, sc_conn *conn) {
    if (!check_pool_state(mgr)) {
        return;
    }
    if (!conn) {
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Null connection passed to sc_mgr_conn_pool_release; ignoring it\n");
        return;
    }
    if (conn->state == CONN_CLOSING) {
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Not releasing connection because its state is CONN_CLOSING\n");
        return;
    }

    // add connection back to free connection stack
    conn->next = mgr->free_conns;
    mgr->free_conns = conn;

    // reset the connection
    conn->state = CONN_CLOSING;
    conn->last_active = time(NULL);

    __atomic_fetch_sub(&mgr->conn_count, 1, __ATOMIC_SEQ_CST); // decrement the mgr conn count
}

void sc_mgr_conns_cleanup(sc_conn_mgr *mgr) {
    if (!check_pool_state(mgr)) {
        return;
    }

    time_t now = time(NULL);

    for (size_t i = 0; i < mgr->conn_pool_size; i++) {
        sc_conn *conn = &mgr->conn_pool[i];
        
        // check time limits
        if ((CONN_ACTIVE == conn->state) &&
        (now - conn->last_active > mgr->conn_timeout || now - conn->creation_time > mgr->conn_max_age)) {
            
            // close the fd
            shutdown(conn->fd, SHUT_RDWR);
            sc_mgr_conn_release(mgr, conn);
        }
    }
}

void sc_mgr_conn_pool_destroy(sc_conn_mgr *mgr) {
    if (!mgr || !mgr->mgr_initialized) {
        fprintf(stderr, "[Sculpt] Can't destroy the connection pool of a NULL or unitialized sc_conn_mgr\n");
        return;
    }
    if (!mgr->pool_initialized) {
        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Can't destroy an unitialized connection pool\n"); 
        return;
    }

    // close all active connections from array
    for (int i = 0; i < mgr->conn_pool_size; i++) {
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

    // pool isn't initialized anymore
    mgr->pool_initialized = false;
}
