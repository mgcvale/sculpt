#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/epoll.h>
#include <time.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "sculpt.h"



int next_header(sc_conn_mgr *mgr, int fd, char *header, size_t buf_len);
int get_http_msg(sc_conn_mgr *mgr, char *header, sc_http_msg *http_msg);
int parse_all_headers(sc_conn_mgr *mgr, sc_conn *conn, sc_headers **headers, sc_http_msg *http_msg);

static bool check_conn_state(sc_conn_mgr *mgr) {
    FPRINTF_RETURN_ERROR_IF(!mgr || !mgr->mgr_initialized, false, "[Sculpt] Detected bad state during connection/epoll management (sc_conn_mgr is NULL or unitialized)");
    if (!mgr->pool_initialized || !mgr->epoll_initialized) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] Detected connection/epoll management with unitialized epoll or connection pool. Call the init functions for epoll and connection pooling before polling for requests.\n");
        return false;
    }
    if (!mgr->listening) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] Detected connection/epoll management before starting listening to requests. Call sc_mgr_listen() before polling for requests\n");
        return false;
    }

    return true;
}

int sc_mgr_epoll_init(sc_conn_mgr *mgr) {
    FPRINTF_RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");
    FPRINTF_RETURN_ERROR_IF(!mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] Initialize sc_conn_mgr via sc_mgr_create before calling sc_mgr_epoll_init");
    FPRINTF_RETURN_ERROR_IF(mgr->epoll_initialized, SC_BAD_STATE_ERR, "[Sculpt] Epoll was already initialized");

    // using EPOLL_CLOEXEC to prevent fd leaks across exec()
    mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    RETURN_ERROR_IF(mgr->epoll_fd == -1, SC_EPOLL_CREATION_ERR, "[Sculpt] epoll_create1 failed");

    int flags = fcntl(mgr->fd, F_GETFL);
    RETURN_ERROR_IF(flags == -1, SC_FCNTL_ERR, "[Sculpt] Failed to get socket flags");
    
    RETURN_ERROR_IF(fcntl(mgr->fd, F_SETFL, flags | O_NONBLOCK) == -1,
                   SC_FCNTL_ERR, "[Sculpt] Failed to set non-blocking mode");

    mgr->epoll_event.events = EPOLLIN | EPOLLRDHUP; // no edge triggered mode
    mgr->epoll_event.data.fd = mgr->fd;
    RETURN_ERROR_IF(epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, mgr->fd, &mgr->epoll_event) == -1,
                   SC_EPOLL_CTL_ERR, "[Sculpt] epoll_ctl failed");

    mgr->events = calloc(mgr->max_events, sizeof(struct epoll_event)); 
    RETURN_ERROR_IF(!mgr->events, SC_MALLOC_ERR, "[Sculpt] Failed to allocate events array");

    mgr->epoll_initialized = true;
    return SC_OK;
}

static int create_new_connection(sc_conn_mgr *mgr) {
    FPRINTF_RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");
    FPRINTF_RETURN_ERROR_IF(!mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] uinitialized manager provided");

    sc_log(mgr, SC_LL_DEBUG,  "[Sculpt] INFO Creating new connection\n"); 
    socklen_t addr_len = sizeof(mgr->sock_addr);

    // new connection, check capacity before proceeding
    sc_conn *conn = sc_mgr_conn_get_free(mgr);
    if (conn == NULL) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] ERROR No avaliable connections found! Sending 503 response\n");
        int client_fd = accept(mgr->fd, (struct sockaddr*)&mgr->sock_addr, &addr_len);
        if (client_fd != -1) {
             static const char *msg = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 21\r\n\r\nServer at capacity\r\n";
            send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);
            close(client_fd);
        }
        return SC_CONTINUE;
    }

    // valid connection was found, so we accept the request
    conn->fd = accept(mgr->fd, (struct sockaddr*)&mgr->sock_addr, &addr_len);

    if (conn->fd == -1) {
        sc_perror(mgr,  SC_LL_NORMAL, "[Sculpt] Error on Accept. Checking severity");
        sc_mgr_conn_pool_release(mgr, conn);
        if (errno != EAGAIN && errno != EWOULDBLOCK) { // if the error is not because it would block or cuz it is unavailable, we don't return the function
            sc_perror(mgr, SC_LL_DEBUG, "[Sculpt] Accept error:");  
            return SC_ACCEPT_ERR;
        }
        return SC_CONTINUE;
    }

    // set connection as non-blocking because we used accept instead of accept4
    if (fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK) == -1) {
        sc_perror(mgr, SC_LL_NORMAL, "Error setting non-blocking mode");
        sc_mgr_conn_pool_release(mgr, conn);
        close(conn->fd);
        return SC_CONTINUE;
    }  

    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT, // no edge triggered mode
        .data.ptr = conn
    };

    // add the event to epoll 
    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_ADD, conn->fd, &event) == -1) {
        sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Failed to add connection to epoll");
        sc_mgr_conn_pool_release(mgr, conn);
        close(conn->fd);
        return SC_CONTINUE;
    }
    return SC_OK;
}

static void return_500(sc_conn_mgr *mgr, sc_conn *conn) {
    const char *http_response_500 = 
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 21\r\n"
        "\r\n"
        "Internal Server Error";

    send(conn->fd, http_response_500, strlen(http_response_500), 0);
    sc_mgr_conn_release(mgr, conn);
}

static void epoll_readd_conn(sc_conn_mgr *mgr, sc_conn *conn) {
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
        .data.ptr = conn
    };

    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_MOD, conn->fd, &event) == -1) {
        sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Failed to re-add connection to epoll");
        sc_mgr_conn_release(mgr, conn);
    }
}

void sc_mgr_conn_readd(sc_conn_mgr *mgr, sc_conn *conn) {
    if (!check_conn_state(mgr)) return;
    epoll_readd_conn(mgr, conn);
}

void sc_mgr_conn_release(sc_conn_mgr *mgr, sc_conn *conn) {
    if (!check_conn_state(mgr)) return;
    if (!conn || conn->fd < 0) {
        sc_error_log(mgr, SC_LL_NORMAL, "Tried calling sc_mgr_conn_release with an invalid connection; skipping\n");
        return;
    }

    if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL) == -1) {
        sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] epoll_ctl DEL failed");
    }

    close(conn->fd);
    conn->fd = -1;

    sc_mgr_conn_pool_release(mgr, conn);
}

static void return_400(sc_conn_mgr *mgr, sc_conn *conn) {
    const char *response = 
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: 11\r\n"
        "\r\n"
        "Bad Request";

    send(conn->fd, response, strlen(response), 0);

    if (conn->persistent) {
        epoll_readd_conn(mgr, conn);
    } else {
        sc_mgr_conn_release(mgr, conn);
    }
}

void cleanup_after_error(sc_conn_mgr *mgr, sc_conn *conn) {
    if (conn) {
        return_500(mgr, conn);
    }
}

int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms) {
    // here I will use the manual macro instead of calling check_mgr_state because 
    // I need fine-grained control on the return code (eg BAD_ARGUMENTS, BAD_STATE, etc)
    FPRINTF_RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The mgr pointer cant be null");
    FPRINTF_RETURN_ERROR_IF(!mgr->mgr_initialized, SC_BAD_STATE_ERR, "[Sculpt] Initialize sc_conn_mgr via sc_mgr_create before calling sc_mgr_poll");
    FPRINTF_RETURN_ERROR_IF(!mgr->epoll_initialized, SC_BAD_STATE_ERR, "[Sculpt] Initialize epoll via sc_mgr_epoll_init before calling sc_mgr_poll");
    FPRINTF_RETURN_ERROR_IF(!mgr->pool_initialized, SC_BAD_STATE_ERR, "[Sculpt] Initialize connection pooling via sc_mgr_conn_pool_init before calling sc_mgr_poll");
    FPRINTF_RETURN_ERROR_IF(!mgr->listening, SC_BAD_STATE_ERR, "[Sculpt] Start server's listening with sc_mgr_listen before calling sc_mgr_poll");


    sc_mgr_conns_cleanup(mgr);

    // wait for some event in epoll and handle errors
    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, timeout_ms);
    if (n == -1) {
        if (errno == EINTR) { // not an error - the system just got interrupted mid syscall
            sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Warning - epoll_wait interrupted (errno = EINTR)\n");
            return SC_OK;
        }
        sc_perror(mgr, SC_LL_DEBUG, "[Sculpt] Error on epoll_wait");
        return SC_EPOLL_WAIT_ERR;
    }

    // log stuff
    sc_log(mgr,  SC_LL_DEBUG, "[Sculpt] Connection quantity: %d\n", mgr->conn_count);
    sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Epoll event quantity: %d\n", n);

    // manage all epoll events
    for (int i = 0; i < n; i++) {
       // handle errors with the epoll event
        if (mgr->events[i].events & EPOLLERR) {
            sc_conn *conn = mgr->events[i].data.ptr;
            sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Error with epoll, closing connection...\n");
            cleanup_after_error(mgr, conn);
            continue;
        }

        if (mgr->events[i].data.fd == mgr->fd) {
            // new connection handling
            int rc = create_new_connection(mgr);
            if (rc == SC_CONTINUE) continue;
            if (rc != SC_OK) return rc;
        } else {
            // existing connection handling

            sc_conn *conn = mgr->events[i].data.ptr; // get the connection and handle errors
            if (!conn) {
                sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: Error gathering connection struct from epoll event\n");
                continue;
            }
            if (mgr->events[i].events & (EPOLLERR)) {
                sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: error with epoll in new event, closing connection\n");
                sc_mgr_conn_release(mgr, conn);
                continue;
            }
            if (mgr->events[i].events & (EPOLLHUP)) {
                sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Client closed its connection.\n");
                sc_mgr_conn_release(mgr, conn);
                continue;
            } // will not handle EPOLLRDHUP, as the header parsing function already handles EOF

            // after getting the connection and managing errors, we can check if it has an input event and handle the input.
            if (mgr->events[i].events & EPOLLIN) {
                // input event from fd
                conn->last_active = time(NULL);
                
                /* HEADER PARSING */
                sc_http_msg http_msg = {0};
                sc_headers *headers = NULL;
                void *extra_data = NULL;
                if (mgr->protocol == SC_PROTOCOL_HTTP) { // parsing of HTTP headers
                    int err = parse_all_headers(mgr, conn, &headers, &http_msg);
                    
                    if (err == SC_MALFORMED_HEADER_ERR || err == SC_BUFFER_OVERFLOW_ERR) {
                        sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Returning 400 due to malformed headers\n");
                        return_400(mgr, conn);
                        sc_headers_free(headers);
                        sc_http_msg_free(&http_msg);
                        continue;
                    } else if (err == SC_CONN_CLOSED) {
                        sc_log(mgr, SC_LL_NORMAL, "[Sculpt] Detected socket closing during header pasrsing; closing the connection with the client\n");
                        sc_mgr_conn_release(mgr, conn);
                        sc_http_msg_free(&http_msg);
                        continue;
                    } else if (err != SC_OK) {
                        sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Returning 500 dure to internal server error while getting headers\n"); 
                        cleanup_after_error(mgr, conn);
                        sc_headers_free(headers);
                        sc_http_msg_free(&http_msg);
                        continue;
                    } // the parse_all_headers frunction already logs everything

                } else {
                    // if the protocol is not HTTP, we use the parser set in the mgr
                    int err = mgr->protocol_handler(conn, &http_msg, &headers, &extra_data);
                    if (err != SC_OK) {
                        sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Response code on protocol handler was not OK, calling protocol fallback function. Error code: %d", err);
                        mgr->protocol_fallback(mgr, conn, http_msg, headers, extra_data, err);
                        continue;
                    }
                    // TODO: add protocol cleanup callback support to avoid leaks
                }
                /* END OF HEADER PARSING */

                // log request
                sc_log(mgr, SC_LL_NORMAL, "[Sculpt] HTTP Request: %s on %s\n", http_msg.method.buf, http_msg.uri.buf);

                // here we will send the request to a endpoint handler (if possible)
                struct _endpoint_list *current = mgr->endpoints;
                while (current) {
                    if (current->soft) {
                        // we call it even if just the prefix matches (soft)
                        if (sc_strprefix(http_msg.uri, current->val)) {
                            // the uri buffer starts with the prefix of the endpoint

                            current->func(conn->fd, http_msg, headers, extra_data); // call the handler
                            goto end;
                        }
                    } else {
                        if(sc_strcmp(current->val, http_msg.uri) == 0) {
                            // we only call it if the URI is the SAME (hard)

                            current->func(conn->fd, http_msg, headers, extra_data); // call the handler
                            goto end;
                        }
                    }
                    current = current->next;
                }

                // if no valid enpoints were found, we return HTTP 404
                const char *http_response_404 = 
                    "HTTP/1.1 404 NOT FOUND\r\n"
                    "Content-Type: text/html; charset=UTF-8\r\n"
                    "Content-Length: 9\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "NOT FOUND";
                if (send(conn->fd, http_response_404, strlen(http_response_404), 0) == -1) {
                    sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Error sending 404 response on unset route");
                }

                end:
                    // cleanup headers and re-add connection to epoll (if persistent is true due to keep-alive header)
                    sc_headers_free(headers);
                    sc_http_msg_free(&http_msg);

                    if (!conn->persistent) {
                        // delete and release connection

                        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Connection close requested\n");
                        sc_mgr_conn_release(mgr, conn);
                    } else {
                        // re-add the connection to epoll for further requests

                        epoll_readd_conn(mgr, conn);
                    }

                // all other responsibilities are passed to the handler, so no need to do anything else
            } else {
                // this means that there was no input event
                sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Error reading from client: no input event in epoll");
            }
        }
    }
    return SC_OK;
}
