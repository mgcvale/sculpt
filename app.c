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
#define BODY_BUF 4096

static bool s_exit_flag = false;

static void signal_handler(int sig) {
    signal(sig, signal_handler);
    s_exit_flag = true;
}

void root_handler(int fd, sc_http_msg msg, sc_headers *headers) {
    char body[BODY_BUF] = "<html><h1>Hello, world!</h1>\0";
    size_t body_size = strlen(body);
    
    sc_headers *current = headers;
    while (current) {
        body_size += current->header.len + 7;
        if (body_size + 8 >= BODY_BUF) break; // 8 for the </html>
        strncat(body, "<p>", 4);
        strncat(body, current->header.buf, current->header.len);
        strncat(body, "</p>", 5);
        current = current->next;
    }
    strncat(body, "</html>", 8);
    
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

    rc = sc_mgr_conn_pool_init(mgr, 1);
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

    sc_mgr_bind_hard(mgr, "/", root_handler);

    sc_mgr_ll_set(mgr, SC_LL_DEBUG);
    sc_mgr_conn_recycling_set(mgr, true);
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (!s_exit_flag) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
