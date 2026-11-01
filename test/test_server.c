#include "sculpt.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static bool s_exit_flag = false;

static void signal_handler(int sig) {
    signal(sig, signal_handler);
    s_exit_flag = true;
}


void generic_handler(int fd, sc_http_msg msg, sc_headers *headers, void *extra_data) {

    char *body = "Sculpt Base Response";
    size_t body_size = strlen(body);

    int err = sc_easy_send(fd, 200, "OK", "Content-type: plain/txt", body, NULL);
    if (err) {
        fprintf(stderr, "Error sending response: %d", err);
    }
}

int main(int argc, char **argv) {
    if (argc - 1 != 1) {
        fprintf(stderr, "Error: exactly one argument is necessary to run the test server: config_path. Got %d arguments.\n", argc - 1);
        exit(-1);
    }

    int err;
    sc_conn_mgr *mgr = sc_mgr_create(&err);
    if (err != SC_OK) {
        fprintf(stderr, "Error %d when creating sculpt mgr; aborting\n", err);
        exit(err);
    }

    err = sc_mgr_config_apply(mgr, argv[1]);
    if (err != SC_OK) {
        fprintf(stderr, "Error %d when loading sculpt config. Does the provided config exist? aborting.\n", err);
        sc_mgr_finish(mgr);
        exit(err);
    }

    err = sc_mgr_epoll_init(mgr);
    if (err != SC_OK) {
        fprintf(stderr, "Error initializing epoll: %d", err);
        sc_mgr_finish(mgr);
        exit(err);
    }

    err = sc_mgr_conn_pool_init(mgr);
    if (err != SC_OK) {
        fprintf(stderr, "Error initializing connection pool: %d", err);
        sc_mgr_finish(mgr);
        exit(err);
    }

    err = sc_mgr_listen(mgr);
    if (err != SC_OK) {
        fprintf(stderr, "Error on listen: %d", err);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    sc_mgr_bind_soft(mgr, "/", generic_handler);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    sc_mgr_state_log(mgr, stdout);

    while (!s_exit_flag) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
