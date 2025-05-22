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
    int port;        // Port the process is listening on
} Process;

// Global variables
Process *processes = NULL;
int process_count = 0;
volatile sig_atomic_t running = 1;
volatile sig_atomic_t child_exited = 0;  // Flag to indicate a child has exited

// Signal handler
void handle_signal(int sig) {
    if (sig == SIGTERM || sig == SIGTSTP || sig == SIGINT) {
        printf("[watchdog] Received termination signal, shutting down...\n");
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
    // Create a pipe for IPC
    if (pipe(proc->pipe_fd) < 0) {
        perror("[watchdog] Pipe creation failed");
        return false;
    }

    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        perror("[watchdog] Fork failed");
        close(proc->pipe_fd[0]);
        close(proc->pipe_fd[1]);
        return false;
    } else if (pid == 0) {
        // Child process
        // Close read end, keep write end
        close(proc->pipe_fd[0]);
        
        // Pass the pipe FD as an environment variable so the child knows where to report its port
        char pipe_fd_str[16];
        snprintf(pipe_fd_str, sizeof(pipe_fd_str), "%d", proc->pipe_fd[1]);
        setenv("WATCHDOG_PIPE_FD", pipe_fd_str, 1);
        
        // Add process type as environment variable
        char proc_type_str[2];
        snprintf(proc_type_str, sizeof(proc_type_str), "%d", (int)proc->type);
        setenv("PROCESS_TYPE", proc_type_str, 1);
        
        execv(proc->path, proc->args);
        // If execv returns, it failed
        perror("[watchdog] execv failed");
        close(proc->pipe_fd[1]);
        exit(EXIT_FAILURE);
    } else {
        // Parent process
        // Close write end, keep read end
        close(proc->pipe_fd[1]);
        
        proc->pid = pid;
        proc->active = true;
        proc->last_spawned = time(NULL);
        proc->port = 0;  // Will be set when child reports back
        
        // For non-load balancer processes, wait for port information
        if (proc->type != PROC_LOAD_BALANCER) {
            char buffer[32];
            ssize_t bytes_read = read(proc->pipe_fd[0], buffer, sizeof(buffer) - 1);
            
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
                proc->port = atoi(buffer);
                // Removed port logging
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

// Update environment variables with current ports
void update_port_environment() {
    int proxy_count = 0;
    int server_count = 0;
    char env_name[32];
    char env_value[16];
    
    for (int i = 0; i < process_count; i++) {
        // Skip processes without a valid port
        if (processes[i].port <= 0) continue;
        
        // Set environment variables based on process type
        if (processes[i].type == PROC_REVERSE_PROXY) {
            proxy_count++;
            snprintf(env_name, sizeof(env_name), "REVERSE_PROXY_PORT_%d", proxy_count);
            snprintf(env_value, sizeof(env_value), "%d", processes[i].port);
            setenv(env_name, env_value, 1);  // Overwrite any existing value
            // Removed environment variable logging
        }
        else if (processes[i].type == PROC_SERVER) {
            server_count++;
            snprintf(env_name, sizeof(env_name), "SERVER_PORT_%d", server_count);
            snprintf(env_value, sizeof(env_value), "%d", processes[i].port);
            setenv(env_name, env_value, 1);  // Overwrite any existing value
            // Removed environment variable logging
        }
    }
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
                        spawn_process(&processes[i]);
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
                spawn_process(&processes[i]);
            }
        }
        
        // Update environment variables with current port mappings
        update_port_environment();
        
        sleep(CHECK_INTERVAL);
    }
}

// Clean up resources
void cleanup() {
    printf("[watchdog] Terminating all child processes...\n");
    
    // Send SIGTERM to all child processes
    for (int i = 0; i < process_count; i++) {
        if (processes[i].active && processes[i].pid > 0) {
            printf("[watchdog] Sending SIGTERM to %s (PID: %d)\n", processes[i].name, processes[i].pid);
            kill(processes[i].pid, SIGTERM);
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

    printf("[watchdog] Starting load balancer\n");

    // Spawn the load balancer first
    Process* lb_process = &processes[0];
    if (!spawn_process(lb_process)) {
        printf("[watchdog] Failed to spawn load balancer\n");
        cleanup();
        return EXIT_FAILURE;
    }

    printf("[watchdog] Starting 2 reverse proxies\n");

    // Spawn reverse proxies
    for (int i = 1; i <= 2; i++) {
        Process* rp_process = &processes[i];
        if (!spawn_process(rp_process)) {
            printf("[watchdog] Failed to spawn reverse proxy %d\n", i);
            cleanup();
            return EXIT_FAILURE;
        }
    }

    printf("[watchdog] Starting 6 server workers\n");

    // Spawn server workers
    for (int i = 3; i < process_count; i++) {
        Process* srv_process = &processes[i];
        if (!spawn_process(srv_process)) {
            printf("[watchdog] Failed to spawn server worker %d\n", i);
            cleanup();
            return EXIT_FAILURE;
        }
    }
    
    // Main monitoring loop
    monitor_processes();
    
    // Cleanup
    cleanup();
    
    printf("[watchdog] Watchdog shut down successfully\n");
    return EXIT_SUCCESS;
}