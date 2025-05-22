// filepath: /Users/emre/Projects/load-balancer/server.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <math.h>

#define DEFAULT_PORT 9001
#define RESPONSE_SIZE 1024

// Global variables
int server_fd;
volatile sig_atomic_t running = 1;
int server_id = 0;  // Global server ID for logging

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        printf("[server-%d] Received termination signal, shutting down...\n", server_id);
        running = 0;
    }
}

// Initialize server socket
bool initialize_server(int port) {
    struct sockaddr_in address;
    int opt = 1;
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[server] Socket creation failed");
        return false;
    }
    
    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("[server] Setsockopt failed");
        return false;
    }
    
    // Configure address
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    
    // Find an available port if requested port is 0 or binding fails
    if (port == 0 || port == DEFAULT_PORT) {
        // Try to find an available port in range 9001-9100
        for (int p = 9001; p < 9100; p++) {
            address.sin_port = htons(p);
            if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) >= 0) {
                port = p;
                break;
            }
        }
    } else {
        address.sin_port = htons(port);
        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("[server] Bind failed");
            close(server_fd);
            return false;
        }
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("[server] Listen failed");
        return false;
    }
    
    // Report the port to watchdog if WATCHDOG_PIPE_FD is set
    char *pipe_fd_str = getenv("WATCHDOG_PIPE_FD");
    if (pipe_fd_str != NULL) {
        int pipe_fd = atoi(pipe_fd_str);
        if (pipe_fd > 0) {
            char port_str[16];
            snprintf(port_str, sizeof(port_str), "%d", port);
            write(pipe_fd, port_str, strlen(port_str));
        }
    }
    
    return true;
}

// This section has been removed since square root calculation is now done directly in handle_connection

// Handle a client connection
void handle_connection(int client_socket) {
    char buffer[1024] = {0};
    int bytes_read;
    
    // Read client request - expect a float value
    bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read <= 0) {
        printf("[server] Error reading from client or connection closed\n");
        close(client_socket);
        return;
    }
    
    // Ensure null termination
    buffer[bytes_read] = '\0';
    
    // Parse the float
    double input_value = atof(buffer);
    
    // Calculate square root and send response
    char response[RESPONSE_SIZE];
    double sqrt_result = sqrt(input_value);
    snprintf(response, RESPONSE_SIZE, "%.6f", sqrt_result);
    printf("[server-%d] Processing request for input: %f, result: %s\n", server_id, input_value, response);
    
    // Send response
    write(client_socket, response, strlen(response));
    
    // Close connection
    close(client_socket);
}

// Clean up resources
void cleanup() {
    if (server_fd >= 0) {
        close(server_fd);
    }
}

int main(int argc, char *argv[]) {
    int port = 0;  // 0 means find an available port
    
    // Parse command line arguments
    // Usage: ./server [id] [port]
    if (argc > 1) {
        server_id = atoi(argv[1]);
        if (server_id < 1) {
            printf("[server] Invalid server ID %s, using default 0\n", argv[1]);
            server_id = 0;
        }
    }
    
    if (argc > 2) {
        port = atoi(argv[2]);
        if (port < 0 || port > 65535) {
            printf("[server-%d] Invalid port number %s, using auto-discovery\n", server_id, argv[2]);
            port = 0;
        }
    }
    
    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    
    printf("[server-%d] Starting server worker\n", server_id);
    
    // Initialize server
    if (!initialize_server(port)) {
        return EXIT_FAILURE;
    }
    
    // Main loop
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // Accept connection (with timeout to check running flag periodically)
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        
        struct timeval timeout;
        timeout.tv_sec = 1;  // Check for termination every second
        timeout.tv_usec = 0;
        
        if (select(server_fd + 1, &read_fds, NULL, NULL, &timeout) > 0) {
            int client_socket = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
            if (client_socket >= 0) {
                // Handle the connection
                handle_connection(client_socket);
            }
        }
    }
    
    // Clean up and exit
    cleanup();
    return EXIT_SUCCESS;
}