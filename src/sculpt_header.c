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
    while(headers) {
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
