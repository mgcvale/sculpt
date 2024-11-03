#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "sculpt.h"

#define PORT 8000
#define BACKLOG 128

int main() {    
    // create and setup socket    
    sc_addr_info addr_info = sc_addr_create(AF_INET, 8000);

    int error;
    sc_conn_mgr *mgr = sc_mgr_create(addr_info, &error);
    if (mgr == NULL) {
        fprintf(stderr, "Error on create: %d", error);
        exit(EXIT_FAILURE);
    }
    
    error = sc_mgr_listen(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error on listen: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    int rc = sc_mgr_epoll_init(mgr);
    if (rc != SC_OK) {
        fprintf(stderr, "Error initializing epoll: %d", rc);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    rc = sc_mgr_pool_init(mgr, 1);
    if (rc != SC_OK) {
        fprintf(stderr, "Error initializing connection pool: %d", rc);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    for (;;) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
