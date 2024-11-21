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

#define SC_HEADER_PARSE_ERR -256
#define SC_HEADER_PARSE_INCOMPLETE_ERR -257

int sc_mgr_epoll_init(sc_conn_mgr *mgr) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");

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
    RETURN_ERROR_IF(!mgr->events, SC_MALLOC_ERR, "[Sculpt] Failed to allocate events array");return SC_OK;
}

static int create_new_connection(sc_conn_mgr *mgr) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] NULL manager provided");
    sc_log(mgr, SC_LL_DEBUG,  "[Sculpt] INFO Creating new connection\n"); 
    socklen_t addr_len = sizeof(mgr->addr_info._sock_addr);

    // new connection, check capacity before proceeding
    if (mgr->conn_count >= mgr->max_conn_count) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] ERROR No avaliable connections found! Sending 503 response");
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
        perror("[Sculpt] Failed to find free connection on sc_mgr_conn_get_free()\n");
        return SC_CONTINUE;
    }

    // valid connection was found, so we accept the request
     conn->fd = accept(mgr->fd, (struct sockaddr*)&mgr->addr_info._sock_addr, &addr_len);

    if (conn->fd == -1) {
        sc_perror(mgr,  SC_LL_NORMAL, "[Sculpt] Error on Accept. Checking severity\n");
        sc_mgr_conn_release(mgr, conn);
        if (errno != EAGAIN && errno != EWOULDBLOCK) { // if the error is not because it would block or cuz it is unavailable, we don't return the function
            sc_perror(mgr, SC_LL_DEBUG, "[Scupt] Accept error:");  
            return SC_ACCEPT_ERR;
        }
        return SC_CONTINUE;
    }

    // set connection as non-blocking because we used accept instead of accept4
    if (fcntl(conn->fd, F_SETFL, fcntl(conn->fd, F_GETFL) | O_NONBLOCK) == -1) {
        sc_perror(mgr, SC_LL_NORMAL, "Error setting non-blocking mode");
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
        sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Failed to add connection to epoll");
        sc_mgr_conn_release(mgr, conn);
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
     close(conn->fd);
     sc_mgr_conn_release(mgr, conn);                    
     epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
}

void cleanup_after_error(sc_conn_mgr *mgr, sc_conn *conn) {
    if (conn) {
        return_500(mgr, conn);
        sc_mgr_conn_release(mgr, conn);
        close(conn->fd);
        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    }
}

int next_header(sc_conn_mgr *mgr, int fd, char *header, size_t buf_len) {
    header[0] = '\0';
    size_t header_len = 0;
    char last_char = '\0';

    while (1) {
        if (header_len >= buf_len - 1) { // stop if the header is larger than the buffer
            return SC_BUFFER_OVERFLOW_ERR;
        }

        ssize_t bytes_read = read(fd, &header[header_len], 1);
        if (bytes_read > 0) {
            if (last_char == '\r' && header[header_len] == '\n') {
                // we got to the end of the header
                header[header_len - 1] = '\0';
                header_len --;
                break;
            }
            last_char = header[header_len];
            header_len++;
            header[header_len] = '\0';        
        } else if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no more data to read, break out of loop
                break;
            }
            // an actual error occoured
            sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Error reading body from client");
            return SC_READ_ERR;
        } else if (bytes_read == 0) {
            // EOF e or client closed the connection
            if (header_len > 0) {
                // we still have some header data read, but EOF is reached
                return SC_OK; 
            }
            return SC_FINISHED;
        }
    }

    if (header[0] == '\0') { // the header is empty, that is, we have reached the end of the headers
        return SC_FINISHED;
    }

    return SC_OK;
}

int get_http_msg(sc_conn_mgr *mgr, char *header, sc_http_msg *http_msg) {
    RETURN_ERROR_IF(!http_msg, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The http_msg pointer can't be null");
    RETURN_ERROR_IF(!header, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The sc_header pointer can't be null");
 
    const char *space = strchr(header, ' ');
    RETURN_ERROR_IF(space == NULL, SC_MALFORMED_HEADER_ERR, "[Sculpt] The header passed to get_http_msg was malformed, as it was missing a space character");
    
    // find method in header
    size_t method_len = space - header;
    if (method_len == 0 || method_len > METHOD_BUF_SIZE) {
        return SC_BUFFER_OVERFLOW_ERR;
    }

    char method_buf[METHOD_BUF_SIZE];
    memcpy(method_buf, header, method_len);
    method_buf[method_len] = '\0';

    // skip extra spaces
    const char *uri_start = space + 1;
    while (*uri_start == ' ') uri_start++;

    // find uri in header
    const char *uri_end = strchr(uri_start, ' ');
    if (!uri_end) {
        return SC_MALFORMED_HEADER_ERR;
    }

    size_t uri_len = uri_end - uri_start;
    if (uri_len == 0 || uri_len > URL_BUF_SIZE) {
        return SC_BUFFER_OVERFLOW_ERR;
    }

    char uri_buf[URL_BUF_SIZE];
    memcpy(uri_buf, uri_start, uri_len);
    uri_buf[uri_len] = '\0';

    http_msg->uri = sc_str_copy_n(uri_buf, uri_len);
    http_msg->method = sc_str_copy_n(method_buf, method_len);

    return SC_OK;
}

static int parse_all_headers(sc_conn_mgr *mgr, sc_conn *conn, sc_headers **headers, sc_http_msg *http_msg, bool *keep_alive) {

    int err;
    // get and parse headers
    // first, get the initial HTTP header (METHOD URI HTTP/VERSION)
    char header_buf[HEADER_BUF_SIZE] = {0};
    err = next_header(mgr, conn->fd, header_buf, HEADER_BUF_SIZE);
    if (err != SC_OK && err != SC_FINISHED) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: Error parsing request line header. Proceeding is impossible. Error code: %d\n", err); // will log on minimal because the error illed the entire request
        cleanup_after_error(mgr, conn);
        return SC_HEADER_PARSE_ERR;
    }

    err = get_http_msg(mgr, header_buf, http_msg);
    if (err != SC_OK) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: error parsing URI and Method from HTTP request line. Proceeding is impossible. Error code: %d\n", err); 
        cleanup_after_error(mgr, conn);
        return SC_HEADER_PARSE_ERR;
    }
    sc_log(mgr, SC_LL_DEBUG, "HTTP MSG: %s, %s\n", http_msg->uri.buf, http_msg->method.buf);

    // now, we parse the missing HTTP headers into sc_headers
    *headers = NULL;
    int error_count = 0;
    while (err != SC_FINISHED) {
        // check if there are happening errors consistently
        if (error_count >= SC_MAX_HEADER_ERROR_COUNT) {
            sc_error_log(mgr,  SC_LL_NORMAL, "[Sculpt] More than %d consecutive errors occoured in header parsing. Interrupting parsing process.\n", SC_MAX_HEADER_ERROR_COUNT);
            return SC_HEADER_PARSE_ERR;
        }

        // get next header
        err = next_header(mgr, conn->fd, header_buf, HEADER_BUF_SIZE);
        if (err != SC_OK && err != SC_FINISHED) {
            sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] Error parsing one of the headers in request, error code: %d\n", err);
            error_count ++;
            continue;
        }
        if (err == SC_FINISHED) {
            continue;
        }

        if (strstr(header_buf, "Connection: keep-alive")) {
            *keep_alive = true;
        }

        // add header to headers list
        *headers = sc_header_append(header_buf, *headers);
        if (headers == NULL) {
            sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] Error appending new header to header list. Headers may be incomplete as a result.");
        }

        error_count = 0;
    }

    return SC_OK;
}

int sc_mgr_poll(sc_conn_mgr *mgr, int timeout_ms) {
    RETURN_ERROR_IF(!mgr, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The mgr pointer cant be null");
    sc_mgr_conns_cleanup(mgr);

    int n = epoll_wait(mgr->epoll_fd, mgr->events, mgr->max_events, timeout_ms);
    if (n == -1) {
        if (errno == EINTR) { // not an error - the system just got interrupted mid syscall
            sc_error_log(mgr, SC_LL_DEBUG, "[Sculpt] Warning - epoll_wait interrupted (errno = EINTR)");
            return SC_OK;
        }
        sc_perror(mgr, SC_LL_DEBUG, "[Sculpt] Error on epoll_wait");
        return SC_EPOLL_WAIT_ERR;
    }

    sc_log(mgr,  SC_LL_DEBUG, "[Sculpt] Connection quantity: %d\n", mgr->conn_count);

    for (int i = 0; i < n; i++) {
       // handle errors with the epoll event
        if (mgr->events[i].events & EPOLLERR) {
            sc_conn *conn = mgr->events[i].data.ptr;
            sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Error with epoll, closing connection...");
            cleanup_after_error(mgr, conn);
            continue;
        }

        if (mgr->events[i].data.fd == mgr->fd) {
            int rc = create_new_connection(mgr);
            if (rc == SC_CONTINUE) continue;
            if (rc != SC_OK) return rc;
        } else {
            // existing connection handling
            sc_conn *conn = mgr->events[i].data.ptr;
            if (!conn) {
                sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: Error gathering connection struct from epoll event");
                continue;
            }
            if (mgr->events[i].events & (EPOLLERR)) {
                sc_perror(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: error with epoll in new event, closing connection");
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                sc_mgr_conn_release(mgr, conn);
                continue;
            }
            if (mgr->events[i].events & (EPOLLHUP)) {
                sc_perror(mgr, SC_LL_DEBUG, "[Sculpt] Client closed its connection.");
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                sc_mgr_conn_release(mgr, conn);
                continue;
            } // will not handle EPOLLRDHUP, as the header parsing function already handles EOF

            if (mgr->events[i].events & EPOLLIN) {
                bool keep_alive = false;
                conn->last_active = time(NULL);
                
                sc_headers *headers = NULL;
                sc_http_msg http_msg;
                int err = parse_all_headers(mgr, conn, &headers, &http_msg, &keep_alive);
                if (err != SC_OK) {
                    sc_headers_free(headers);
                }

                // log request
                sc_log(mgr, SC_LL_NORMAL, "[Sculpt] Request: %s on %s\n", http_msg.method.buf, http_msg.uri.buf);
                struct _endpoint_list *current = mgr->endpoints;
                while (current) {
                    if (current->soft) {
                        // we call it even if just the prefix matches
                        if (sc_strprefix(http_msg.uri, current->val)) {
                            // the uri buffer starts with the prefix of the endpoint
                            current->func(conn->fd, http_msg, headers);
                            sc_str_free(&http_msg.uri);
                            sc_str_free(&http_msg.method);
                            goto end;
                        }
                    } else {
                        if(sc_strcmp(current->val, http_msg.uri) == 0) {
                            // the uri buffer is EQUAL to the endpoint
                            current->func(conn->fd, http_msg, headers);
                            sc_str_free(&http_msg.uri);
                            sc_str_free(&http_msg.method);
                            goto end;
                        }
                    }
                    current = current->next;
                }

                // no valid enpoints were found, so we return 404
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
                    sc_headers_free(headers);
                    if (!keep_alive) {
                        sc_log(mgr, SC_LL_DEBUG, "[Sculpt] Connection close requested\n");
                        close(conn->fd);
                        sc_mgr_conn_release(mgr, conn);
                        epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    } else {
                        // re-add the connection to epoll for further requests
                        struct epoll_event event = {
                            .events = EPOLLIN | EPOLLRDHUP | EPOLLONESHOT,
                            .data.ptr = conn
                        };
                        if (epoll_ctl(mgr->epoll_fd, EPOLL_CTL_MOD, conn->fd, &event) == -1) {
                            sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Failed to re-add connection to epoll");
                            close(conn->fd);
                            sc_mgr_conn_release(mgr, conn);
                            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, &event);
                        }
                    }

                // all other responsibilities are passed to the handler, so no need to do anything else
            } else {
                sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Error reading from client");
            }
        }
    }

    return SC_OK;
}
