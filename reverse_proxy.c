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
#define HEADER_SIZE 32  // Size of the header in bytes

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

// Global pipe file descriptor for reading configuration
int config_pipe_fd = -1;

// Forward declarations
void initialize_server_ports(void);
void read_server_ports_from_pipe(void);

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        printf("[reverse_proxy-%d] Received termination signal, shutting down...\n", proxy_id);
        running = 0;
    } else if (sig == SIGHUP) {
        printf("[reverse_proxy-%d] Received SIGHUP, reading updated server ports from pipe\n", proxy_id);
        // Read server ports from the pipe instead of environment variables
        read_server_ports_from_pipe();
        
        // Check if configuration was successful
        bool any_server_available = false;
        for (size_t i = 0; i < NUM_SERVERS; i++) {
            if (servers[i].available) {
                any_server_available = true;
                break;
            }
        }
        
        if (any_server_available) {
            printf("[reverse_proxy-%d] Updated server configuration received, now operational\n", proxy_id);
        } else {
            printf("[reverse_proxy-%d] Warning: Received SIGHUP but no valid server configuration\n", proxy_id);
        }
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

// Track whether this is first initialization
static bool first_init = true;

// Read server port information from the config pipe
void read_server_ports_from_pipe() {
    if (config_pipe_fd < 0) {
        printf("[reverse_proxy-%d] Error: Cannot read from pipe - invalid file descriptor\n", proxy_id);
        return;
    }
    
    char buffer[256];
    memset(buffer, 0, sizeof(buffer));
    
    // Set non-blocking read with timeout to avoid hanging if pipe is empty
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(config_pipe_fd, &read_fds);
    
    struct timeval timeout;
    timeout.tv_sec = 2;  // 2 second timeout - increased for reliability
    timeout.tv_usec = 0;
    
    // Try multiple times in case data is not immediately available (common for respawned processes)
    int max_attempts = 3;
    int attempts = 0;
    int ready = 0;
    
    while (attempts < max_attempts) {
        ready = select(config_pipe_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (ready > 0) {
            // Data available
            break;
        } else if (ready < 0) {
            if (errno == EINTR) {
                // Interrupted, try again
                printf("[reverse_proxy-%d] Select was interrupted, retrying...\n", proxy_id);
                attempts++;
                continue;
            } else {
                perror("[reverse_proxy] Error reading from config pipe");
                return;
            }
        } else {
            // Timeout
            printf("[reverse_proxy-%d] Attempt %d: No data available yet on pipe\n", proxy_id, attempts + 1);
            attempts++;
            
            if (attempts < max_attempts) {
                // Reset for next try
                FD_ZERO(&read_fds);
                FD_SET(config_pipe_fd, &read_fds);
                timeout.tv_sec = 2;
                timeout.tv_usec = 0;
            }
        }
    }
    
    if (ready <= 0) {
        printf("[reverse_proxy-%d] No data received after %d attempts\n", proxy_id, max_attempts);
        return;
    }
    
    // Data is available, read it
    ssize_t bytes_read = read(config_pipe_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        // Error or end of file
        if (bytes_read < 0) {
            perror("[reverse_proxy] Failed to read from config pipe");
        } else {
            printf("[reverse_proxy-%d] Config pipe closed or empty\n", proxy_id);
        }
        return;
    }
    
    buffer[bytes_read] = '\0';  // Ensure null-termination
    
    // Parse the server port information
    // Format expected: "count:port1:port2:...:portn:"
    char *token;
    char *rest = buffer;
    
    // Get server count
    token = strtok_r(rest, ":", &rest);
    if (!token) {
        printf("[reverse_proxy-%d] Error: Invalid server port data format\n", proxy_id);
        return;
    }
    
    int server_count = atoi(token);
    
    if (server_count <= 0 || server_count > 6) {
        printf("[reverse_proxy-%d] Error: Invalid server count: %d\n", proxy_id, server_count);
        return;
    }
    
    // Based on proxy_id, determine which servers to use
    // Proxy 1 will use servers 1,2,3 and Proxy 2 will use servers 4,5,6
    int start_server = (proxy_id == 2) ? 3 : 0;  // Start from server 4 for proxy_id 2
    
    // Skip to the correct starting port based on proxy_id
    if (proxy_id == 2) {
        // For proxy_id 2, skip the first 3 ports in the data
        for (int i = 0; i < 3; i++) {
            token = strtok_r(rest, ":", &rest);
            if (!token) {
                printf("[reverse_proxy-%d] Error: Not enough ports in data to skip for proxy 2\n", proxy_id);
                return;
            }
        }
    }
    
    // Read each server port for this proxy
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        // Get the port for this server
        token = strtok_r(rest, ":", &rest);
        if (!token) {
            printf("[reverse_proxy-%d] Warning: Not enough ports in data for server %zu\n", proxy_id, i);
            continue;
        }
        
        int port = atoi(token);
        
        // Store the server ID (1-based)
        int server_id = (int)(start_server + i + 1);
        
        servers[i].id = server_id;
        
        if (port > 0 && port < 65536) {
            servers[i].port = port;
            servers[i].available = true;
        } else {
            printf("[reverse_proxy-%d] Warning: Invalid port value '%d' for server %d\n", 
                  proxy_id, port, server_id);
            servers[i].available = false;
        }
    }
    
    first_init = false;
}

// Initialize server ports - start in non-workable state, await configuration
void initialize_server_ports() {
    // Start in a non-workable state
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        servers[i].available = false;
    }
    
    // Try to read from pipe if available
    if (config_pipe_fd >= 0) {
        read_server_ports_from_pipe();
        
        // Check if we got any valid ports
        bool any_server_available = false;
        for (size_t i = 0; i < NUM_SERVERS; i++) {
            if (servers[i].available) {
                any_server_available = true;
                break;
            }
        }
        
        if (any_server_available) {
            printf("[reverse_proxy-%d] Received initial server configuration, now operational\n", proxy_id);
        }
    } else {
        printf("[reverse_proxy-%d] Error: Config pipe not available, cannot receive server ports\n", proxy_id);
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
    static int check_frequency = 5;  // Start with more frequent checks (5 seconds)
    static int check_counter = 0;
    time_t current_time = time(NULL);
    
    // Dynamic check frequency - more frequent at startup, less frequent later
    if (current_time - last_check < check_frequency) {
        return;
    }
    
    last_check = current_time;
    check_counter++;
    
    // After 10 checks, increase the interval to 30 seconds
    if (check_counter == 10) {
        check_frequency = 30;
    }
    
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

// Track when we last checked environment variables
static time_t last_env_check = 0;
static const int ENV_CHECK_INTERVAL = 60;       // Regular check interval (60 seconds)
static const int INITIAL_CHECK_INTERVAL = 5;    // More frequent checks at startup (5 seconds)
static int current_check_interval = INITIAL_CHECK_INTERVAL;
static int check_count = 0;

// Handle a client connection and forward to a server
void handle_connection(int client_socket) {
    // Check for server availability periodically, but don't read from pipe during request handling
    time_t now = time(NULL);
    
    if (now - last_env_check > current_check_interval) {
        // Only check server availability, don't try to read from pipes during request handling
        check_server_availability();
        last_env_check = now;
        
        // After a few checks at the faster interval, switch to normal interval
        check_count++;
        if (check_count >= 6 && current_check_interval == INITIAL_CHECK_INTERVAL) {
            printf("[reverse_proxy-%d] Switching to normal availability check interval (%d seconds)\n", 
                   proxy_id, ENV_CHECK_INTERVAL);
            current_check_interval = ENV_CHECK_INTERVAL;
        }
    }
    
    ServerInfo* server = get_next_server();
    
    if (!server) {
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    // Read client input with header
    char buffer[1024] = {0};
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    
    // Check if we received at least the header
    if (bytes_read < HEADER_SIZE) {
        printf("[reverse_proxy-%d] Received incomplete message (less than header size)\n", proxy_id);
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    // Extract the client ID from the 32-byte header
    char header[HEADER_SIZE + 1] = {0};
    memcpy(header, buffer, HEADER_SIZE);
    int client_id = atoi(header);
    
    // Extract the actual input data that follows the header
    char* input_data = buffer + HEADER_SIZE;
    size_t input_length = bytes_read - HEADER_SIZE;
    
    // Ensure null termination of input data for validation
    input_data[input_length] = '\0';
    
    // Validate input - check if it's a non-negative float
    if (!is_valid_input(input_data)) {
        printf("[reverse_proxy-%d] Received invalid input from client %d, returning -1\n", 
               proxy_id, client_id);
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    printf("[reverse_proxy-%d] Received valid input from client %d, forwarding to server ID %d (port %d)\n", 
           proxy_id, client_id, server->id, server->port);
           
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
        printf("[reverse_proxy-%d] Failed to connect to server ID %d on port %d: %s\n", 
              proxy_id, server->id, server->port, strerror(errno));
        server->available = false;  // Mark as unavailable
        close(server_socket);
        close(client_socket);
        return;
    }
    
    // Forward the entire message with header to the server
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

// Wait for server ports to become available in environment variables
// Return true if server ports were found, false if we gave up waiting
bool wait_for_server_ports(int max_attempts) {
    printf("[reverse_proxy-%d] Waiting for server ports to be registered...\n", proxy_id);
    
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        // Attempt to initialize from environment variables
        initialize_server_ports();
        
        // Check if we got any valid ports
        bool any_server_available = false;
        for (size_t i = 0; i < NUM_SERVERS; i++) {
            if (servers[i].available) {
                any_server_available = true;
                break;
            }
        }
        
        if (any_server_available) {
            printf("[reverse_proxy-%d] Successfully found server ports on attempt %d\n", proxy_id, attempt);
            return true;
        }
        
        if (attempt < max_attempts) {
            printf("[reverse_proxy-%d] No server ports found yet, waiting 2 seconds (attempt %d/%d)...\n", 
                  proxy_id, attempt, max_attempts);
            sleep(2);  // Wait 2 seconds before trying again
        }
    }
    
    printf("[reverse_proxy-%d] Gave up waiting for server ports after %d attempts\n", proxy_id, max_attempts);
    return false;
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
    
    // Get the config pipe file descriptor from environment
    char *config_pipe_str = getenv("WATCHDOG_CONFIG_PIPE_FD");
    if (config_pipe_str != NULL) {
        config_pipe_fd = atoi(config_pipe_str);
        if (config_pipe_fd <= 0) {
            printf("[reverse_proxy-%d] Warning: Invalid config pipe FD: %s\n", proxy_id, config_pipe_str);
            config_pipe_fd = -1;
        }
    } else {
        printf("[reverse_proxy-%d] Warning: WATCHDOG_CONFIG_PIPE_FD not set\n", proxy_id);
        config_pipe_fd = -1;
    }
    
    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);

    printf("[reverse_proxy-%d] Starting reverse proxy\n", proxy_id);

    // Initialize all servers as unavailable to start in non-workable state
    for (size_t i = 0; i < NUM_SERVERS; i++) {
        servers[i].available = false;
    }
    
    // Initialize server socket first
    if (!initialize_server(port)) {
        if (config_pipe_fd >= 0) {
            close(config_pipe_fd);
        }
        return EXIT_FAILURE;
    }
    
    // Try to read server ports from config pipe - will remain in non-workable state
    // until receiving configuration via SIGHUP when servers report their ports
    initialize_server_ports();
    
    // We'll wait for the watchdog to signal when servers are ready via SIGHUP
    
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
    
    // Close config pipe if it's open
    if (config_pipe_fd >= 0) {
        close(config_pipe_fd);
    }
    
    return EXIT_SUCCESS;
}