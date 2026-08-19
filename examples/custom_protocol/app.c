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

typedef struct custom_protocol_data {
    char uri[URI_BUF];
    char method;
    int content_len;
    char connection_state;
    char auth[AUTH_BUF];
} protocol_data;

static bool s_exit_flag = false;

static void signal_handler(int sig) {
    signal(sig, signal_handler);
    s_exit_flag = true;
}

void protocol_fallback(sc_conn_mgr *mgr, sc_conn *conn, sc_http_msg msg, sc_headers *headers, void *extra_data, int err) {
    if (err == SC_CONN_CLOSE) { // if the handler requests a connection close, we do so
        sc_mgr_conn_release(mgr, conn);
        return;
    }

    sc_error_log(mgr, SC_LL_MINIMAL, "Error code: %d\n", err);
    const char *response = "50000000021|Internal Server Error";
    send(conn->fd, response, strlen(response), 0);
    if (conn->persistent) {
        sc_mgr_conn_readd(mgr, conn);
    } else {
        sc_mgr_conn_release(mgr, conn);
    }
}

int protocol_handler(sc_conn *conn, sc_http_msg *msg, sc_headers **headers, void **extra_data) {
    *extra_data = malloc(sizeof(protocol_data));
    if (*extra_data == NULL) {
        return SC_MALLOC_ERR;
    }

    protocol_data *p_data = (protocol_data*) *extra_data;  

    size_t uri_buf_size = 0;
    size_t auth_buf_size = 0;

    // parse URI
    while (1) {
        if (uri_buf_size > URI_BUF - 1) {
            free(*extra_data);
            return SC_BUFFER_OVERFLOW_ERR;
        }

        ssize_t bytes_read = read(conn->fd, &p_data->uri[uri_buf_size], 1);
        if (bytes_read > 0) {
            if (p_data->uri[uri_buf_size] == '|') {
                // we got to the end of the uri
                p_data->uri[uri_buf_size] = '\0';
                break;
            }
            uri_buf_size++;
        } else if (bytes_read == 0) {
            printf("[OPSC protocol] Connection sent 0 bytes, closing it\n");
            free(*extra_data);
            return SC_CONN_CLOSE; // signal to close the connection
        } else {
            p_data->uri[uri_buf_size+1] = '\0';
            printf("Error reading URI; URI read so far: %s\n", p_data->uri);
            free(*extra_data);
            return SC_READ_ERR;
        }
    }

    // parse method
    ssize_t bytes_read = read(conn->fd, &p_data->method, 1);
    if (bytes_read == 0) {
        printf("[OPSC protocol] Connection sent 0 bytes, closing it\n");
        free(*extra_data);
        return SC_CONN_CLOSE;
    } else if (bytes_read != 1) {
        free(*extra_data);
        return SC_READ_ERR;
    } 

    // parse content-length
    char content_len_buf[8];

    bytes_read = read(conn->fd, content_len_buf, 8);
    if (bytes_read == 0) {
        printf("[OPSC protocol] Connection sent 0 bytes, closing it\n");       
        free(*extra_data);
        return SC_CONN_CLOSE;
    } else if (bytes_read != 8) {
        free(*extra_data);
        return SC_MALFORMED_HEADER_ERR;
    }
    p_data->content_len = atoi(content_len_buf);

    // parses connection state
    bytes_read = read(conn->fd, &p_data->connection_state, 1);
    if (bytes_read == 0) {
        printf("[OPSC protocol] Connection sent 0 bytes, sending close signal\n");  
        free(*extra_data);
        return SC_CONN_CLOSE;
    } else if (bytes_read != 1) {
        free(*extra_data);
        return SC_READ_ERR;
    }

    conn->persistent = true;
    if (p_data->connection_state == 'C' || p_data->connection_state == 'c') {
        conn->persistent = false;
    }

    // read divisor (|)

    char buffer[1];
    read(conn->fd, buffer, 1);
    printf("Throwing out %c\n", buffer[0]);


    msg->method = sc_str_copy_n(&p_data->method, 1);
    msg->uri = sc_str_copy_n(p_data->uri, strlen(p_data->uri));
    msg->version = sc_str_copy_n("-1", 2);

    return SC_OK;
}

void root_handler(int fd, sc_http_msg msg, sc_headers *headers, void *extra_data) {
    protocol_data *p_data = (protocol_data*) extra_data;
    size_t response_len = p_data->content_len + strlen(p_data->uri) + 10 + 12 + 200;
    if (response_len >= BODY_BUF) {
       send(fd, "40000000011|Bad Request", 23, 0);
       return;
    }
    
    char response[BODY_BUF];
    char body[BODY_BUF];
    ssize_t bytes_read = read(fd, body, p_data->content_len + 1);
    if (bytes_read <= 0) {
        printf("Bytes read: %lo\n", bytes_read);
        const char *response = "50000000021|Internal Server Error";
        send(fd, response, strlen(response), 0);
        return;
    }

    // flush the fd for good measure
    char buffer[1024];
    while ((bytes_read = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
        // Discard data by doing nothing with `buffer`
    }


    body[bytes_read] = '\0';
    snprintf(response, BODY_BUF, "200%08d|body:%s;uri:%s;method:%c;content-len:%d;conn-state:%c", 321, body, p_data->uri, p_data->method, p_data->content_len, p_data->connection_state);
    printf("Sending response: %s\n", response);
    send(fd, response, strlen(response), 0);
}

int main() {    
    // create and setup socket    
    int error;
    sc_conn_mgr *mgr = sc_mgr_create(&error);
    if (mgr == NULL) {
        fprintf(stderr, "Error on create: %d", error);
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

    sc_mgr_ll_set(mgr, SC_LL_DEBUG);
    sc_mgr_conn_recycling_set(mgr, true);
    mgr->protocol = SC_PROTOCOL_CUSTOM;
    mgr->protocol_handler = protocol_handler;
    mgr->protocol_fallback = protocol_fallback;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    while (!s_exit_flag) {
        sc_mgr_poll(mgr, 1000);
    }

    sc_mgr_finish(mgr);

    return 0;
}
