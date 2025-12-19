/**
 * @file aesdsocket.c
 * @brief Multi-threaded socket server implementation for AESD assignment
 *
 * Opens a stream socket on port 9000, accepts connections, receives data,
 * appends to /var/tmp/aesdsocketdata, and sends the file content back.
 * Supports daemon mode with -d argument.
 * Supports multiple simultaneous connections with threading.
 * Appends timestamp every 10 seconds.
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
#include <pthread.h>
#include <time.h>
#include <sys/queue.h>

#define PORT 9000
#define DATA_FILE "/var/tmp/aesdsocketdata"
#define BUFFER_SIZE 1024
#define TIMESTAMP_INTERVAL 10

// Thread data structure
typedef struct thread_data {
    pthread_t thread_id;
    int client_fd;
    struct sockaddr_in client_addr;
    bool thread_complete;
    SLIST_ENTRY(thread_data) entries;
} thread_data_t;

// Global variables for signal handling
static int server_fd = -1;
static volatile sig_atomic_t caught_signal = 0;

// Mutex for file access synchronization
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread list head
static SLIST_HEAD(thread_list_head, thread_data) thread_list_head;
static pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

// Timer
static timer_t timerid;


/**
 * Timer signal handler - appends timestamp to file
 */
void timer_handler(union sigval sv)
{
    time_t now;
    struct tm *tm_info;
    char timestamp[200];
    
    time(&now);
    tm_info = localtime(&now);
    
    // RFC 2822 compliant format
    strftime(timestamp, sizeof(timestamp), "timestamp:%a, %d %b %Y %H:%M:%S %z\n", tm_info);
    
    pthread_mutex_lock(&file_mutex);
    
    FILE *fp = fopen(DATA_FILE, "a");
    if (fp != NULL) {
        fputs(timestamp, fp);
        fclose(fp);
    } else {
        syslog(LOG_ERR, "Failed to open file for timestamp: %s", strerror(errno));
    }
    
    pthread_mutex_unlock(&file_mutex);
}

/**
 * Initialize timer for periodic timestamps
 */
int init_timer(void)
{
    struct sigevent sev;
    struct itimerspec its;
    
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timer_handler;
    sev.sigev_value.sival_ptr = &timerid;
    
    if (timer_create(CLOCK_REALTIME, &sev, &timerid) == -1) {
        syslog(LOG_ERR, "Failed to create timer: %s", strerror(errno));
        return -1;
    }
    
    // Set timer to fire every 10 seconds
    its.it_value.tv_sec = TIMESTAMP_INTERVAL;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = TIMESTAMP_INTERVAL;
    its.it_interval.tv_nsec = 0;
    
    if (timer_settime(timerid, 0, &its, NULL) == -1) {
        syslog(LOG_ERR, "Failed to set timer: %s", strerror(errno));
        timer_delete(timerid);
        return -1;
    }
    
    return 0;
}

/**
 * Signal handler for SIGINT and SIGTERM
 */
void signal_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM) {
        syslog(LOG_INFO, "Caught signal, exiting");
        caught_signal = 1;
        
        // Shutdown server socket to unblock accept()
        if (server_fd != -1) {
            shutdown(server_fd, SHUT_RDWR);
        }
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
    thread_data_t *thread_item;
    
    // Stop timer
    timer_delete(timerid);
    
    // Join all threads
    pthread_mutex_lock(&thread_list_mutex);
    SLIST_FOREACH(thread_item, &thread_list_head, entries) {
        if (!thread_item->thread_complete) {
            shutdown(thread_item->client_fd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&thread_list_mutex);
    
    // Wait for all threads to complete
    pthread_mutex_lock(&thread_list_mutex);
    while (!SLIST_EMPTY(&thread_list_head)) {
        thread_item = SLIST_FIRST(&thread_list_head);
        pthread_mutex_unlock(&thread_list_mutex);
        
        pthread_join(thread_item->thread_id, NULL);
        
        pthread_mutex_lock(&thread_list_mutex);
        SLIST_REMOVE_HEAD(&thread_list_head, entries);
        close(thread_item->client_fd);
        free(thread_item);
    }
    pthread_mutex_unlock(&thread_list_mutex);
    
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
    
    // Delete the data file
    unlink(DATA_FILE);
    
    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&thread_list_mutex);
    
    closelog();
}

/**
 * Send the contents of the data file to the client
 */
int send_file_to_client(int client_socket)
{
    pthread_mutex_lock(&file_mutex);
    
    FILE *fp = fopen(DATA_FILE, "r");
    if (fp == NULL) {
        syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
        pthread_mutex_unlock(&file_mutex);
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
                pthread_mutex_unlock(&file_mutex);
                return -1;
            }
            total_sent += bytes_sent;
        }
    }
    
    fclose(fp);
    pthread_mutex_unlock(&file_mutex);
    return 0;
}

/**
 * Handle a client connection (thread function)
 */
void *handle_client(void *arg)
{
    thread_data_t *thread_data = (thread_data_t *)arg;
    int client_socket = thread_data->client_fd;
    struct sockaddr_in *client_addr = &thread_data->client_addr;
    
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client_addr->sin_addr), client_ip, INET_ADDRSTRLEN);
    
    syslog(LOG_INFO, "Accepted connection from %s", client_ip);
    
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
            break;
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
                break;
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
            
            // Write packet to file with mutex protection
            pthread_mutex_lock(&file_mutex);
            FILE *fp = fopen(DATA_FILE, "a");
            if (fp != NULL) {
                size_t written = fwrite(buffer, 1, packet_size, fp);
                if (written != packet_size) {
                    syslog(LOG_ERR, "Failed to write to file: %s", strerror(errno));
                }
                fclose(fp);
            } else {
                syslog(LOG_ERR, "Failed to open %s: %s", DATA_FILE, strerror(errno));
            }
            pthread_mutex_unlock(&file_mutex);
            
            // Send file content back to client
            if (send_file_to_client(client_socket) == -1) {
                free(buffer);
                syslog(LOG_INFO, "Closed connection from %s", client_ip);
                thread_data->thread_complete = true;
                return NULL;
            }
            
            // Move remaining data to beginning of buffer
            buffer_used -= packet_size;
            if (buffer_used > 0) {
                memmove(buffer, buffer + packet_size, buffer_used);
            }
        }
    }
    
    free(buffer);
    syslog(LOG_INFO, "Closed connection from %s", client_ip);
    thread_data->thread_complete = true;
    
    return NULL;
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
    
    // Initialize thread list
    SLIST_INIT(&thread_list_head);
    
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
    
    // Initialize timer for timestamps
    if (init_timer() == -1) {
        close(server_fd);
        closelog();
        return -1;
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
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (client_fd == -1) {
            if (caught_signal) {
                break;
            }
            syslog(LOG_ERR, "Failed to accept connection: %s", strerror(errno));
            continue;
        }
        
        // Create thread data structure
        thread_data_t *thread_data = malloc(sizeof(thread_data_t));
        if (thread_data == NULL) {
            syslog(LOG_ERR, "Failed to allocate thread data: %s", strerror(errno));
            close(client_fd);
            continue;
        }
        
        thread_data->client_fd = client_fd;
        thread_data->client_addr = client_addr;
        thread_data->thread_complete = false;
        
        // Create thread to handle client
        if (pthread_create(&thread_data->thread_id, NULL, handle_client, thread_data) != 0) {
            syslog(LOG_ERR, "Failed to create thread: %s", strerror(errno));
            close(client_fd);
            free(thread_data);
            continue;
        }
        
        // Add thread to list
        pthread_mutex_lock(&thread_list_mutex);
        SLIST_INSERT_HEAD(&thread_list_head, thread_data, entries);
        pthread_mutex_unlock(&thread_list_mutex);
        
        // Clean up completed threads
        pthread_mutex_lock(&thread_list_mutex);
        thread_data_t *thread_item = SLIST_FIRST(&thread_list_head);
        while (thread_item != NULL) {
            thread_data_t *next = SLIST_NEXT(thread_item, entries);
            if (thread_item->thread_complete) {
                pthread_join(thread_item->thread_id, NULL);
                SLIST_REMOVE(&thread_list_head, thread_item, thread_data, entries);
                close(thread_item->client_fd);
                free(thread_item);
            }
            thread_item = next;
        }
        pthread_mutex_unlock(&thread_list_mutex);
    }
    
    cleanup_and_exit();
    return 0;
}
