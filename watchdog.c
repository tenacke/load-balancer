#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define MAX_PROCESSES 10
#define CHECK_INTERVAL 5 // Check processes every 5 seconds
#define DEFAULT_LB_PORT 8080  // Default port for load balancer

// Forward declaration of functions we'll use
void update_port_environment(void);

// Function to check if a port is available
bool check_port_available(int port) {
    int sockfd;
    struct sockaddr_in addr;
    
    // Create a socket
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("[watchdog] Socket creation failed");
        return false;
    }
    
    // Set socket options to allow reuse of local addresses
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("[watchdog] Setsockopt failed");
        close(sockfd);
        return false;
    }
    
    // Initialize address structure
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    // Try to bind to the port
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            printf("[watchdog] Port %d is already in use\n", port);
            close(sockfd);
            return false;
        }
        
        perror("[watchdog] Bind failed");
        close(sockfd);
        return false;
    }
    
    // Close socket and return true (port is available)
    close(sockfd);
    return true;
}

typedef enum {
    PROC_LOAD_BALANCER,
    PROC_REVERSE_PROXY,
    PROC_SERVER
} ProcessType;

typedef struct {
    char name[64];
    char path[256];
    char **args;
    pid_t pid;
    ProcessType type;
    bool active;
    time_t last_spawned;
    int pipe_fd[2];  // Communication pipe between watchdog and process
    int config_pipe[2]; // Additional pipe for sending configuration data (server ports, etc.)
    int port;        // Port the process is listening on
} Process;

// Global variables
Process *processes = NULL;
int process_count = 0;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t child_exited = 0;  // Flag to indicate a child has exited

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGTSTP) {
        printf("[watchdog] Received termination signal, terminating child processes for shutting down...\n");
        usleep(100000);  // Sleep to allow any pending writes to complete
        running = 0;
    } else if (sig == SIGCHLD) {
        // Set flag to indicate that a child process has exited
        child_exited = 1;
    }
}

// Initialize a new process structure
Process* create_process(const char* name, const char* path, char** args, ProcessType type) {
    Process* proc = &processes[process_count++];
    
    strncpy(proc->name, name, sizeof(proc->name) - 1);
    strncpy(proc->path, path, sizeof(proc->path) - 1);
    proc->args = args;
    proc->pid = -1;
    proc->type = type;
    proc->active = false;
    proc->last_spawned = 0;
    
    return proc;
}

// Launch a process
bool spawn_process(Process* proc) {
    // Create a pipe for IPC (for process to report its port)
    if (pipe(proc->pipe_fd) < 0) {
        perror("[watchdog] Port reporting pipe creation failed");
        return false;
    }
    
    // Create a config pipe for watchdog to send configuration info to the process
    if (pipe(proc->config_pipe) < 0) {
        perror("[watchdog] Config pipe creation failed");
        close(proc->pipe_fd[0]);
        close(proc->pipe_fd[1]);
        return false;
    }

    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        perror("[watchdog] Fork failed");
        close(proc->pipe_fd[0]);
        close(proc->pipe_fd[1]);
        close(proc->config_pipe[0]);
        close(proc->config_pipe[1]);
        return false;
    } else if (pid == 0) {
        // Child process
        // Close read end, keep write end for reporting port
        close(proc->pipe_fd[0]);
        
        // Close write end, keep read end for receiving config
        close(proc->config_pipe[1]);
        
        // Pass the pipe FDs as environment variables
        char pipe_fd_str[16];
        snprintf(pipe_fd_str, sizeof(pipe_fd_str), "%d", proc->pipe_fd[1]);
        setenv("WATCHDOG_PIPE_FD", pipe_fd_str, 1);
        
        // Pass config pipe FD as an environment variable
        char config_pipe_fd_str[16];
        snprintf(config_pipe_fd_str, sizeof(config_pipe_fd_str), "%d", proc->config_pipe[0]);
        setenv("WATCHDOG_CONFIG_PIPE_FD", config_pipe_fd_str, 1);
        
        // We keep the PROCESS_TYPE environment variable as it might be used elsewhere
        char proc_type_str[2];
        snprintf(proc_type_str, sizeof(proc_type_str), "%d", (int)proc->type);
        setenv("PROCESS_TYPE", proc_type_str, 1);
        
        execv(proc->path, proc->args);
        // If execv returns, it failed
        perror("[watchdog] execv failed");
        close(proc->pipe_fd[1]);
        close(proc->config_pipe[0]);
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        // Close write end, keep read end for receiving port
        close(proc->pipe_fd[1]);
        
        // Close read end, keep write end for sending config
        close(proc->config_pipe[0]);
        
        proc->pid = pid;
        proc->active = true;
        proc->last_spawned = time(NULL);
        
        // Store previous port so we can check if it changed
        int prev_port = proc->port;
        proc->port = 0;  // Will be set when child reports back
        
        // For non-load balancer processes, wait for port information
        if (proc->type != PROC_LOAD_BALANCER) {
            char buffer[32];
            ssize_t bytes_read = read(proc->pipe_fd[0], buffer, sizeof(buffer) - 1);
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                proc->port = atoi(buffer);
                
                // If this is a server whose port has changed, update environment variables
                // and notify reverse proxies
                if (proc->type == PROC_SERVER && prev_port != 0 && proc->port != prev_port) {
                    printf("[watchdog] Server port changed from %d to %d, updating environment\n", 
                           prev_port, proc->port);
                    // This will update env vars and notify reverse proxies as needed
                    update_port_environment();
                }
                
                // If this is a reverse proxy whose port has changed, update environment variables
                // and notify load balancer
                if (proc->type == PROC_REVERSE_PROXY && prev_port != 0 && proc->port != prev_port) {
                    printf("[watchdog] Reverse proxy port changed from %d to %d, updating environment\n", 
                           prev_port, proc->port);
                    // This will update env vars and notify load balancer as needed
                    update_port_environment();
                }
            } else {
                printf("[watchdog] Warning: Process %s did not report port\n", proc->name);
            }
        } else {
            // Load balancer uses a fixed port
            proc->port = DEFAULT_LB_PORT;
        }
        
        return true;
    }
}

// Check if a process is still running
bool is_process_running(pid_t pid) {
    if (pid <= 0)
        return false;
    
    // Send signal 0 to check if process exists
    return (kill(pid, 0) == 0);
}

// Initialize all processes
void init_processes() {
    processes = calloc(MAX_PROCESSES, sizeof(Process));
    if (!processes) {
        perror("[watchdog] Failed to allocate memory for processes");
        exit(EXIT_FAILURE);
    }
    
    // Load balancer with port argument
    int lb_port = DEFAULT_LB_PORT;
    
    // Check if the default port is available, if not find one that is
    if (!check_port_available(lb_port)) {
        printf("[watchdog] Default load balancer port %d is not available, trying alternatives...\n", lb_port);
        // Try a range of ports
        for (int p = 8000; p < 9000; p++) {
            if (check_port_available(p)) {
                lb_port = p;
                printf("[watchdog] Found available port for load balancer: %d\n", lb_port);
                break;
            }
        }
    }
    
    char **lb_args = malloc(3 * sizeof(char*));
    lb_args[0] = strdup("./load_balancer");
    lb_args[1] = malloc(10);
    snprintf(lb_args[1], 10, "%d", lb_port);
    lb_args[2] = NULL;
    
    create_process("Load Balancer", "./load_balancer", lb_args, PROC_LOAD_BALANCER);

    // Reverse proxies (spawning 2 instances)
    for (int i = 0; i < 2; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Reverse Proxy %d", i+1);
        
        // Allocate memory for arguments - pass proxy ID and let it find its own port
        char **rp_args = malloc(3 * sizeof(char*));
        rp_args[0] = strdup("./reverse_proxy");
        rp_args[1] = malloc(10);
        snprintf(rp_args[1], 10, "%d", i+1);  // Pass proxy ID (1-based)
        rp_args[2] = NULL;
        
        create_process(name, "./reverse_proxy", rp_args, PROC_REVERSE_PROXY);
    }

    // Server workers (spawning 6 instances - 3 for each reverse proxy)
    for (int i = 0; i < 6; i++) {
        char name[64];
        snprintf(name, sizeof(name), "Server %d", i+1);
        
        // Allocate memory for arguments - pass server ID and let it find its own port
        char **srv_args = malloc(3 * sizeof(char*));
        srv_args[0] = strdup("./server");
        srv_args[1] = malloc(10);
        snprintf(srv_args[1], 10, "%d", i+1);  // Pass server ID (1-based)
        srv_args[2] = NULL;
        
        create_process(name, "./server", srv_args, PROC_SERVER);
    }
}

// Track current port assignments to avoid unnecessary updates
static int prev_server_ports[MAX_PROCESSES] = {0};
static int prev_proxy_ports[MAX_PROCESSES] = {0};
static bool first_run = true;

// Send port information to child processes via pipes
void update_port_environment() {
    int proxy_count = 0;
    int server_count = 0;
    
    // Track which processes are active
    bool has_active_load_balancer = false;
    
    // First, collect current ports
    int current_server_ports[MAX_PROCESSES] = {0};
    int current_proxy_ports[MAX_PROCESSES] = {0};
    
    for (int i = 0; i < process_count; i++) {
        // Skip processes without a valid port or inactive processes
        if (processes[i].port <= 0 || !processes[i].active) {
            continue;
        }

        // Collect all current ports
        if (processes[i].type == PROC_REVERSE_PROXY) {
            proxy_count++;
            current_proxy_ports[proxy_count] = processes[i].port;
        }
        else if (processes[i].type == PROC_SERVER) {
            server_count++;
            current_server_ports[server_count] = processes[i].port;
        }
        else if (processes[i].type == PROC_LOAD_BALANCER) {
            has_active_load_balancer = true;
        }
    }
    
    printf("[watchdog] Collected %d server ports and %d proxy ports from active processes\n", 
           server_count, proxy_count);
           
    // Validate that we have enough active components to proceed
    if (proxy_count == 0) {
        printf("[watchdog] Warning: No active reverse proxies with valid ports\n");
    }
    if (server_count == 0) {
        printf("[watchdog] Warning: No active servers with valid ports\n");
    }
    if (!has_active_load_balancer) {
        printf("[watchdog] Warning: No active load balancer\n");
    }
    
    // Send ports to components in the correct order based on the startup sequence:
    // 1. First send reverse proxy ports to load balancer
    if (proxy_count > 0) {
        // Prepare proxy port data to send to load balancer
        char proxy_ports_data[128];
        memset(proxy_ports_data, 0, sizeof(proxy_ports_data));
        
        int offset = 0;
        // Add proxy count at the beginning
        offset += snprintf(proxy_ports_data + offset, sizeof(proxy_ports_data) - offset, 
                          "%d:", proxy_count);
        
        // Add proxy ports, separated by colons
        for (int j = 1; j <= proxy_count; j++) {
            offset += snprintf(proxy_ports_data + offset, sizeof(proxy_ports_data) - offset, 
                              "%d:", current_proxy_ports[j]);
        }
        
        // Send data to load balancer through its config pipe
        for (int i = 0; i < process_count; i++) {
            if (processes[i].type == PROC_LOAD_BALANCER && processes[i].active) {
                printf("[watchdog] Sending proxy ports to load balancer (PID: %d): %s\n", 
                       processes[i].pid, proxy_ports_data);
                
                // Send proxy port information through the config pipe
                if (write(processes[i].config_pipe[1], proxy_ports_data, strlen(proxy_ports_data)) < 0) {
                    perror("[watchdog] Failed to write proxy ports to load balancer config pipe");
                    printf("[watchdog] Error code: %d, Error message: %s\n", errno, strerror(errno));
                } else {
                    // Ensure the data is written completely before sending signal
                    fsync(processes[i].config_pipe[1]);
                    
                    // Also send SIGHUP to notify that new data is available
                    if (kill(processes[i].pid, SIGHUP) < 0) {
                        perror("[watchdog] Failed to send SIGHUP to load balancer");
                        printf("[watchdog] Error sending signal to PID %d: %s\n", processes[i].pid, strerror(errno));
                    } else {
                        printf("[watchdog] Sent SIGHUP to load balancer (PID: %d)\n", processes[i].pid);
                    }
                }
                break;  // There should only be one load balancer
            }
        }
        
        // Small delay to ensure load balancer has time to process the configuration
        usleep(200000);  // 200ms
    } else {
        printf("[watchdog] No proxy ports available to send to load balancer\n");
    }
    
    // 2. Then send server ports to reverse proxies
    if (server_count > 0) {
        // Prepare server port data to send to reverse proxies
        char server_ports_data[256];
        memset(server_ports_data, 0, sizeof(server_ports_data));
        
        int offset = 0;
        // Add server count at the beginning
        offset += snprintf(server_ports_data + offset, sizeof(server_ports_data) - offset, 
                          "%d:", server_count);
        
        // Add server ports, separated by colons
        for (int j = 1; j <= server_count; j++) {
            offset += snprintf(server_ports_data + offset, sizeof(server_ports_data) - offset, 
                              "%d:", current_server_ports[j]);
        }
        
        printf("[watchdog] Sending server ports to reverse proxies: %s\n", server_ports_data);
        
        // Send data to each reverse proxy through its config pipe
        for (int i = 0; i < process_count; i++) {
            if (processes[i].type == PROC_REVERSE_PROXY && processes[i].active) {
                int proxy_id = 0;
                
                // Extract proxy ID from the process name (format: "Reverse Proxy X")
                if (sscanf(processes[i].name, "Reverse Proxy %d", &proxy_id) != 1) {
                    // Fallback if we can't extract from name - use position in array
                    // assuming first two processes after load balancer are the proxies
                    proxy_id = (i == 1) ? 1 : ((i == 2) ? 2 : 0);
                }
                
                printf("[watchdog] Sending server ports to reverse proxy %d (PID: %d)\n", 
                       proxy_id, processes[i].pid);
                
                // Send server port information through the config pipe
                if (write(processes[i].config_pipe[1], server_ports_data, strlen(server_ports_data)) < 0) {
                    perror("[watchdog] Failed to write server ports to reverse proxy config pipe");
                    printf("[watchdog] Error code: %d, Error message: %s\n", errno, strerror(errno));
                } else {
                    // Ensure the data is written completely before sending signal
                    fsync(processes[i].config_pipe[1]);
                    
                    // Also send SIGHUP to notify that new data is available
                    if (kill(processes[i].pid, SIGHUP) < 0) {
                        perror("[watchdog] Failed to send SIGHUP to reverse proxy");
                        printf("[watchdog] Error sending signal to PID %d: %s\n", processes[i].pid, strerror(errno));
                    } else {
                        printf("[watchdog] Sent SIGHUP to reverse proxy %d (PID: %d)\n", 
                               proxy_id, processes[i].pid);
                    }
                }
            }
        }
    } else {
        printf("[watchdog] No server ports available to send to reverse proxies\n");
    }
    
    // Save the current ports for next comparison
    memcpy(prev_server_ports, current_server_ports, sizeof(prev_server_ports));
    memcpy(prev_proxy_ports, current_proxy_ports, sizeof(prev_proxy_ports));
    
    printf("[watchdog] Configuration updated for all components\n");
    first_run = false;  // After the first run, we won't notify unless ports change
}

// Main monitoring loop
void monitor_processes() {
    while (running) {
        // Handle any terminated children when SIGCHLD is received
        if (child_exited) {
            child_exited = 0;  // Reset flag
            
            int status;
            pid_t terminated_pid;
            
            // Check for all terminated children
            while ((terminated_pid = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < process_count; i++) {
                    if (processes[i].pid == terminated_pid) {
                        time_t current_time = time(NULL);
                        processes[i].active = false;
                        
                        // Check if process terminated normally or crashed
                        if (WIFEXITED(status)) {
                            printf("[watchdog] Process %s (PID: %d) terminated with status %d\n", 
                                   processes[i].name, terminated_pid, WEXITSTATUS(status));
                        } else if (WIFSIGNALED(status)) {
                            printf("[watchdog] Process %s (PID: %d) crashed with signal %d\n", 
                                   processes[i].name, terminated_pid, WTERMSIG(status));
                        }
                        
                        // Don't respawn too rapidly - add a delay if needed
                        double time_since_last_spawn = difftime(current_time, processes[i].last_spawned);
                        if (time_since_last_spawn < 5) {
                            printf("[watchdog] Process %s restarting too quickly, waiting...\n", processes[i].name);
                            sleep(5 - (int)time_since_last_spawn);
                        }
                        
                        // Respawn the process
                        printf("[watchdog] Respawning %s process\n", processes[i].name);
                        if (spawn_process(&processes[i])) {
                            printf("[watchdog] %s process respawned successfully with PID %d\n", 
                                   processes[i].name, processes[i].pid);
                            
                            // Allow a short delay for the process to initialize
                            usleep(500000);  // 500ms
                            
                            // Process respawned successfully, now send configuration to it
                            if (processes[i].type == PROC_REVERSE_PROXY || processes[i].type == PROC_LOAD_BALANCER) {
                                printf("[watchdog] Sending configuration data to respawned %s\n", processes[i].name);
                                // Force an update for this newly respawned process
                                update_port_environment();
                                
                                // Add an additional verification check to ensure SIGHUP was received
                                usleep(200000);  // 200ms delay
                                
                                // If this is a respawned process, send the signal again to be safe
                                if (processes[i].type == PROC_REVERSE_PROXY) {
                                    kill(processes[i].pid, SIGHUP);
                                    printf("[watchdog] Sent additional SIGHUP to respawned %s (PID: %d)\n", 
                                           processes[i].name, processes[i].pid);
                                } else if (processes[i].type == PROC_LOAD_BALANCER) {
                                    kill(processes[i].pid, SIGHUP);
                                    printf("[watchdog] Sent additional SIGHUP to respawned %s (PID: %d)\n", 
                                           processes[i].name, processes[i].pid);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        
        // Periodic checks (still useful as a safety net)
        // Check all processes in case some died without triggering SIGCHLD
        for (int i = 0; i < process_count; i++) {
            if (processes[i].active && !is_process_running(processes[i].pid)) {
                printf("[watchdog] Detected dead process %s (PID: %d) without notification\n", 
                       processes[i].name, processes[i].pid);
                processes[i].active = false;
                
                // Respawn the process
                printf("[watchdog] Respawning %s process\n", processes[i].name);
                if (spawn_process(&processes[i])) {
                    printf("[watchdog] %s process respawned successfully with PID %d\n", 
                           processes[i].name, processes[i].pid);
                    
                    // Allow a short delay for the process to initialize
                    usleep(500000);  // 500ms
                    
                    // Process respawned successfully, now send configuration to it
                    if (processes[i].type == PROC_REVERSE_PROXY || processes[i].type == PROC_LOAD_BALANCER) {
                        printf("[watchdog] Sending configuration data to respawned %s\n", processes[i].name);
                        // Force an update for this newly respawned process
                        update_port_environment();
                        
                        // Add an additional verification check to ensure SIGHUP was received
                        usleep(200000);  // 200ms delay
                        
                        // If this is a respawned process, send the signal again to be safe
                        if (processes[i].type == PROC_REVERSE_PROXY) {
                            kill(processes[i].pid, SIGHUP);
                            printf("[watchdog] Sent additional SIGHUP to respawned %s (PID: %d)\n", 
                                   processes[i].name, processes[i].pid);
                        } else if (processes[i].type == PROC_LOAD_BALANCER) {
                            kill(processes[i].pid, SIGHUP);
                            printf("[watchdog] Sent additional SIGHUP to respawned %s (PID: %d)\n", 
                                   processes[i].name, processes[i].pid);
                        }
                    }
                }
            }
        }
        
        // Sleep for the check interval
        sleep(CHECK_INTERVAL);
    }
}

// Clean up resources
void cleanup() {
    // Send SIGTERM to all child processes
    for (int i = 0; i < process_count; i++) {
        if (processes[i].active && processes[i].pid > 0) {
            kill(processes[i].pid, SIGTERM);
        }
        
        // Close any open pipe file descriptors
        if (processes[i].pipe_fd[0] >= 0) {
            close(processes[i].pipe_fd[0]);
        }
        if (processes[i].config_pipe[1] >= 0) {
            close(processes[i].config_pipe[1]);
        }
        
        // Free allocated argument arrays for all processes
        if (processes[i].args) {
            for (int j = 0; processes[i].args[j] != NULL; j++) {
                free(processes[i].args[j]);
            }
            free(processes[i].args);
        }
    }
    
    free(processes);
}

int main() {
    // Setup signal handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    
    // Install signal handlers
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGCHLD, &sa, NULL);
    
    printf("[watchdog] Starting watchdog process\n");
    
    // Initialize process structures
    init_processes();

    printf("[watchdog] Starting load balancer first\n");
    
    // Spawn the load balancer FIRST with non-workable state
    Process* lb_process = &processes[0];
    if (!spawn_process(lb_process)) {
        printf("[watchdog] Failed to spawn load balancer\n");
        cleanup();
        return EXIT_FAILURE;
    }
    
    sleep(1); // Allow load balancer time to initialize
    
    printf("[watchdog] Starting reverse proxies\n");
    
    // Spawn reverse proxies SECOND with non-workable state
    for (int i = 1; i <= 2; i++) {
        Process* rp_process = &processes[i];
        if (!spawn_process(rp_process)) {
            printf("[watchdog] Failed to spawn reverse proxy %d\n", i);
            cleanup();
            return EXIT_FAILURE;
        }
    }
    
    sleep(1); // Allow reverse proxies time to initialize
    
    printf("[watchdog] Starting server workers\n");
    
    // Start server workers LAST
    // Server processes are at indexes 3-8
    for (int i = 3; i < process_count; i++) {
        Process* srv_process = &processes[i];
        if (!spawn_process(srv_process)) {
            printf("[watchdog] Failed to spawn server worker %d\n", i-2);
            cleanup();
            return EXIT_FAILURE;
        }
    }
    
    sleep(1); // Allow servers time to initialize and register their ports
    
    // Update all environment variables after all components are started
    // This sends configuration to each component and signals them to become operational
    printf("[watchdog] All components started, sending configuration signals\n");
    update_port_environment();

    
    // Main monitoring loop
    monitor_processes();
    
    // Cleanup
    cleanup();
    
    printf("[watchdog] Watchdog shut down successfully\n");
    return EXIT_SUCCESS;
}