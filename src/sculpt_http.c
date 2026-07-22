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

// here, I will read the headers byte by byte.
// This is to avoid reading into the body of the request - as this will be something that the user will handle, not us.
// Because the headers should be relatively small in a typical HTTP request, this shouldn't present a performance issue to the program.
int next_header(sc_conn_mgr *mgr, int fd, char *header, size_t buf_len) {
    header[0] = '\0';
    size_t header_len = 0;
    char last_char = '\0';

    while (1) {
        // stop if the header is larger than the buffer
        // only checking header_len > buf_len could lead to a two-byte overflow, since it reads into header[buf_len] and writes `\0` at buf_len + 1
        if (header_len >= buf_len - 1) {
            return SC_BUFFER_OVERFLOW_ERR;
        }

        ssize_t bytes_read = read(fd, &header[header_len], 1);
        if (bytes_read > 0) {
            if (last_char == '\r' && header[header_len] == '\n') {
                // we got to the end of the header
                header[header_len - 1] = '\0';
                header_len--;
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
            sc_perror(mgr, SC_LL_NORMAL, "[Sculpt] Error reading header from client\n");
            return SC_READ_ERR;

        } else if (bytes_read == 0) {
            // EOF e or client closed the connection
            if (header_len > 0) {
                // we still have some header data read, but EOF is reached
                return SC_OK; 
            }
            return SC_CONN_CLOSED;
        }
    }

    if (header[0] == '\0') { // the header is empty, that is, we have reached the end of the headers
        return SC_FINISHED;
    }

    return SC_OK;
}

int get_http_msg(sc_conn_mgr *mgr, char *header, sc_http_msg *http_msg) {
    RETURN_ERROR_IF(!http_msg, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The http_msg pointer can't be null\n");
    RETURN_ERROR_IF(!header, SC_BAD_ARGUMENTS_ERR, "[Sculpt] The sc_header pointer can't be null\n");
 
    sc_log(mgr, SC_LL_DEBUG, "Getting http msg on header: %s\n", header);

    char *space = strchr(header, ' ');
    if (space == NULL) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it was missing a space character\n");
        return SC_MALFORMED_HEADER_ERR;
    }

    // find method in header
    size_t method_len = space - header;
    if (method_len == 0 || method_len >= METHOD_BUF_SIZE) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it had a method that was too long\n");
        return SC_BUFFER_OVERFLOW_ERR;
    }

    char method_buf[METHOD_BUF_SIZE];
    memcpy(method_buf, header, method_len);
    method_buf[method_len] = '\0';

    // skip extra spaces
    const char *uri_start = space + 1;
    while (*uri_start == ' ') uri_start++;

    // find uri in header
    space = strchr(uri_start, ' ');
    if (!space) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it was missing a space character\n");
        return SC_MALFORMED_HEADER_ERR;
    }

    size_t uri_len = space - uri_start;
    if (uri_len == 0 || uri_len >= URL_BUF_SIZE) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it had a uri that exceeded the max buffer size\n");
        return SC_BUFFER_OVERFLOW_ERR;
    }

    char uri_buf[URL_BUF_SIZE];
    memcpy(uri_buf, uri_start, uri_len);
    uri_buf[uri_len] = '\0';

    // skip extra spaces
    const char *version_start = space + 1;
    while (*version_start == ' ') version_start++;

    // find version in header
    const char *end = strchr(version_start, '\0');
    if (!end) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it was missing a newline character\n");
        return SC_MALFORMED_HEADER_ERR;
    }

    size_t version_len = end - version_start;
    if (version_len == 0 || version_len >= VERSION_BUF_SIZE) {
        sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] The header passed to get_http_msg was malformed, as it had a uri that exceeded the max buffer size\n");
        return SC_BUFFER_OVERFLOW_ERR;
    }

    char version_buf[VERSION_BUF_SIZE];
    memcpy(version_buf, version_start, version_len);
    version_buf[version_len] = '\0';

    http_msg->uri = sc_str_copy_n(uri_buf, uri_len);
    http_msg->method = sc_str_copy_n(method_buf, method_len);
    http_msg->version = sc_str_copy_n(version_buf, version_len);

    return SC_OK;
}

void sc_http_msg_free(sc_http_msg *msg) {
    if (!msg) {
        return;
    }
    sc_str_free(&msg->uri);
    sc_str_free(&msg->version);
    sc_str_free(&msg->method);
}



int parse_all_headers(sc_conn_mgr *mgr, sc_conn *conn, sc_headers **headers, sc_http_msg *http_msg) {
    int err;

    // get and parse headers
    // first, get the initial HTTP header (METHOD URI HTTP/VERSION)
    char header_buf[HEADER_BUF_SIZE] = {0};
    err = next_header(mgr, conn->fd, header_buf, HEADER_BUF_SIZE);
    if (err != SC_OK && err != SC_FINISHED) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: Error parsing request line header. Proceeding is impossible. Error code: %d\n", err); // will log on minimal because the error killed the entire request
        return err;
    }

    err = get_http_msg(mgr, header_buf, http_msg);
    if (err != SC_OK) {
        sc_error_log(mgr, SC_LL_MINIMAL, "[Sculpt] Critical: error parsing URI and Method from HTTP request line. Proceeding is impossible. Returning error 400. Error code: %d\n", err);
        return err;
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
            sc_log(mgr, SC_LL_DEBUG, "Found keep-alive header\n");
            conn->persistent = true;
        }

        // add header to headers list
        *headers = sc_header_append(header_buf, *headers);
        if (headers == NULL) {
            sc_error_log(mgr, SC_LL_NORMAL, "[Sculpt] Error appending new header to header list. Headers may be incomplete as a result.\n");
        }

        error_count = 0;
    }

    return SC_OK;
}