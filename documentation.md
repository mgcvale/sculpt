# Sculpt docs

This is the documentation for the Sculpt framework. Here you will learn all about it, from creating simple apps to more complex ones, _sculpted_ to your specific needs.

## Quickstart

Here is a simple, sample 'Hello, world!" application made using the sculpt framework.

```
#include "sculpt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8000
#define MAX_CONNECTIONS 8

// handler for requests to the root endpoint ("/")
// responds with a default JSON message: {"message": "Hello, World!"}
void root_handler(int fd, sc_http_msg msg) {
    const char *body = "{\"message\": \"Hello, World!\"}";
    
    // Send a JSON response with status 200 OK
    if (sc_easy_send(fd, 200, "OK", "Content-Type: application/json", body, NULL) == SC_OK) {
        printf("Response sent successfully\n");
    } else {
        perror("Error sending response");
    }
}

int main() {
    // Step 1: create address info for the server
    // AF_INET is used for IPv4, and the server will listen on PORT 8000.
    sc_addr_info addr_info = sc_addr_create(AF_INET, PORT); 
    
    // Step 2: initialize connection manager with address info
    int error_code = 0;
    sc_conn_mgr *mgr = sc_mgr_create(addr_info, &error_code);
    if (mgr == NULL) { 
        fprintf(stderr, "Error creating Sculpt manager: %d\n", error_code);
        exit(EXIT_FAILURE);
    }

    // Step 3: initialize Sculpt components (epoll and connection pool)
    if (sc_mgr_epoll_init(mgr) != SC_OK) {
        fprintf(stderr, "Error initializing epoll\n");
        sc_mgr_finish(mgr); // Cleanup before exit
        exit(EXIT_FAILURE);
    }

    if (sc_mgr_conn_pool_init(mgr, MAX_CONNECTIONS) != SC_OK) {
        fprintf(stderr, "Error initializing connection pool (max connections: %d)\n", MAX_CONNECTIONS);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }
    
    // Step 4: set up server to listen on the specified port
    if (sc_mgr_listen(mgr) != SC_OK) {
        fprintf(stderr, "Error starting server on port %d\n", PORT);
        sc_mgr_finish(mgr);
        exit(EXIT_FAILURE);
    }

    // Step 5: bind the root URI ("/") to the root_handler function
    // using soft binding allows it to match any URI that starts with "/".
    // if we were to use hard binding, it would only match the "/" uri.
    sc_mgr_bind_soft(mgr, "/", root_handler);
    
    // Step 6: poll the connection manager indefinitely with a timeout of 1000ms
    // this listens for incoming connections and handles requests.
    for (;;) {
        sc_mgr_poll(mgr, 1000); // Polls every 1000 ms
    }

    // cleanup (unreachable code, would require signal handling to be reached)
    sc_mgr_finish(mgr); // free allocated resources
    return 0;
}

```

