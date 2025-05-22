// filepath: /Users/emre/Projects/load-balancer/reverse_proxy.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <pthread.h>

#define DEFAULT_PORT 8081
#define DEFAULT_SERVER_PORT 9001
#define MAX_THREADS 100

// Global variables
int server_fd;
volatile sig_atomic_t running = 1;
int proxy_id = 0;  // Global proxy ID for logging

// Thread argument structure
typedef struct {
    int client_socket;
} ClientConnection;

// Server information
typedef struct {
    char address[16];     // IP address
    int port;             // Port number
    bool available;       // Whether the server is active
    int id;               // Server ID
} ServerInfo;

// Default server connections (round-robin between these)
ServerInfo servers[] = {
    {"127.0.0.1", 0, false, 0},
    {"127.0.0.1", 0, false, 0},
    {"127.0.0.1", 0, false, 0}
};

#define NUM_SERVERS (sizeof(servers) / sizeof(ServerInfo))
int current_server = 0; // For round-robin load balancing

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        printf("[reverse_proxy-%d] Received termination signal, shutting down...\n", proxy_id);
        running = 0;
    }
}

// Initialize server socket and find available port
bool initialize_server(int port) {
    struct sockaddr_in address;
    int opt = 1;
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[reverse_proxy] Socket creation failed");
        return false;
    }
    
    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("[reverse_proxy] Setsockopt failed");
        return false;
    }
    
    // Configure address
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;

    // Find an available port if requested port is 0 or binding fails
    if (port == 0 || port == DEFAULT_PORT) {
        // Try to find an available port in range 8081-8180
        for (int p = 8081; p < 8180; p++) {
            address.sin_port = htons(p);
            if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) >= 0) {
                port = p;
                break;
            }
        }
    } else {
        address.sin_port = htons(port);
        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("[reverse_proxy] Bind failed");
            close(server_fd);
            return false;
        }
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("[reverse_proxy] Listen failed");
        close(server_fd);
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

// Initialize server ports from environment variables
void initialize_server_ports() {
    char env_var[32];
    char *env_val;
    
    // Based on proxy_id, determine which servers to use
    // Proxy 1 will use servers 1,2,3 and Proxy 2 will use servers 4,5,6
    size_t start_server = (proxy_id == 2) ? 3 : 0;  // Start from server 4 for proxy_id 2
    
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        // Get environment variable for the appropriate server based on proxy_id
        size_t server_num = start_server + i + 1;  // Server numbers start from 1
        snprintf(env_var, sizeof(env_var), "SERVER_PORT_%zu", server_num);
        env_val = getenv(env_var);
        
        // Store the server ID
        servers[i].id = server_num;
        
        if (env_val != NULL) {
            int port = atoi(env_val);
            if (port > 0 && port < 65536) {
                servers[i].port = port;
                servers[i].available = true;
            }
        }
    }
    
    // Check if any server ports were set
    bool any_server_available = false;
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        if (servers[i].available) {
            any_server_available = true;
            break;
        }
    }
    
    // If no servers are available, set default values for testing
    if (!any_server_available) {
        int base_port = (proxy_id == 2) ? 9004 : 9001;  // Start from 9004 for proxy_id 2
        for (size_t i = 0; i < NUM_SERVERS; i++) {
            int server_id = (proxy_id == 2) ? (int)i + 4 : (int)i + 1;
            servers[i].port = base_port + (int)i;
            servers[i].available = true;
            servers[i].id = server_id;
        }
    }
}

// Get a random available server
ServerInfo* get_next_server() {
    // Initialize random seed if not already done
    static bool seed_initialized = false;
    if (!seed_initialized) {
        srand((unsigned int)time(NULL));
        seed_initialized = true;
    }
    
    // Count available servers
    int available_count = 0;
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        if (servers[i].available) {
            available_count++;
        }
    }
    
    if (available_count == 0) {
        return NULL;
    }
    
    // Select a random available server
    int random_index = rand() % available_count;
    int current_available = 0;
    
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        if (servers[i].available) {
            if (current_available == random_index) {
                return &servers[i];
            }
            current_available++;
        }
    }
    
    // Should never reach here if available_count > 0
    return NULL;
}

// Check if a server is available by attempting a connection
void check_server_availability() {
    static time_t last_check = 0;
    time_t current_time = time(NULL);
    
    // Only check every 30 seconds
    if (current_time - last_check < 30) {
        return;
    }
    
    last_check = current_time;
    
    // Check each unavailable server
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        if (!servers[i].available) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;
            
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(servers[i].port);
            
            if (inet_pton(AF_INET, servers[i].address, &addr.sin_addr) <= 0) {
                close(sock);
                continue;
            }
            
            // Set non-blocking for quick timeout
            int flags = fcntl(sock, F_GETFL, 0);
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
            
            // Try to connect
            int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
            
            if (result < 0 && errno == EINPROGRESS) {
                fd_set write_fds;
                FD_ZERO(&write_fds);
                FD_SET(sock, &write_fds);
                
                struct timeval timeout;
                timeout.tv_sec = 1;  // 1 second timeout for connection attempt
                timeout.tv_usec = 0;
                
                if (select(sock + 1, NULL, &write_fds, NULL, &timeout) > 0) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                        // Connection successful
                        servers[i].available = true;
                    }
                }
            }
            
            close(sock);
        }
    }
}

// Validate if input is a non-negative float
bool is_valid_input(const char* input) {
    // Skip leading whitespace
    while (*input && isspace(*input)) {
        input++;
    }
    
    // Empty string is not valid
    if (*input == '\0') {
        return false;
    }
    
    bool has_digit = false;
    bool has_decimal = false;
    
    // Check if first character is a digit or decimal point
    if (*input == '.') {
        has_decimal = true;
    } else if (isdigit(*input)) {
        has_digit = true;
    } else {
        return false;  // First non-space character must be digit or decimal point
    }
    
    input++;
    
    // Process the rest of the string
    while (*input && !isspace(*input) && *input != '\0') {
        if (*input == '.') {
            if (has_decimal) {
                return false;  // More than one decimal point is invalid
            }
            has_decimal = true;
        } else if (isdigit(*input)) {
            has_digit = true;
        } else {
            return false;  // Non-digit and non-decimal characters are invalid
        }
        input++;
    }
    
    // Must have at least one digit
    return has_digit;
}

// Forward declaration of the connection handler
void handle_connection(int client_socket);

// Thread function to handle client connections
void* handle_connection_thread(void* arg) {
    // Detach thread so resources are automatically released when it exits
    pthread_detach(pthread_self());
    
    // Extract client socket
    ClientConnection* conn = (ClientConnection*)arg;
    int client_socket = conn->client_socket;
    free(conn);  // Free allocated memory
    
    // Handle the connection
    handle_connection(client_socket);
    
    return NULL;
}

// Handle a client connection and forward to a server
void handle_connection(int client_socket) {
    ServerInfo* server = get_next_server();
    
    if (!server) {
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    // Read client input first for validation
    char buffer[1024] = {0};
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    
    // Ensure null termination
    buffer[bytes_read] = '\0';
    
    // Validate input - check if it's a non-negative integer
    if (!is_valid_input(buffer)) {
        printf("[reverse_proxy-%d] Received invalid input, returning -1\n", proxy_id);
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    printf("[reverse_proxy-%d] Received valid input, forwarding to server ID %d\n", 
           proxy_id, server->id);
           
    // Create a socket to connect to the server
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        printf("[reverse_proxy-%d] Failed to create socket for server connection: %s\n", 
               proxy_id, strerror(errno));
        close(client_socket);
        return;
    }
    
    // Set up server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server->port);
    
    // Convert IP from string to binary
    if (inet_pton(AF_INET, server->address, &server_addr.sin_addr) <= 0) {
        printf("[reverse_proxy-%d] Invalid address for server ID %d: %s\n", 
              proxy_id, server->id, strerror(errno));
        close(server_socket);
        close(client_socket);
        return;
    }
    
    // Connect to the server
    if (connect(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[reverse_proxy-%d] Failed to connect to server ID %d: %s\n", 
              proxy_id, server->id, strerror(errno));
        server->available = false;  // Mark as unavailable
        close(server_socket);
        close(client_socket);
        return;
    }
    
    // Forward the validated input to the server
    if (send(server_socket, buffer, bytes_read, 0) != bytes_read) {
        close(server_socket);
        close(client_socket);
        return;
    }
    
    // Receive response from server
    memset(buffer, 0, sizeof(buffer));
    bytes_read = recv(server_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        // Forward the server's response to the client
        write(client_socket, buffer, bytes_read);
    }
    
    // Clean up
    close(server_socket);
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
    if (argc > 1) {
        proxy_id = atoi(argv[1]);  // Set the global proxy_id
        if (proxy_id < 1) {
            printf("[reverse_proxy] Invalid proxy ID %s, using default 0\n", argv[1]);
            proxy_id = 0;
        }
    }
    
    if (argc > 2) {
        port = atoi(argv[2]);
        if (port < 0 || port > 65535) {
            printf("[reverse_proxy] Invalid port number %s, using auto-discovery\n", argv[2]);
            port = 0;
        }
    }
    
    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    
    printf("[reverse_proxy-%d] Starting reverse proxy\n", proxy_id);
    
    // Initialize server
    initialize_server_ports();
    if (!initialize_server(port)) {
        return EXIT_FAILURE;
    }
    
    // Main loop
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        // Periodically check server availability
        check_server_availability();
        
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
                // Create thread argument
                ClientConnection* conn = malloc(sizeof(ClientConnection));
                conn->client_socket = client_socket;
                
                // Create a new thread to handle the client connection
                pthread_t thread_id;
                if (pthread_create(&thread_id, NULL, handle_connection_thread, conn) != 0) {
                    perror("[reverse_proxy] Failed to create thread");
                    close(client_socket);
                    free(conn);
                }
            }
        }
        
        check_server_availability();  // Check server availability in the main loop
    }
    
    // Clean up and exit
    cleanup();
    return EXIT_SUCCESS;
}