// filepath: /Users/emre/Projects/load-balancer/load_balancer.c
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
#include <pthread.h>

#define MAX_REVERSE_PROXIES 2
#define DEFAULT_PORT 8080
#define MAX_THREADS 100
#define HEADER_SIZE 32  // Size of the header in bytes

// Forward declarations
void initialize_proxy_ports(void);
void read_proxy_ports_from_pipe(void);

// Global variables
int server_fd;
int config_pipe_fd = -1;  // Global pipe FD for reading configuration
volatile sig_atomic_t running = 1;

// Thread argument structure
typedef struct {
    int client_socket;
    struct sockaddr_in client_addr;
} ClientInfo;

// Reverse proxy information
typedef struct {
    int id;
    char address[16];      // IP address (e.g. "127.0.0.1")
    int port;              // Port number
    bool available;        // Whether the proxy is active
} ReverseProxy;

ReverseProxy reverse_proxies[MAX_REVERSE_PROXIES] = {
    {1, "127.0.0.1", 0, false},  // Ports will be updated from environment variables
    {2, "127.0.0.1", 0, false}
};

int current_proxy = 0;     // Used for round-robin load balancing

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        printf("[load_balancer] Received termination signal, shutting down...\n");
        running = 0;
    } else if (sig == SIGHUP) {
        printf("[load_balancer] Received SIGHUP, reading proxy ports from pipe\n");
        // Read proxy ports from the pipe instead of environment variables
        read_proxy_ports_from_pipe();
        
        // Check if we have any available proxies after reading configuration
        bool any_proxy_available = false;
        for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
            if (reverse_proxies[i].available) {
                any_proxy_available = true;
                printf("[load_balancer] Proxy %d is available at port %d\n", i+1, reverse_proxies[i].port);
                break;
            }
        }
        
        if (any_proxy_available) {
            printf("[load_balancer] Updated proxy configuration received, now operational\n");
        } else {
            printf("[load_balancer] Warning: Received SIGHUP but no valid proxy configuration\n");
        }
    }
}

// Initialize server socket
bool initialize_server(int port) {
    struct sockaddr_in address;
    int opt = 1;
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("[load_balancer] Socket creation failed");
        return false;
    }
    
    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("[load_balancer] Setsockopt failed");
        return false;
    }
    
    // Configure address
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    
    // Bind socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("[load_balancer] Bind failed");
        return false;
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("[load_balancer] Listen failed");
        return false;
    }
    
    return true;
}

// Initialize reverse proxy ports from environment variables
// Read proxy port information from the config pipe
void read_proxy_ports_from_pipe() {
    if (config_pipe_fd < 0) {
        printf("[load_balancer] Error: Cannot read from pipe - invalid file descriptor\n");
        return;
    }
    
    char buffer[128];
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
                printf("[load_balancer] Select was interrupted, retrying...\n");
                attempts++;
                continue;
            } else {
                perror("[load_balancer] Error reading from config pipe");
                return;
            }
        } else {
            // Timeout
            printf("[load_balancer] Attempt %d: No data available yet on pipe\n", attempts + 1);
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
        printf("[load_balancer] No data received after %d attempts\n", max_attempts);
        return;
    }
    
    // Data is available, read it
    ssize_t bytes_read = read(config_pipe_fd, buffer, sizeof(buffer) - 1);
    
    if (bytes_read <= 0) {
        // Error or end of file
        if (bytes_read < 0) {
            perror("[load_balancer] Failed to read from config pipe");
        } else {
            printf("[load_balancer] Config pipe closed or empty\n");
        }
        return;
    }
    
    buffer[bytes_read] = '\0';  // Ensure null-termination
    
    // Parse the proxy port information
    // Format expected: "count:port1:port2:...:portn:"
    char *token;
    char *rest = buffer;
    
    // Get proxy count
    token = strtok_r(rest, ":", &rest);
    if (!token) {
        printf("[load_balancer] Error: Invalid proxy port data format\n");
        return;
    }
    
    int proxy_count = atoi(token);
    
    if (proxy_count <= 0 || proxy_count > MAX_REVERSE_PROXIES) {
        printf("[load_balancer] Error: Invalid proxy count: %d\n", proxy_count);
        return;
    }
    
    // Read each proxy port
    for (int i = 0; i < proxy_count && i < MAX_REVERSE_PROXIES; i++) {
        // Get the port for this proxy
        token = strtok_r(rest, ":", &rest);
        if (!token) {
            printf("[load_balancer] Warning: Not enough ports in data for proxy %d\n", i + 1);
            continue;
        }
        
        int port = atoi(token);
        
        if (port > 0 && port < 65536) {
            reverse_proxies[i].port = port;
            reverse_proxies[i].available = true;
        } else {
            reverse_proxies[i].available = false;
        }
    }
}

void initialize_proxy_ports() {
    // Start in non-workable state
    for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
        reverse_proxies[i].available = false;
    }
    
    // Check if config pipe is available
    if (config_pipe_fd >= 0) {
        read_proxy_ports_from_pipe();
    } else {
        printf("[load_balancer] Warning: Config pipe not available, cannot receive configuration\n");
        
        // Only as a fallback if pipe isn't available (should not happen in normal operation)
        char env_var[32];
        char *env_val;
        
        for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
            // Check for environment variables like REVERSE_PROXY_PORT_1, REVERSE_PROXY_PORT_2, etc.
            snprintf(env_var, sizeof(env_var), "REVERSE_PROXY_PORT_%d", i + 1);
            env_val = getenv(env_var);
            
            if (env_val != NULL) {
                int port = atoi(env_val);
                if (port > 0 && port < 65536) {
                    reverse_proxies[i].port = port;
                    reverse_proxies[i].available = true;
                }
            }
        }
    }
    
    // Check if any proxy ports were set
    bool any_proxy_available = false;
    for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
        if (reverse_proxies[i].available) {
            any_proxy_available = true;
            break;
        }
    }
    
    if (any_proxy_available) {
        printf("[load_balancer] Proxy ports configured, load balancer now operational\n");
    }
}

// Get a reverse proxy based on client ID (odd/even)
ReverseProxy* get_proxy_for_client(int client_id) {
    // Calculate which proxy to use based on client ID (0 for even, 1 for odd)
    int proxy_index = client_id % 2;
    
    // Check if the selected proxy is available
    if (reverse_proxies[proxy_index].available) {
        return &reverse_proxies[proxy_index];
    }
    
    // If the selected proxy is unavailable, try the other one
    int other_proxy = (proxy_index + 1) % MAX_REVERSE_PROXIES;
    if (reverse_proxies[other_proxy].available) {
        return &reverse_proxies[other_proxy];
    }
    
    // No available proxies
    return NULL;
}

// Check if a proxy is available by attempting a connection
void check_proxy_availability() {
    static time_t last_check = 0;
    time_t current_time = time(NULL);
    
    // Only check every 30 seconds
    if (current_time - last_check < 30) {
        return;
    }
    
    last_check = current_time;
    
    // Check each unavailable proxy
    for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
        if (!reverse_proxies[i].available) {
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) continue;
            
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_port = htons(reverse_proxies[i].port);
            
            if (inet_pton(AF_INET, reverse_proxies[i].address, &addr.sin_addr) <= 0) {
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
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {                    // Connection successful
                    reverse_proxies[i].available = true;
                    }
                }
            }
            
            close(sock);
        }
    }
}

// Forward declaration of the handle_connection function
void handle_connection(int client_socket, struct sockaddr_in *client_addr);

// Thread function to handle client connections
void* handle_connection_thread(void* arg) {
    // Detach thread so resources are automatically released when it exits
    pthread_detach(pthread_self());
    
    // Extract client information
    ClientInfo* client_info = (ClientInfo*)arg;
    int client_socket = client_info->client_socket;
    struct sockaddr_in client_addr = client_info->client_addr;
    free(client_info);  // Free allocated memory
    
    // Handle the connection (non-blocking)
    handle_connection(client_socket, &client_addr);
    
    return NULL;
}

// Handle a client connection and forward to a reverse proxy
void handle_connection(int client_socket, struct sockaddr_in *client_addr) {
    // Default client ID from IP address in case header reading fails
    int client_id = client_addr->sin_addr.s_addr & 0xFF;
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    
    // Buffer to hold the complete message (header + data)
    char buffer[1024] = {0};
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read < HEADER_SIZE) {
        printf("[load_balancer] Received incomplete message (less than header size)\n");
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    // Extract the client ID from the 32-byte header
    char header[HEADER_SIZE + 1] = {0};
    memcpy(header, buffer, HEADER_SIZE);
    client_id = atoi(header);
    
    // Get appropriate reverse proxy based on client ID
    ReverseProxy* proxy = get_proxy_for_client(client_id);
    
    if (!proxy) {
        const char* error_msg = "-1";
        write(client_socket, error_msg, strlen(error_msg));
        close(client_socket);
        return;
    }
    
    printf("[load_balancer] Handling request from client %d, forwarding to reverse proxy %d\n", 
           client_id, proxy->id);
           
    // Create a socket to connect to the proxy
    int proxy_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socket < 0) {
        perror("[load_balancer] Failed to create socket for proxy connection");
        close(client_socket);
        return;
    }
    
    // Set up proxy address
    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(proxy_addr));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(proxy->port);
    
    // Convert IP from string to binary
    if (inet_pton(AF_INET, proxy->address, &proxy_addr.sin_addr) <= 0) {
        perror("[load_balancer] Invalid address for proxy");
        close(proxy_socket);
        close(client_socket);
        return;
    }
    
    // Connect to the proxy
    if (connect(proxy_socket, (struct sockaddr*)&proxy_addr, sizeof(proxy_addr)) < 0) {
        perror("[load_balancer] Failed to connect to proxy");
        proxy->available = false;  // Mark as unavailable
        close(proxy_socket);
        close(client_socket);
        return;
    }
    
    // We already have the initial data from the client in 'buffer'
    // Just forward it directly to the proxy (with the 32-byte header intact)
    if (send(proxy_socket, buffer, bytes_read, 0) != bytes_read) {
        perror("[load_balancer] Failed to forward initial data to proxy");
        close(proxy_socket);
        close(client_socket);
        return;
    }
    
    // Now set up for bidirectional data transfer for any subsequent data
    ssize_t bytes_transferred;
    fd_set read_fds;
    int max_fd = (client_socket > proxy_socket) ? client_socket : proxy_socket;
    
    // Set a timeout for the select call
    struct timeval timeout;
    timeout.tv_sec = 30;  // 30 seconds timeout
    timeout.tv_usec = 0;
    
    // Forward data until connection closes or timeout
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_socket, &read_fds);
        FD_SET(proxy_socket, &read_fds);
        
        int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        
        if (activity <= 0) {
            // Timeout or error
            break;
        }
        
        // Check if client has sent data
        if (FD_ISSET(client_socket, &read_fds)) {
            bytes_transferred = recv(client_socket, buffer, sizeof(buffer), 0);
            if (bytes_transferred <= 0) {
                break;  // Client closed connection or error
            }
            
            // Forward to proxy
            if (send(proxy_socket, buffer, bytes_transferred, 0) != bytes_transferred) {
                break;  // Failed to send all data
            }
        }
        
        // Check if proxy has sent data
        if (FD_ISSET(proxy_socket, &read_fds)) {
            bytes_transferred = recv(proxy_socket, buffer, sizeof(buffer), 0);
            if (bytes_transferred <= 0) {
                break;  // Proxy closed connection or error
            }
            
            // Forward to client
            if (send(client_socket, buffer, bytes_transferred, 0) != bytes_transferred) {
                break;  // Failed to send all data
            }
        }
    }
    
    // Clean up
    close(proxy_socket);
    close(client_socket);
}

// Clean up resources
void cleanup() {
    if (server_fd >= 0) {
        close(server_fd);
    }
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    
    // Parse command line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            printf("[load_balancer] Invalid port number %s, using default %d\n", argv[1], DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }
    
    // Get the config pipe file descriptor from environment
    char *config_pipe_str = getenv("WATCHDOG_CONFIG_PIPE_FD");
    if (config_pipe_str != NULL) {
        config_pipe_fd = atoi(config_pipe_str);
        if (config_pipe_fd <= 0) {
            printf("[load_balancer] Warning: Invalid config pipe FD: %s\n", config_pipe_str);
            config_pipe_fd = -1;
        }
    } else {
        printf("[load_balancer] Warning: WATCHDOG_CONFIG_PIPE_FD not set\n");
        config_pipe_fd = -1;
    }
    
    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);
    
    printf("[load_balancer] Starting load balancer\n");
    
    // Start in non-workable state, will be made operational after receiving configuration
    for (int i = 0; i < MAX_REVERSE_PROXIES; i++) {
        reverse_proxies[i].available = false;
    }
    
    // Initialize server first
    if (!initialize_server(port)) {
        if (config_pipe_fd >= 0) {
            close(config_pipe_fd);
        }
        return EXIT_FAILURE;
    }

    
    // Initialize reverse proxy ports from config pipe
    initialize_proxy_ports();
    
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
                // Create thread argument
                ClientInfo* client_info = malloc(sizeof(ClientInfo));
                client_info->client_socket = client_socket;
                memcpy(&client_info->client_addr, &client_addr, sizeof(struct sockaddr_in));
                
                // Create a new thread to handle the client connection
                pthread_t thread_id;
                if (pthread_create(&thread_id, NULL, handle_connection_thread, client_info) != 0) {
                    perror("[load_balancer] Failed to create thread");
                    close(client_socket);
                    free(client_info);
                }
            }
        }
        
        check_proxy_availability();  // Check proxy availability in the main loop
    }
    
    // Clean up and exit
    cleanup();
    
    // Close config pipe if it's open
    if (config_pipe_fd >= 0) {
        close(config_pipe_fd);
    }
    
    return EXIT_SUCCESS;
}