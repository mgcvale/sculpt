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
    // 
    if (ll == SC_LL_NONE || ll > mgr->ll) return;

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

    printf("[Sculpt] sending request with sc_easy_send: %s", response);
    if (send(fd, response, strlen(response), 0) == -1) {
       return SC_SEND_ERR;
    }

    free(response);
    sc_headers_free(headers);
    return SC_OK;
}
