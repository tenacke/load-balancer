// filepath: /Users/emre/Projects/load-balancer/client.c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>  // For usleep

#define DEFAULT_SERVER_IP "127.0.0.1"
#define DEFAULT_SERVER_PORT 8080  // Default port for the load balancer
#define BUFFER_SIZE 1024

// Function to create a connection to the server
int connect_to_server(const char* ip, int port) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    
    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[client] Socket creation failed");
        return -1;
    }
    
    // Configure server address
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    
    // Convert IP address from text to binary form
    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("[client] Invalid address/Address not supported");
        close(sock);
        return -1;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("[client] Connection failed");
        close(sock);
        return -1;
    }
    
    return sock;
}

// Function to get user input
char* get_user_input() {
    static char input_buffer[BUFFER_SIZE];
    
    // Get input number from user
    printf("Enter a non-negative float: ");
    
    if (fgets(input_buffer, BUFFER_SIZE, stdin) == NULL) {
        printf("Error reading input\n");
        return NULL;
    }
    
    // Remove trailing newline if present
    size_t len = strlen(input_buffer);
    if (len > 0 && input_buffer[len-1] == '\n') {
        input_buffer[len-1] = '\0';
    }
    
    return input_buffer;
}

// Function to send client ID and a number to the server
void send_request(int socket_fd, int client_id, const char* input_value) {
    char client_id_str[16];
    snprintf(client_id_str, sizeof(client_id_str), "%d", client_id);
    
    // Send client ID first to identify ourselves to the load balancer
    if (send(socket_fd, client_id_str, strlen(client_id_str), 0) < 0) {
        perror("[client] Failed to send client ID");
        return;
    }
    
    // Small delay to ensure client ID is processed separately
    usleep(50000);  // 50ms delay
    
    // Send the input number to the server
    if (send(socket_fd, input_value, strlen(input_value), 0) < 0) {
        perror("[client] Failed to send input");
        return;
    }
    
    printf("Input: %s\n", input_value);
}

// Function to receive response from the server
void receive_response(int socket_fd) {
    char buffer[BUFFER_SIZE] = {0};
    ssize_t bytes_received;
    
    // Receive response
    bytes_received = recv(socket_fd, buffer, BUFFER_SIZE - 1, 0);
    
    if (bytes_received < 0) {
        perror("[client] Receive failed");
        return;
    }
    buffer[bytes_received] = '\0';
    
    printf("Result: %s\n", buffer);
}

int main(int argc, char *argv[]) {
    const char* server_ip = DEFAULT_SERVER_IP;
    int server_port = DEFAULT_SERVER_PORT;
    int client_id = 1;  // Default client ID
    
    // Check if client ID was provided as command line argument
    if (argc >= 2) {
        client_id = atoi(argv[1]);
        if (client_id <= 0) {
            printf("Invalid client ID %s, using default 1\n", argv[1]);
            printf("Usage: ./client <id_arg> where id_arg is a positive integer\n");
            client_id = 1;
        }
    } else {
        printf("No client ID provided, using default ID: 1\n");
        printf("Usage: ./client <id_arg> where id_arg is a positive integer\n");
    }

    printf("This is Client %d\n", client_id);
    
    // Get user input first
    char* input_value = get_user_input();
    if (input_value == NULL) {
        return EXIT_FAILURE;
    }
    
    // Connect to server (load balancer) after getting input
    int sock_fd = connect_to_server(server_ip, server_port);
    if (sock_fd < 0) {
        printf("Failed to connect to the load balancer. Is it running?\n");
        return EXIT_FAILURE;
    }
    
    // Send client ID and input
    send_request(sock_fd, client_id, input_value);
    
    // Receive response (square root result)
    receive_response(sock_fd);
    
    // Close connection
    close(sock_fd);
    
    return EXIT_SUCCESS;
}