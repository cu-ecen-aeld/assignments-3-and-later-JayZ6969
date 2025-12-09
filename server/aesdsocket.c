/**
 * @file aesdsocket.c
 * @brief Socket server implementation for AESD assignment
 *
 * Opens a stream socket on port 9000, accepts connections, receives data,
 * appends to /var/tmp/aesdsocketdata, and sends the file content back.
 * Supports daemon mode with -d argument.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024

// Global variables for signal handling
static int server_fd = -1;
static int client_fd = -1;
static volatile sig_atomic_t caught_signal = 0;

/**
 * Signal handler for SIGINT and SIGTERM
 */
void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_signal = 1;
    }
}

/**
 * Setup signal handlers for SIGINT and SIGTERM
 */
int setup_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGINT, &sa, NULL) == -1) {
        perror("sigaction SIGINT");
        return -1;
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction SIGTERM");
        return -1;
    }

    return 0;
}

/**
 * Cleanup resources and exit
 */
void cleanup_and_exit(void)
{
    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }
    
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    
    // Delete the data file
    unlink(DATA_FILE);
    
    closelog();
}

/**
 * Send the contents of the data file to the client
 */
int send_file_to_client(int client_socket)
{
    FILE *fp = fopen(DATA_FILE, "r");
    if (fp == NULL) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        return -1;
    }

    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), fp)) > 0) {
        ssize_t bytes_sent = 0;
        ssize_t total_sent = 0;
        
        while (total_sent < bytes_read) {
            bytes_sent = send(client_socket, buffer + total_sent, 
                            bytes_read - total_sent, 0);
            if (bytes_sent == -1) {
                syslog(LOG_ERR, "Failed to send data: %s", strerror(errno));
                fclose(fp);
                return -1;
            }
            total_sent += bytes_sent;
        }
    }
    
    fclose(fp);
    return 0;
}

/**
 * Handle a client connection
 */
int handle_client(int client_socket, struct sockaddr_in *client_addr)
{
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);
    
    // Open the data file in append mode
    FILE *fp = fopen(DATA_FILE, "a");
    if (fp == NULL) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        syslog(LOG_INFO, "Closed connection from %s", client_ip);
        return -1;
    }
    
    char *buffer = NULL;
    size_t buffer_size = 0;
    size_t buffer_used = 0;
    char recv_buffer[BUFFER_SIZE];
    ssize_t bytes_received;
    
    // Receive data until connection closes
    while (!caught_signal) {
        bytes_received = recv(client_socket, recv_buffer, sizeof(recv_buffer), 0);
        
        if (bytes_received < 0) {
            syslog(LOG_ERR, "Failed to receive data: %s", strerror(errno));
            free(buffer);
            fclose(fp);
            syslog(LOG_INFO, "Closed connection from %s", client_ip);
            return -1;
        } else if (bytes_received == 0) {
            // Connection closed by client
            break;
        }
        
        // Expand buffer if needed
        if (buffer_used + bytes_received > buffer_size) {
            size_t new_size = buffer_size + BUFFER_SIZE;
            char *new_buffer = realloc(buffer, new_size);
            if (new_buffer == NULL) {
                syslog(LOG_ERR, "Failed to allocate memory: %s", strerror(errno));
                free(buffer);
                fclose(fp);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                return -1;
            }
            buffer = new_buffer;
            buffer_size = new_size;
        }
        
        // Copy received data to buffer
        memcpy(buffer + buffer_used, recv_buffer, bytes_received);
        buffer_used += bytes_received;
        
        // Check for newline character - process all complete packets
        char *newline_pos;
        while ((newline_pos = memchr(buffer, '\n', buffer_used)) != NULL) {
            // Calculate packet size including newline
            size_t packet_size = newline_pos - buffer + 1;
            
            // Write packet to file
            size_t written = fwrite(buffer, 1, packet_size, fp);
            if (written != packet_size) {
                syslog(LOG_ERR, "Failed to write to file: %s", strerror(errno));
                free(buffer);
                fclose(fp);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                return -1;
            }
            fflush(fp);
            
            // Close file for writing, send content back, then reopen for append
            fclose(fp);
            
            if (send_file_to_client(client_socket) == -1) {
                free(buffer);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                return -1;
            }
            
            // Reopen file for next packet
            fp = fopen(DATA_FILE, "a");
            if (fp == NULL) {
                syslog(LOG_ERR, "Failed to reopen %s: %s", DATA_FILE, strerror(errno));
                free(buffer);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                return -1;
            }
            
            // Move remaining data to beginning of buffer
            buffer_used -= packet_size;
            if (buffer_used > 0) {
                memmove(buffer, buffer + packet_size, buffer_used);
            }
        }
    }
    
    free(buffer);
    fclose(fp);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    
    return 0;
}

/**
 * Run as daemon process
 */
int daemonize(void)
{
    pid_t pid = fork();
    
    if (pid < 0) {
        syslog(LOG_ERR, "Failed to fork: %s", strerror(errno));
        return -1;
    }
    
    if (pid > 0) {
        // Parent process exits
        exit(EXIT_SUCCESS);
    }
    
    // Child process continues
    // Create new session
    if (setsid() < 0) {
        syslog(LOG_ERR, "Failed to create new session: %s", strerror(errno));
        return -1;
    }
    
    // Change working directory to root
    if (chdir("/") < 0) {
        syslog(LOG_ERR, "Failed to change directory: %s", strerror(errno));
        return -1;
    }
    
    // Redirect standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    open("/dev/null", O_RDONLY); // stdin
    open("/dev/null", O_WRONLY); // stdout
    open("/dev/null", O_WRONLY); // stderr
    
    return 0;
}

int main(int argc, char *argv[])
{
    bool daemon_mode = false;
    int opt;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_addr_len;
    int reuse = 1;
    
    // Open syslog
    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
    
    // Parse command line arguments
    while ((opt = getopt(argc, argv, "d")) != -1) {
        switch (opt) {
            case 'd':
                daemon_mode = true;
                break;
            default:
                fprintf(stderr, "Usage: %s [-d]\n", argv[0]);
                closelog();
                return -1;
        }
    }
    
    // Setup signal handlers
    if (setup_signal_handlers() == -1) {
        closelog();
        return -1;
    }
    
    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        syslog(LOG_ERR, "Failed to create socket: %s", strerror(errno));
        closelog();
        return -1;
    }
    
    // Set socket options to reuse address
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        syslog(LOG_ERR, "Failed to set socket options: %s", strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }
    
    // Bind socket to port 9000
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        syslog(LOG_ERR, "Failed to bind to port %d: %s", PORT, strerror(errno));
        close(server_fd);
        closelog();
        return -1;
    }
    
    // Run as daemon if requested
    if (daemon_mode) {
        if (daemonize() == -1) {
            close(server_fd);
            closelog();
            return -1;
        }
    }
    
    // Listen for connections
    if (listen(server_fd, 10) == -1) {
        syslog(LOG_ERR, "Failed to listen: %s", strerror(errno));
        cleanup_and_exit();
        return -1;
    }
    
    // Accept connections in a loop
    while (!caught_signal) {
        client_addr_len = sizeof(client_addr);
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_fd == -1) {
            if (caught_signal) {
                break;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }
        
        handle_client(client_fd, &client_addr);
        
        close(client_fd);
        client_fd = -1;
    }
    
    cleanup_and_exit();
    return 0;
}
