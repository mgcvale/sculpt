#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>

#include "sculpt.h"


const char *http_template =
    "HTTP/1.1 %d %s\r\n"
    "Content-Length: %zu\r\n"
    "Connection: keep-alive\r\n";


sc_str sc_mk_str(const char *str) {
    sc_str sc_str = {(char *) str, str == NULL ? 0 : strlen(str)};
    return sc_str;
}

sc_str sc_mk_str_n(const char *str, size_t len) {
    sc_str sc_str = {(char *) str, len};
    return sc_str;
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

struct _endpoint_list *_endpoint_add(struct _endpoint_list *list, const char *endpoint, bool soft, void (*func)(int, sc_http_msg)) {
    struct _endpoint_list *new = malloc(sizeof(struct _endpoint_list));
    if (new == NULL) {
        return NULL;
    }

    new->soft = soft;
    new->func = func;
    sc_str val = sc_mk_str_n(endpoint, strlen(endpoint));
    new->val = val;
    new->next = list;
    return new;
}

char *sc_easy_request_build(int code, const char *code_str, const char *body, sc_headers *headers) {
    char *request;
    size_t response_len = strlen(http_template) + strlen(code_str) + 3 + 16 + 4; 
    // 3 for the response code (200, 404, 403, etc), 16 for the content-length and 4 for the \r\n\r\n
    
    sc_headers *current = headers;
    while (current) {
        // assuming the header_len includes the \r\n, as it should
        response_len += current->header_len + 1; 
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
        strncat(response, current->header, strlen(current->header));
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

    sc_headers *added_header = headers;
    headers = headers->next;
    free(added_header->header);
    free(added_header);


    if (send(fd, response, strlen(response), 0) == -1) {
       return SC_SEND_ERR;
    }

    free(response);

    return SC_OK;
}
