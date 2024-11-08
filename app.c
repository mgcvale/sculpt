#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/signal.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "src/sculpt.h"

#define PORT 8000
#define BACKLOG 128

static bool s_exit_flag = false;

static void signal_handler(int sig) {
    signal(sig, signal_handler);
    s_exit_flag = true;
}

void root_handler(int fd, sc_http_msg msg) {
    const char *body = "Hello, world";
    if (sc_easy_send(fd, 200, "OK", "Content-Type: text/html", body, NULL) == SC_OK) {
        printf("Response sent\n");
    } else {
        perror("Error sending response");
    }
}

int main() {    
    // create and setup socket    
    sc_addr_info addr_info = sc_addr_create(AF_INET, 8000);

    int error;
    sc_conn_mgr *mgr = sc_mgr_create(addr_info, &error);
    if (mgr == NULL) {
        fprintf(stderr, "Error on create: %d", error);
        exit(EXIT_FAILURE);
    }
    
    int rc = sc_mgr_epoll_init(mgr);
    if (rc != SC_OK) {
        fprintf(stderr, "Error initializing epoll: %d", rc);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    rc = sc_mgr_pool_init(mgr, 20);
    if (rc != SC_OK) {
        fprintf(stderr, "Error initializing connection pool: %d", rc);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    error = sc_mgr_listen(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error on listen: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    sc_mgr_bind_hard(mgr, "/root", root_handler);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (!s_exit_flag) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
