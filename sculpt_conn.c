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

static int create_new_connection(sc_conn_mgr *mgr) {
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
        return SC_CONTINUE;
    }

    // try to find an unused connection
    sc_conn *conn = sc_mgr_conn_get_free(mgr);
    if (!conn) {
        perror("Failed to find free connection on sc_mgr_conn_get_free()\n");
        return SC_CONTINUE;
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
        return SC_CONTINUE;
    }

    // set connection as non-blocking because we used accept instead of accept4
    if (fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK) == -1) {
        perror("Error setting non-blocking mode");
        sc_mgr_conn_release(mgr, conn);
        close(conn->fd);
        return SC_CONTINUE;
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
        return SC_CONTINUE;
    }
    fprintf(stdout, "Connection successfully established\n");
    return SC_OK;
}

static void return_500(sc_conn_mgr *mgr, sc_conn *conn) {
     const char *http_response_500 = 
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 0\r\n"
        "\r\n";

     send(conn->fd, http_response_500, strlen(http_response_500), 0);
     close(conn->fd);
     sc_mgr_conn_release(mgr, conn);                    
     epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
}

int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "The mgr pointer cant be null");
    sc_mgr_conns_cleanup(mgr);
    
    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, timeout_ms);
    if (n == -1) {
        if (errno == EINTR) return SC_OK;
        return SC_EPOLL_WAIT_ERR;
    }
    printf("Connection quantity: %d\n", mgr->conn_count);

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
            int rc = create_new_connection(mgr);
            if (rc == SC_CONTINUE) continue;
            if (rc != SC_OK) return rc;
        } else {
            // existing connection handling
            fprintf(stdout, "Request on existing connection\n");
            sc_conn *conn = mgr->events[i].data.ptr;
            if (!conn) {
                perror("Critical: Error gathering connection struct from epoll event");
                continue;
            }
            if (mgr->events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                sc_mgr_conn_release(mgr, conn);
                continue;
            }

            char headers[HEADER_BUF_SIZE] = {0};

            if (mgr->events[i].events & EPOLLIN) {
                bool keep_alive = false;
                conn->last_active = time(NULL);
               
                // parse the basic headers
                char headers[HEADER_BUF_SIZE] = {'\0'};
                size_t headers_len = 0;
                char buf[1] = {0};
                while (headers_len < HEADER_BUF_SIZE - 1) {
                    ssize_t bytes_read = read(conn->fd, buf, 1);
                    if (bytes_read >= 0) {     
                        headers[headers_len] = buf[0];
                        headers[headers_len + 1] = '\0';
                        headers_len ++;
                        if (strstr(headers, "\r\n\r\n")) {
                            break; // stop if we got to the end of the headers
                        }
                    } else if (bytes_read == -1) {
                         if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // no more data to read, break out of loop
                            break;
                         }
                         // else this was just an error
                         printf("Error reading body from client\n");
                         return_500(mgr, conn);
                         return SC_OK;
                    } else { // bytes_read == 0 (conn closed by client)
                        close(conn->fd);
                        sc_mgr_conn_release(mgr, conn);
                        return SC_OK;
                    }
                }

                sc_http_msg http_msg;
                char method_buf[METHOD_BUF_SIZE];
                char uri_buf[URL_BUF_SIZE];
                sscanf(headers, "%s %s", method_buf, uri_buf);
                http_msg.method = sc_mk_str(method_buf);
                http_msg.uri = sc_mk_str(uri_buf);
                sc_str sc_headers = sc_mk_str(headers);

                if (strstr(headers, "Connection: keep-alive")) {
                    keep_alive = true;
                }

                struct _endpoint_list *current = mgr->endpoints;
                while (current) {
                    if (current->soft) {
                        // we call it even if just the prefix matches
                        if (sc_strprefix(http_msg.uri, current->val)) {
                            // the uri buffer starts with the prefix of the endpoint
                            current->func(conn->fd, http_msg);
                            goto end;
                        }
                    } else {
                        printf("hard\n");
                        if(sc_strcmp(current->val, http_msg.uri) == 0) {
                            // the uri buffer is EQUAL to the endpoint
                            current->func(conn->fd, http_msg);
                            goto end;
                        }
                    }
                    current = current->next;
                }
                // no valid enpoints were found, so we return 404
                const char *http_response_404 = 
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=UTF-8\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n" // this is 200 ok for testing purposes. Ignore.
                "\r\n";
                if (send(conn->fd, http_response_404, strlen(http_response_404), 0) == -1) {
                    perror("Error sending response");
                }

                end:
                    if (!keep_alive) {
                        fprintf(stdout, "Connection close requested\n");
                        close(conn->fd);
                        sc_mgr_conn_release(mgr, conn);                    
                        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    } else {
                        printf("Re-adding connection to epoll\n");
                        // Re-add the connection to epoll for further requests
                        struct epoll_event event = {
                            .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
                            .data.ptr = conn
                        };
                        if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_MOD, conn->fd, &event) == -1) {
                            perror("Failed to re-add connection to epoll");
                            close(conn->fd);
                            sc_mgr_conn_release(mgr, conn);
                        }
                    }

                // all other responsibilities are passed to the handler, so no need to do anything else
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

