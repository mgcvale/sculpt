#include <stdio.h>
#include <stdlib.h>
#include "cast.h"

static cst_headers *_create_header(const char *header, cst_headers *next) {
    // Allocate memory for the new header
    cst_headers *headers = malloc(sizeof(cst_headers));
    if (headers == NULL) {
        return NULL; // Handle memory allocation failure
    }
    
    // Initialize the header and set next to NULL
    headers->header = strdup(header); // Use strdup to allocate memory for the string
    headers->next = next;

    return headers;
}

cst_headers *cst_header_append(const char *header, cst_headers *list) {
    return _create_header(header, list);
}

void cst_headers_free(cst_headers *headers) {
    while(headers) {
        cst_headers *next = headers->next;
        free((char *) headers->header);
        free(headers);
        headers = next;
    }
}

