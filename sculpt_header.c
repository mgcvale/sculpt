#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sculpt.h"

static sc_headers *_create_header(const char *header, sc_headers *next) {
    // Allocate memory for the new header
    sc_headers *headers = malloc(sizeof(sc_headers));
    if (headers == NULL) {
        return NULL; // Handle memory allocation failure
    }
    
    // Initialize the header and set next to NULL
    headers->header = strdup(header); // Use strdup to allocate memory for the string
    headers->next = next;

    return headers;
}

sc_headers *sc_header_append(const char *header, sc_headers *list) {
    return _create_header(header, list);
}

void sc_headers_free(sc_headers *headers) {
    while(headers) {
        sc_headers *next = headers->next;
        free((char *) headers->header);
        free(headers);
        headers = next;
    }
}

