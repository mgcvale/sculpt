#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/signal.h>

#include <sys/types.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include "sculpt.h"

#define PORT 8000
#define BACKLOG 128
#define BODY_BUF 4096
#define URI_BUF 128
#define AUTH_BUF 128

static bool s_exit_flag = false;

static void signal_handler(int sig) {
    signal(sig, signal_handler);
    s_exit_flag = true;
}

void root_handler(int fd, sc_http_msg msg, sc_headers *headers, void *extra_data) {
    if (strncmp(msg.method.buf, "OPTIONS", strlen("OPTIONS")) == 0) {
        const char *cors_msg = 
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: *\r\n"
            "Access-Control-Allow-Headers: *\r\n"
            "Access-Control-Max-Age: 86400\r\n"
            "Content-Length: 0\r\n"
            "\r\n";
        send(fd, cors_msg, strlen(cors_msg), 0);
        printf("Sent CORS authorization response");
    }

    char body[BODY_BUF] = "<html><h1>Hello, world! Your request:</h1>\0";
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

    sc_headers *response_headers = NULL;
    response_headers = sc_header_append("Access-Control-Allow-Origin: *", response_headers);
    
    if (sc_easy_send(fd, 200, "OK", "Content-Type: text/html", body, response_headers) == SC_OK) {
        printf("Response sent\n");
    } else {
        perror("Error sending response");
    }
}

int main() {    
    // create and setup socket    
    sc_addr_info addr_info = sc_addr_create(AF_INET, 8003, INADDR_ANY);

    int error;
    sc_conn_mgr *mgr = sc_mgr_create(addr_info, &error);
    if (mgr == NULL) {
        fprintf(stderr, "Error on create: %d", error);
        exit(EXIT_FAILURE);
    }

    error = sc_mgr_config_auto_apply(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error applying sculpt config: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }
    
    error = sc_mgr_epoll_init(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error initializing epoll: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    error = sc_mgr_conn_pool_init(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error initializing connection pool: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    error = sc_mgr_listen(mgr);
    if (error != SC_OK) {
        fprintf(stderr, "Error on listen: %d", error);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    sc_mgr_bind_soft(mgr, "/", root_handler);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    sc_mgr_state_log(mgr, stdout);

    while (!s_exit_flag) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
