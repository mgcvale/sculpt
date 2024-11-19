// Combined source file for sculpt
#include "sculpt.h"

// Start of ../src/sculpt_util.c
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#include "sculpt.h"


const char *http_template = "HTTP/1.1 %d %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: keep-alive\r\n";

// logging
void sc_log(sc_conn_mgr *mgr, int ll, const char *format, ...) {
    // only log if the current log level (ll) is equal or higher than the requested one (level)
    // exapmle: ll = SC_LL_NORMAL (2), level = SC_LL_MINIMAL (1) -> we log;
    // ll = SC_LL_MINIMAL (1), level = SC_LL_NORMAL (2) -> we DON'T log;
    if (ll == SC_LL_NONE || ll < mgr->ll) return;

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

void sc_error_log(sc_conn_mgr *mgr, int ll, const char *format, ...) {
    if (ll == SC_LL_NONE || ll < mgr->ll) return;

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void sc_perror(sc_conn_mgr *mgr, int ll, const char *err) {
    if (ll == SC_LL_NONE || ll < mgr->ll) return;

    perror(err);
}

sc_str sc_str_ref(const char *str) {
    sc_str sc_str = {(char *) str, str == NULL ? 0 : strlen(str)};
    return sc_str;
}

sc_str sc_str_ref_n(const char *str, size_t len) {
    sc_str sc_str = {(char *) str, len};
    return sc_str;
}

sc_str sc_str_copy(const char *str) {
    size_t buf_size = strlen(str);
    return sc_str_copy_n(str, buf_size);
}

sc_str sc_str_copy_n(const char *str, size_t len) {
    char *buf = malloc(len + 1);
    memcpy(buf, str, len);
    buf[len] = '\0';
    sc_str sc_str = {buf, len};
    return sc_str;
}

void sc_str_free(sc_str *str) {
    if (str->buf != NULL) {
        free(str->buf);
    }
}

int sc_strcmp(const sc_str str1, const sc_str str2) {
    size_t i;
    for (i = 0; i < str1.len && i < str2.len; i++) {
      int ch1 = str1.buf[i];
      int ch2 = str2.buf[i];
      if (ch1 < ch2) return -1;
      if (ch1 > ch2) return 1;
    }
    if (i < str1.len) return 1;
    if (i < str2.len) return -1;
    return 0;
}

bool sc_strprefix(const sc_str str, const sc_str prefix) {
    if (str.len < prefix.len) {
        return false; // false if str is shorter than prefix
    }

    return memcmp(str.buf, prefix.buf, prefix.len) == 0;
}

char *sc_easy_request_build(int code, const char *code_str, const char *body, sc_headers *headers) {
    char *request;
    size_t response_len = strlen(http_template) + strlen(code_str) + 3 + 16 + 4; 
    // 3 for the response code (200, 404, 403, etc), 16 for the content-length and 4 for the \r\n\r\n
    
    sc_headers *current = headers;
    while (current) {
        // assuming the header_len includes the \r\n, as it should
        response_len += current->header.len + 1; 
        current = current->next;
    }

    size_t body_len = strlen(body);
    response_len += body_len;

    char *response = malloc(response_len + 1);
    
    if (response == NULL) {
        return NULL;
    }

    snprintf(response, response_len, http_template, 
            code, code_str, body_len
    );

    current = headers;
    while (current) {
        // assuming the header ends with \r\n, as it should
        strncat(response, current->header.buf, current->header.len);
        current = current->next;
    }

    strcat(response, "\r\n");
    strncat(response, body, body_len);

    return response;
}

int sc_easy_send(int fd, int code, const char *code_str, const char *content_type, const char *body, sc_headers *headers) {

    headers = sc_header_append(content_type, headers);
    char *response = sc_easy_request_build(code, code_str, body, headers);

    if (headers == NULL) {
        return SC_MALLOC_ERR;
    }

    if (send(fd, response, strlen(response), 0) == -1) {
       return SC_SEND_ERR;
    }

    free(response);
    sc_headers_free(headers);

    return SC_OK;
}

// End of ../src/sculpt_util.c

// Start of ../src/sculpt_header.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sculpt.h"

static sc_headers *_create_header(const char *header, sc_headers *next) {
    sc_headers *headers = malloc(sizeof(sc_headers));
    if (headers == NULL) {
        return NULL;
    }
    
    headers->header = sc_str_copy(header);
    if (headers->header.buf == NULL) {
        sc_headers_free(headers);
        return NULL;
    }

    headers->next = next;

    return headers;
}

sc_headers *sc_header_append(const char *header, sc_headers *list) {
    sc_headers *res;
    if (!strstr(header, "\r\n")) {
        size_t len = strlen(header) + 3;
        char *h = malloc(len);
        if (h == NULL) {
            return NULL;
        }
        h[len - 1] = '\0';
        snprintf(h, len, "%s\r\n", header); 
        res = _create_header(h, list);
        free(h);
    } else {
        res = _create_header(header, list);
    }
    return res;
}

void sc_headers_free(sc_headers *headers) {
    printf("Freeing headers\n");
    while(headers != NULL) {
        sc_headers *next = headers->next;
        sc_str_free(&headers->header);
        free(headers);
        headers = next;
    }
}

void sc_header_free(sc_headers *header) {
    if (header != NULL) {
        sc_str_free(&header->header);
        free(header);
    }
}

// End of ../src/sculpt_header.c

// Start of ../src/sculpt_conn.c
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
    fprintf(stdout, "[Sculpt] Creating new connection\n");
    socklen_t addr_len = sizeof(mgr->addr_info._sock_addr);

    // new connection, check capacity before proceeding
    if (mgr->conn_count >= mgr->max_conn_count) {
        perror("[Sculpt] No avaliable connections found! Sending 503 response");
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
        perror("[Sculpt] Error on Accept. Checking severity\n");
        sc_mgr_conn_release(mgr, conn);
        if (errno != EAGAIN && errno != EWOULDBLOCK) { // if the error is not because it would block or cuz it is unacailable, we don't return the function
            fprintf(stderr, "[Sculpt] Fatal: Accept error: %d\n", errno);
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
        perror("[Sculpt] Failed to add connection to epoll");
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

int next_header(int fd, char *header, size_t buf_len) {
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
            perror("[Sculpt] Error reading body from client");
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

int get_http_msg(char *header, sc_http_msg *http_msg) {
    if (http_msg == NULL) {
        return SC_BAD_ARGUMENTS_ERR;
    }
    if (header == NULL) {
        return SC_BAD_ARGUMENTS_ERR;
    }    
    
    const char *space = strchr(header, ' ');
    if (space == NULL) {
        return SC_MALFORMED_HEADER_ERR;
    }
    
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

    printf("Result: URI: %s, Method: %s\n", http_msg->uri.buf, http_msg->method.buf);

    return SC_OK;
}

static int parse_all_headers(sc_conn_mgr *mgr, sc_conn *conn, sc_headers **headers, sc_http_msg *http_msg, bool *keep_alive) {

    int err;
    // get and parse headers
    // first, get the initial HTTP header (METHOD URI HTTP/VERSION)
    char header_buf[HEADER_BUF_SIZE] = {0};
    err = next_header(conn->fd, header_buf, HEADER_BUF_SIZE);
    if (err != SC_OK && err != SC_FINISHED) {
        fprintf(stderr, "[Sculpt] Critical: Error parsing request line header. Proceeding is impossible. Error code: %d\n", err);
        cleanup_after_error(mgr, conn);
        return SC_HEADER_PARSE_ERR;
    }

    err = get_http_msg(header_buf, http_msg);
    if (err != SC_OK) {
        fprintf(stderr, "[Sculpt] Critical: error parsing URI and Method from HTTP request line. Proceeding is impossible. Error code: %d\n", err); 
        cleanup_after_error(mgr, conn);
        return SC_HEADER_PARSE_ERR;
    }
    printf("HTTP MSG: %s, %s\n", http_msg->uri.buf, http_msg->method.buf);

    // now, we parse the missing HTTP headers into sc_headers
    *headers = NULL;
    int error_count = 0;
    while (err != SC_FINISHED) {
        // check if there are happening errors consistently
        if (error_count >= SC_MAX_HEADER_ERROR_COUNT) {
            fprintf(stderr, "[Sculpt] More than %d consecutive errors occoured in header parsing. Interrupting parsing process.\n", SC_MAX_HEADER_ERROR_COUNT);
            return SC_HEADER_PARSE_ERR;
        }

        // get next header
        err = next_header(conn->fd, header_buf, HEADER_BUF_SIZE);
        if (err != SC_OK && err != SC_FINISHED) {
            fprintf(stderr, "[Sculpt] Error parsing one of the headers in request, error code: %d\n", err);
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
            fprintf(stderr, "[Sculpt] Error appending new header to header list. Headers may be incomplete as a result.");
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
            printf("[Sculpt] Warning - epoll_wait interrupted (errno = EINTR)");
            return SC_OK;
        }
        perror("[Sculpt] Error no epoll_wait");
        return SC_EPOLL_WAIT_ERR;
    }
    printf("[Sculpt] Connection quantity: %d\n", mgr->conn_count);

    for (int i = 0; i < n; i++) {
       // handle errors with the epoll event
        if (mgr->events[i].events & EPOLLERR) {
            sc_conn *conn = mgr->events[i].data.ptr;
            perror("[Sculpt] Error with epoll, closing connection...");
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
                perror("[Sculpt] Critical: Error gathering connection struct from epoll event");
                continue;
            }
            if (mgr->events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                close(conn->fd);
                sc_mgr_conn_release(mgr, conn);
                continue;
            }

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
                printf("[Sculpt] Request: %s on %s\n", http_msg.method.buf, http_msg.uri.buf);
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
                    perror("[Sculpt] Error sending response");
                }

                end:
                    sc_headers_free(headers);
                    if (!keep_alive) {
                        printf("[Sculpt] Connection close requested\n");
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
                            perror("[Sculpt] Failed to re-add connection to epoll");
                            close(conn->fd);
                            sc_mgr_conn_release(mgr, conn);
                            epoll_ctl(mgr->epoll_fd, EPOLL_CTL_DEL, conn->fd, &event);
                        }
                    }

                // all other responsibilities are passed to the handler, so no need to do anything else
            } else {
                perror("[Sculpt] Error reading from client");
            }
        }
    }

    return SC_OK;
}

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

// End of ../src/sculpt_conn.c

// Start of ../src/sculpt_mgr.c
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

    mgr->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mgr->fd < 0) {
        perror("[Sculpt] Error: error creating socket for conn_mgr");
        free(mgr);
        *err = SC_SOCKET_CREATION_ERR;
        return NULL;
    }
    
    int opt = 1;
    if (setsockopt(mgr->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
        perror("[Sculpt] Error: failed to set socket options");
        *err = SC_SOCKET_SETOPT_ERR;
        goto error;
    }

    if (bind(mgr->fd, (struct sockaddr *)&mgr->addr_info, sizeof(mgr->addr_info))) {
        perror("[Sculpt] Error: Failed to bind server to the address");
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
        fprintf(stderr, "[Sculpt] Warning: %s; ", gai_strerror(rc));
        fprintf(stderr, "Server is listening on unknown URL\n");
        return SC_SOCKET_GETNAMEINFO_ERR;
    }

    printf("\n[Sculpt] Server is listening on http://%s%s:%d\n", mgr->host_buf, mgr->service_buf, mgr->addr_info.port);

    mgr->listening = true;
    return SC_OK;
}

void sc_mgr_finish(sc_conn_mgr *mgr) {
    if (!mgr) {
        return;
    }
    int ll = mgr->ll;

    sc_mgr_conn_pool_destroy(mgr);
    if (ll == SC_LL_DEBUG) {
        printf("[Sculpt]freed conn pool\n");
    }

    // close epoll fd and free events array
    if (mgr->epoll_fd >= 0) {
        close(mgr->epoll_fd);
        mgr->epoll_fd = -1;
    }
    free(mgr->events);
    mgr->events = NULL; // !! dangling pointers

    printf("[Sculpt]freed epoll\n");

    // close server socket
    if (mgr->fd >= 0) {
        close(mgr->fd);
        mgr->fd = -1;
    }
    if (ll == SC_LL_DEBUG) {
        printf("[Sculpt]freed server socket\n");
     }

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
    if (mgr->ll == SC_LL_DEBUG) {
        printf("[Sculpt]Endpoint added: %s", mgr->endpoints->val.buf);
    }
    if (mgr->endpoints == NULL) {
        return SC_MALLOC_ERR;
    }
    return SC_OK;
}

// End of ../src/sculpt_mgr.c

