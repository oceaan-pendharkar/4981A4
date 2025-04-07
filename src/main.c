#include "../include/http.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <ndbm.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
    #include <sys/epoll.h>
#elif defined(__FreeBSD__) || defined(__APPLE__)
    #include <sys/event.h>
#endif

#define PORT 8080
#define BUFFER_SIZE 1024
// #define children 3
#define TIME_SIZE 20
#define FOUR 4
#define FIVE 5
#define BASE_TEN 10
#define RELOAD_MSG 60

static void           setup_signal_handler(void);
static void           sigint_handler(int signum);
static int            handle_request(struct sockaddr_in client_addr, int client_fd, void *handle);
static int            recv_fd(int socket);
static int            send_fd(int socket, int fd);
static time_t         get_last_modified_time(const char *path);
static void           format_timestamp(time_t timestamp, char *buffer, size_t buffer_size);
static int            worker_loop(time_t last_time, void *handle, int i, int client_sockets[], int **worker_sockets);
static void           check_for_dead_children(time_t last_time, void *handle, int client_sockets[], int **worker_sockets, int child_pids[], int children);
static void           clean_up_worker_sockets(int **worker_sockets, int children);
static int            call_handle_client(int (*handle_c)(int, const char *, int, int), void *handle, int client_fd, char *req_path, int is_head, int is_img);
static int            call_set_request_path(void (*set_req_path)(const char *, const char *), void *handle, char *req_path, char *buffer);
static int            call_is_http(int (*is_http)(const char *), void *handle, char *buffer);
_Noreturn static void usage(const char *program_name, int exit_code, const char *message);
static int            parse_positive_int(const char *binary_name, const char *str);
static void           handle_arguments(const char *binary_name, const char *children_str, int *children);
static void           parse_arguments(int argc, char *argv[], char **children);

// this variable should not be moved to a .h file
static volatile sig_atomic_t exit_flag = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

int main(int argc, char *argv[])
{
    void              *handle;
    fd_set             readfds;                                        // Set of file descriptors for select
    size_t             max_clients;                                    // Maximum number of clients that can connect
    struct sockaddr_in host_addr;                                      // Server's address structure
    unsigned int       host_addrlen;                                   // Length of the server address
    int               *client_sockets = (int *)malloc(sizeof(int));    // Array of active client sockets
    int                sd             = 0;                             // Temp variable for socket descriptor
    int                max_fd;                                         // Maximum file descriptor for select
    int                dsfd[2];                                        // the domain socket for server->monitor
    int              **worker_sockets;                                 // Stores UNIX socket pairs for each worker
    pid_t             *child_pids;
    pid_t              monitor;
    int                server_fd;
    time_t             last_modified;
    char               time_str[TIME_SIZE];
    char               cwd[BUFFER_SIZE];
    char              *children_str = NULL;
    int                children;

    if(getcwd(cwd, sizeof(cwd)) != NULL)
    {
        printf("Current working directory: %s\n", cwd);
    }
    else
    {
        perror("getcwd() error");
    }

    parse_arguments(argc, argv, &children_str);
    handle_arguments(argv[0], children_str, &children);

    child_pids = (pid_t *)malloc((size_t)children * sizeof(pid_t));
    if(child_pids == NULL)
    {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    // initialize shared library
    handle = dlopen("./http.so", RTLD_NOW);
    if(!handle)
    {
        free((void *)client_sockets);
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        free(child_pids);
        return 1;
    }

    // Grab last modified time of http.so
    last_modified = get_last_modified_time("./http.so");
    format_timestamp(last_modified, time_str, sizeof(time_str));     // Convert to human readable
    printf("http.so last modified time on init: %s\n", time_str);    // Testing

    // create domain socket for server -> monitor
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, dsfd) == -1)
    {
        perror("webserver (socketpair)");
        free(client_sockets);
        dlclose(handle);
        free(child_pids);
        return 1;
    }

    // create domain socket for monitor -> worker
    // this is my solution for making sure only one child gets a fd from the monitor
    worker_sockets = (int **)malloc(sizeof(int *) * (size_t)children);
    if(!worker_sockets)
    {
        perror("malloc failed");
        free(child_pids);
        exit(EXIT_FAILURE);
    }
    for(int i = 0; i < children; i++)
    {
        worker_sockets[i] = (int *)malloc(sizeof(int) * 2);
        if(!worker_sockets[i])
        {
            perror("malloc failed");
            free(child_pids);
            exit(EXIT_FAILURE);
        }
        if(socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]) == -1)
        {
            perror("webserver (socketpair)");
            free(client_sockets);
            dlclose(handle);
            for(int j = i; j >= 0; j--)
            {
                free(worker_sockets[j]);
            }
            free((void *)worker_sockets);
            free(child_pids);

            return 1;
        }
    }

    // fork the monitor
    monitor = fork();
    if(monitor == -1)
    {
        perror("fork");
        free(child_pids);
        exit(EXIT_FAILURE);
    }
    if(monitor == 0)
    {
        int worker_index = 0;    // Replace with actual worker selection logic

        // close the end of ds we're going to monitor for in select
        close(dsfd[0]);

        // pre-fork children
        for(int i = 0; i < children; i++)
        {
            pid_t pid = fork();
            if(pid < 0)
            {
                perror("webserver (fork)");
                dlclose(handle);
                free(client_sockets);
                clean_up_worker_sockets(worker_sockets, children);
                free(child_pids);
                return 1;
            }
            if(pid == 0)
            {
                // Get last time http.so was modified
                time_t last_time = get_last_modified_time("./http.so");

                int result = worker_loop(last_time, handle, i, client_sockets, worker_sockets);
                if(result != 0)
                {
                    perror("webserver (worker loop)");
                    free(child_pids);
                    exit(EXIT_FAILURE);
                }
            }
            else
            {
                child_pids[i] = pid;
                close(worker_sockets[i][1]);    // Close worker's end in monitor
            }
        }

        // monitor code
        while(1)
        {
            fd_set monitor_read_fds;
            int    max_monitor_fd = dsfd[1];
            int    monitor_activity;

            memset(&monitor_read_fds, 0, sizeof(monitor_read_fds));

            // Listen for new client FDs from server
            FD_SET(dsfd[1], &monitor_read_fds);

            // Listen for worker responses
            for(int i = 0; i < children; i++)
            {
                FD_SET(worker_sockets[i][0], &monitor_read_fds);
                if(worker_sockets[i][0] > max_monitor_fd)
                {
                    max_monitor_fd = worker_sockets[i][0];
                }
            }

            // Wait for an event on any socket
            monitor_activity = select(max_monitor_fd + 1, &monitor_read_fds, NULL, NULL, NULL);
            if(monitor_activity < 0)
            {
                perror("select error in monitor");
                continue;
            }

            // Receive client FD from server
            if(FD_ISSET(dsfd[1], &monitor_read_fds))
            {
                int client_fd_monitor = recv_fd(dsfd[1]);
                if(client_fd_monitor > 0)
                {
                    // printf("Monitor received client FD %d from server\n", client_fd_monitor);

                    // Send the FD to a worker (Round-robin or first available)
                    send_fd(worker_sockets[worker_index][0], client_fd_monitor);
                    // printf("Monitor sent client FD %d to worker %d\n", client_fd_monitor, worker_index);
                    close(client_fd_monitor);
                    worker_index++;
                    if(worker_index == children)
                    {
                        worker_index = 0;
                    }
                }
            }

            // Receive processed FDs from workers
            for(int i = 0; i < children; i++)
            {
                if(FD_ISSET(worker_sockets[i][0], &monitor_read_fds))
                {
                    int returned_fd = recv_fd(worker_sockets[i][0]);
                    if(returned_fd > 0)
                    {
                        // printf("Monitor received processed FD %d from worker %d\n", returned_fd, i);
                        send_fd(dsfd[1], returned_fd);
                        // printf("Monitor sent fd %d back to server\n", returned_fd);
                        close(returned_fd);    // Clean up after worker has finished
                    }
                }
            }
            check_for_dead_children(last_modified, handle, client_sockets, worker_sockets, child_pids, children);
        }
    }
    // Create a TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);    // NOLINT(android-cloexec-socket)
    if(server_fd == -1)
    {
        perror("webserver (socket)");
        free(client_sockets);
        clean_up_worker_sockets(worker_sockets, children);
        free(child_pids);
        return 1;
    }

    // (Debugging) Print program arguments
    // printf("program arg: %d\n", argc);
    // printf("program argv[0]: %s\n", argv[0]);

    // Set up Signal Handler
    setup_signal_handler();

    // Initialize client socket, address and number as zero or null
    //    client_sockets = NULL;
    max_clients = 0;

    // Create the address to bind the socket to
    // Initialize the server address structure
    host_addrlen = sizeof(host_addr);

    // Use IPv4 to set the server port and bind to available network interface
    host_addr.sin_family      = AF_INET;
    host_addr.sin_port        = htons(PORT);
    host_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    // Bind the socket to the server address
    if(bind(server_fd, (struct sockaddr *)&host_addr, host_addrlen) != 0)
    {
        perror("webserver (bind)");
        close(server_fd);
        free(client_sockets);
        dlclose(handle);
        clean_up_worker_sockets(worker_sockets, children);
        free(child_pids);
        return 1;
    }
    // printf("Socket successfully bound to address\n");

    // Listen for incoming connections
    if(listen(server_fd, SOMAXCONN) != 0)
    {
        perror("webserver (listen)");
        close(server_fd);
        free(client_sockets);
        dlclose(handle);
        clean_up_worker_sockets(worker_sockets, children);
        free(child_pids);
        return 1;
    }
    printf("Server listening for connections\n\n");

    // Clear the socket set
#ifndef __clang_analyzer__
    memset(&readfds, 0, sizeof(readfds));

#endif

#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
    // Add the server socket to the set
    FD_SET(server_fd, &readfds);
    // Add the domain socket to the set (for when worker is done with client fd and sends it back)
    FD_SET(dsfd[0], &readfds);
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
    max_fd = server_fd;

    // printf("entering loop\n\n");
    while(!exit_flag)
    {
        int activity;    // Number of ready file descriptors

        // printf("adding client sockets\n");
        //  Add the client sockets to the set
        for(size_t i = 0; i < max_clients; i++)
        {
            sd = client_sockets[i];
            if(sd > 0)
            {
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
                FD_SET(sd, &readfds);
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
            }
            if(sd > max_fd)
            {
                max_fd = sd;
            }
        }

        // printf("maxfd: %d\n", max_fd);

        // Wait for activity on one of the monitored sockets
        activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if(activity < 0)
        {
            perror("Select error");
            continue;
        }

        // printf("select ok\n\n");

        if(FD_ISSET(server_fd, &readfds))
        {
            int *temp;
            // Accept incoming connections
            int newsockfd = accept(server_fd, (struct sockaddr *)&host_addr, (socklen_t *)&host_addrlen);
            if(newsockfd < 0)
            {
                perror("webserver (accept)");
                continue;
            }
            printf("Connection Accepted\n");

            // Increase the size of the client_sockets array
            max_clients++;
            temp = (int *)realloc(client_sockets, sizeof(int) * max_clients);

            if(temp == NULL)
            {
                perror("realloc");
                free(client_sockets);
                free(child_pids);
                exit(EXIT_FAILURE);
            }
            else
            {
                client_sockets                  = temp;
                client_sockets[max_clients - 1] = newsockfd;
            }
            // printf("Sending client fd %d\n", newsockfd);
            send_fd(dsfd[0], newsockfd);
            close(newsockfd);

#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
            FD_CLR(newsockfd, &readfds);    // Remove the closed socket from the set
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
            client_sockets[max_clients - 1] = -1;
        }

        if(FD_ISSET(dsfd[0], &readfds))
        {
            int fd_from_monitor;
            int added = 0;

            // printf("received fd from monitor on domain socket\n");
            fd_from_monitor = recv_fd(dsfd[0]);
            // printf("received fd from monitor: %d\n", fd_from_monitor);

// Add the FD back to readfds after getting it from the worker
#if (defined(__APPLE__) && defined(__MACH__))
            FD_SET(fd_from_monitor, &readfds);
#endif

#if defined(__linux__)
            if(fd_from_monitor >= 0)
            {
                FD_SET(fd_from_monitor, &readfds);
                if(fd_from_monitor > max_fd)
                {
                    max_fd = fd_from_monitor;
                }
            }
            else
            {
                fprintf(stderr, "Warning: fd_from_monitor was negative: %d\n", fd_from_monitor);
            }

#endif

            if(fd_from_monitor > max_fd)
            {
                max_fd = fd_from_monitor;
            }

            // Add it back to client_sockets
            for(size_t i = 0; i < max_clients; i++)
            {
                if(client_sockets[i] <= 0)
                {
                    client_sockets[i] = fd_from_monitor;
                    added             = 1;
                    break;
                }
            }
            if(!added)
            {
                printf("No space in client_sockets to re-add fd %d\n", fd_from_monitor);
            }
        }
    }
    close(server_fd);
    dlclose(handle);    // close shared library handle
    free((void *)client_sockets);

    // close domain socket fds
    close(dsfd[0]);
    close(dsfd[1]);

    clean_up_worker_sockets(worker_sockets, children);
    free(child_pids);

    return EXIT_SUCCESS;
}

/*
    Sets up the signal handler
 */
static void setup_signal_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
#endif
    sa.sa_handler = sigint_handler;
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif
    sigaction(SIGINT, &sa, NULL);
}

/*
    Updates the signal flag upon receiving the exit signal

    @param signum: The signal number received
 */
static void sigint_handler(int signum)
{
    (void)signum;
    exit_flag = 1;
}

/*
    Processes an HTTP request from the client and sends the appropriate response.

    @param
    client_addr: Client address info
    client_fd: File descriptor for the client connection
    handle: Handle to the shared library for dynamic function calls
 */
static int handle_request(struct sockaddr_in client_addr, int client_fd, void *handle)
{
    int (*handle_c)(int, const char *, int, int)     = NULL;    // function pointer for handle_client
    void (*set_req_path)(const char *, const char *) = NULL;    // function pointer for set_request_path
    int (*is_http_req)(const char *)                 = NULL;    // function pointer for set_request_path

    char buffer[BUFFER_SIZE] = {0};    // Buffer for storing incoming data
    int  is_http;
    int  is_head;
    int  is_img;
    char req_path[BUFFER_SIZE];    // Path of the requested file
    int  retval;

    ssize_t valread;
    printf("[%s:%u]\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    // Read client request
    valread = read(client_fd, buffer, BUFFER_SIZE);
    if(valread < 0)
    {
        perror("webserver (read)");
        return 1;
    }

    is_http = call_is_http(is_http_req, handle, buffer);
    if(is_http < 0)
    {
        printf("gets 400 file path and isn't proper http request\n");
        strncpy(req_path, "/400.txt", LEN_405);
        req_path[TEN] = '\0';
        return call_handle_client(handle_c, handle, client_fd, req_path, -1, -1);
    }

    retval = call_set_request_path(set_req_path, handle, req_path, buffer);
    if(retval == 1)
    {
        printf("could not call set request path from library\n");
        return 1;
    }

    // printf("\nrequest path generated: %s\n", req_path);

    // Detect HTTP Method
    if(strncmp(buffer, "POST ", FIVE) == 0)
    {
        int (*handle_post_lib)(const char *, int) = NULL;

        *(void **)(&handle_post_lib) = dlsym(handle, "handle_post_request");
        if(!handle_post_lib)
        {
            fprintf(stderr, "dlsym failed (handle_post_request): %s\n", dlerror());
            return 1;
        }

        printf("POST request detected\n");
        return handle_post_lib(buffer, client_fd);
    }
    if(strncmp(buffer, "HEAD ", FIVE) == 0)
    {
        is_head = 0;    // says it IS a head request if  == 0
        is_img  = -1;

        printf("HEAD request detected\n");

        return call_handle_client(handle_c, handle, client_fd, req_path, is_head, is_img);
    }
    if(strncmp(buffer, "GET ", FOUR) == 0)
    {
        is_head = -1;
        is_img  = -1;

        // printf("Buffer being checked for image: %s\n", buffer);
        if(is_img_request(buffer) == 0)
        {
            is_img = 0;
        }

        printf("GET request detected\n");

        return call_handle_client(handle_c, handle, client_fd, req_path, is_head, is_img);
    }
    if(strncmp(buffer, "HEAD ", FIVE) != 0 && strncmp(buffer, "GET ", FOUR) != 0 && strncmp(buffer, "POST ", FIVE) != 0)
    {
        is_head = -1;
        is_img  = -1;
        strncpy(req_path, "/405.txt", LEN_405);
        req_path[TEN] = '\0';
        return call_handle_client(handle_c, handle, client_fd, req_path, is_head, is_img);
    }

    return 0;
}

/*
    Receives a file descriptor sent over a UNIX domain socket

    @param
    socket: The socket to receive the file descriptor from

    @return
    The received file descriptor, or -1 on error
 */
int recv_fd(int socket)
{
    struct msghdr   msg = {0};
    struct iovec    io  = {0};
    char            buf[1];
    struct cmsghdr *cmsg;
    char            control[CMSG_SPACE(sizeof(int))];
    int             fd;

    io.iov_base    = buf;
    io.iov_len     = sizeof(buf);
    msg.msg_iov    = &io;
    msg.msg_iovlen = 1;

    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    if(recvmsg(socket, &msg, 0) < 0)
    {
        perror("recvmsg");
        close(socket);
        return -1;
    }
    cmsg = CMSG_FIRSTHDR(&msg);

    if(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
    {
        memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
        return fd;
    }
    return -1;
}

/*
    Sends a file descriptor over a UNIX domain socket

    @param
    socket: The socket to send the file descriptor through
    fd: The file descriptor to send

    @return
    0 on success, -1 on error
 */
int send_fd(int socket, int fd)
{
    struct msghdr   msg    = {0};
    struct iovec    io     = {0};
    char            buf[1] = {0};
    struct cmsghdr *cmsg;
    char            control[CMSG_SPACE(sizeof(int))];

    io.iov_base        = buf;
    io.iov_len         = sizeof(buf);
    msg.msg_iov        = &io;
    msg.msg_iovlen     = 1;
    msg.msg_control    = control;
    msg.msg_controllen = sizeof(control);

    cmsg             = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));

    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    if(sendmsg(socket, &msg, 0) < 0)
    {
        perror("sendmsg");
        close(socket);
        return -1;
    }
    return 0;
}

/*
    Retrieves the last modified time of a file

    @param
    path: Path to the file

    @return
    Last modified time on success, 0 on failure
 */
time_t get_last_modified_time(const char *path)
{
    struct stat attr;

    // On success, return last time file was modified
    if(stat(path, &attr) == 0)
    {
        return attr.st_mtime;
    }

    fprintf(stderr, "stat() failed for %s: %s\n", path, strerror(errno));
    // If stat fails, return 0
    return 0;
}

/*
    Formats a time value into a human-readable timestamp string

    @param
    timestamp: The time to format
    buffer: Destination buffer for the formatted string
    buffer_size: Size of the destination buffer
 */
void format_timestamp(time_t timestamp, char *buffer, size_t buffer_size)
{
    struct tm tm_info;
    if(localtime_r(&timestamp, &tm_info))
    {
        strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", &tm_info);
    }
    else
    {
        perror("Unknown time");
    }
}

/*
    Checks for terminated worker processes and restarts them if needed

    @param
    last_time: Timestamp of the last shared library update
    handle: Handle to the shared library
    client_sockets: Array of client socket FDs
    worker_sockets: 2D array of monitor-worker socket pairs
    child_pids: Array of worker process IDs
    children: Total number of worker processes
 */
static void check_for_dead_children(time_t last_time, void *handle, int client_sockets[], int **worker_sockets, int child_pids[], int children)
{
    int dead_worker;
    int status;
    while((dead_worker = waitpid(-1, &status, WNOHANG)) > 0)
    {
        printf("Worker %d died, restarting...\n", dead_worker);

        // Find the corresponding worker index
        for(int i = 0; i < children; i++)
        {
            if(child_pids[i] == dead_worker)
            {
                pid_t new_pid;
                // Restart the worker
                close(worker_sockets[i][0]);
                close(worker_sockets[i][1]);

                // Recreate the socket pair
                if(socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]) == -1)
                {
                    perror("Failed to create worker socket pair");
                    continue;
                }

                new_pid = fork();
                if(new_pid == 0)
                {
                    int result;
                    // Worker process
                    close(worker_sockets[i][0]);                                                   // Close monitor’s end
                    result = worker_loop(last_time, handle, i, client_sockets, worker_sockets);    // Start worker loop
                    if(result != 0)
                    {
                        perror("webserver (worker_loop) failed");
                        exit(1);
                    }
                    exit(EXIT_SUCCESS);
                }
                else if(new_pid > 0)
                {
                    // Monitor process
                    child_pids[i] = new_pid;
                    close(worker_sockets[i][1]);    // Close worker’s end in monitor
                }
                else
                {
                    perror("Failed to restart worker");
                }
                break;
            }
        }
    }
}

/*
    Main loop for a worker process to handle client requests

    @param
    last_time: Last known modification time of the shared library
    handle: Handle to the shared library
    i: Index of the worker process
    client_sockets: Array of client socket FDs
    worker_sockets: 2D array of monitor-worker socket pairs

    @return
    0: Worker loop executed successfully
    1: An error occurred
 */
static int worker_loop(time_t last_time, void *handle, int i, int client_sockets[], int **worker_sockets)
{
    while(!exit_flag)
    {
        int    sockn;
        int    handle_result;
        int    fd;
        time_t new_time;
        char   last_time_str[TIME_SIZE];
        char   new_time_str[TIME_SIZE];
        void (*my_func)(const char *);
        // Create client address
        struct sockaddr_in client_addr;
        unsigned int       client_addrlen = sizeof(client_addr);
        memset(&client_addr, 0, sizeof(client_addr));

        fd = recv_fd(worker_sockets[i][1]);    // recv_fd from monitor
        if(fd == -1)
        {
            perror("webserver: worker (recv_fd)");
            dlclose(handle);
            free(client_sockets);
            return 1;
        }

        // printf("Received client fd in child: %d\n", fd);

        // Check if http.so has been updated
        new_time = get_last_modified_time("./http.so");

        // Testing
        format_timestamp(last_time, last_time_str, sizeof(last_time_str));
        format_timestamp(new_time, new_time_str, sizeof(new_time_str));
        printf("[Worker %d] Checking http.so timestamps\n", i);
        printf("Last: %s | New: %s\n\n", last_time_str, new_time_str);

        if(new_time > last_time)
        {
            char reload_msg[RELOAD_MSG];

            // Close old shared library
            if(handle)
            {
                dlclose(handle);
            }

            // Load newer version
            handle = dlopen("./http.so", RTLD_NOW);
            if(!handle)
            {
                perror("Failed to load shared library");
                free(client_sockets);
                return 1;
            }

            *(void **)(&my_func) = dlsym(handle, "my_function");
            if(!my_func)
            {
                perror("dlsym failed");
                dlclose(handle);
                free(client_sockets);
                return 1;
            }

            // Confirms library was updated
            strcpy(reload_msg, "Shared library updated! Reloading and matching case...");
            my_func(reload_msg);
            printf("\n\n");

            last_time = new_time;
        }

        // Get client address
        sockn = getsockname(fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addrlen);
        if(sockn < 0)
        {
            perror("webserver (getsockname)");
            continue;
        }
        // handle_request()
        handle_result = handle_request(client_addr, fd, handle);
        if(handle_result == 1)
        {
            // todo: kill this process ?
            printf("handle request failed in a child worker\n");
        }
        // printf("fd before sending back to monitor: %d\n", fd);
        //  sendmsg: send the fd back to the monitor
        send_fd(worker_sockets[i][1], fd);
        printf("sent client fd back to monitor: %d\n", fd);
        close(fd);
    }
    return 0;
}

/*
    Closes and frees all worker socket pairs

    @param
    worker_sockets: 2D array of monitor-worker socket pairs
    children: Number of worker processes
 */
static void clean_up_worker_sockets(int **worker_sockets, int children)
{
    for(int i = 0; i < children; i++)
    {
        close(worker_sockets[i][0]);
        close(worker_sockets[i][1]);
        free(worker_sockets[i]);
    }
    free((void *)worker_sockets);
}

/*
    Loads and calls the handle_client function from the shared library

    @param
    handle_c: Function pointer for handle_client
    handle: Handle to the shared library
    client_fd: File descriptor for the client connection
    req_path: Requested file path
    is_head: 0 if HEAD request, -1 otherwise
    is_img: 0 if image request, -1 otherwise

    @return
    0: Success
    1: Error occurred
 */
static int call_handle_client(int (*handle_c)(int, const char *, int, int), void *handle, int client_fd, char *req_path, int is_head, int is_img)
{
    ssize_t valwrite;
    // Retrieve function from shared library
    // printf("retrieving func from shared library\n\n");
    // retrieving symbol (the function name in the lib is my_function)
    *(void **)(&handle_c) = dlsym(handle, "handle_client");
    if(!handle_c)
    {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    // Process and send HTTP response
    // printf("calling func %p\n", *(void **)(&handle_c));
    printf("\n");
    valwrite = handle_c(client_fd, req_path, is_head, is_img);
    if(valwrite < 0)
    {
        return 1;
    }
    return 0;
}

/*
    Loads and calls the set_request_path function from the shared library

    @param
    set_req_path: Function pointer for set_request_path
    handle: Handle to the shared library
    req_path: Output buffer to store the extracted request path
    buffer: HTTP request buffer

    @return
    0: Success
    1: An Error occurred
 */
static int call_set_request_path(void (*set_req_path)(const char *, const char *), void *handle, char *req_path, char *buffer)
{
    // Retrieve function from shared library
    //    printf("retrieving func from shared library\n\n");
    // retrieving symbol (the function name in the lib is my_function)
    *(void **)(&set_req_path) = dlsym(handle, "set_request_path");
    if(!set_req_path)
    {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    printf("calling func set_request_path from shared lib %p\n", *(void **)(&set_req_path));
    printf("\n");
    set_req_path(req_path, buffer);
    return 0;
}

/*
    Loads and calls the is_http_request function from the shared library

    @param
    is_http: Function pointer for is_http_request
    handle: Handle to the shared library
    buffer: HTTP request buffer

    @return
    0: Valid HTTP request
    1: Error occurred or invalid request
 */
static int call_is_http(int (*is_http)(const char *), void *handle, char *buffer)
{
    // Retrieve function from shared library
    //    printf("retrieving func from shared library\n\n");
    // retrieving symbol (the function name in the lib is my_function)
    *(void **)(&is_http) = dlsym(handle, "is_http_request");
    if(!is_http)
    {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    printf("calling func set_request_path from shared lib %p\n", *(void **)(&is_http));
    printf("\n");
    return is_http(buffer);
}

/*
    Parses command-line arguments for program options

    @param
    argc: Argument count
    argv: Argument vector
    children: Output pointer to store the number of child processes (as a string)

 */
static void parse_arguments(int argc, char *argv[], char **children)
{
    int opt;

    opterr = 0;

    while((opt = getopt(argc, argv, "hc:")) != -1)
    {
        switch(opt)
        {
            case 'c':
            {
                *children = optarg;
                break;
            }
            case 'h':
            {
                usage(argv[0], EXIT_SUCCESS, NULL);
            }

            default:
            {
                usage(argv[0], EXIT_FAILURE, NULL);
            }
        }
    }

    if(*children == NULL || *children[0] == '0')
    {
        usage(argv[0], EXIT_FAILURE, "Error: please specify a nonzero number of children to fork");
    }

    if(optind < argc - 1)
    {
        usage(argv[0], EXIT_FAILURE, "Error: Too many arguments.");
    }
}

/*
    Prints usage information and exits the program

    @param
    program_name: Name of the executable
    exit_code: Exit status code
    message: Optional error or help message to display

 */
_Noreturn static void usage(const char *program_name, int exit_code, const char *message)
{
    if(message)
    {
        fprintf(stderr, "%s\n", message);
    }

    fprintf(stderr, "Usage: %s [-h] -c <children>\n", program_name);
    fputs("Options:\n", stderr);
    fputs("  -h  Display this help message\n", stderr);
    fputs("  -c <children> the number of children to fork\n", stderr);
    exit(exit_code);
}

/*
    Converts the children argument string to an integer

    @param
    binary_name: Name of the executable (used for error reporting)
    children_str: String representing number of child processes
    children: Output pointer to store parsed integer value
 */
static void handle_arguments(const char *binary_name, const char *children_str, int *children)
{
    *children = parse_positive_int(binary_name, children_str);
}

/*
    Parses a string as a positive integer with error handling

    @param
    binary_name: Name of the executable (used for error reporting)
    str: String to parse

    @return
    Parsed positive integer value, exits on error
 */
static int parse_positive_int(const char *binary_name, const char *str)
{
    char    *endptr;
    intmax_t parsed_value;

    errno        = 0;
    parsed_value = strtoimax(str, &endptr, BASE_TEN);

    if(errno != 0)
    {
        usage(binary_name, EXIT_FAILURE, "Error parsing integer.");
    }

    // Check if there are any non-numeric characters in the input string
    if(*endptr != '\0')
    {
        usage(binary_name, EXIT_FAILURE, "Invalid characters in input.");
    }

    // Check if the parsed value is non-negative
    if(parsed_value < 0 || parsed_value > INT_MAX)
    {
        usage(binary_name, EXIT_FAILURE, "Integer out of range or negative.");
    }

    return (int)parsed_value;
}
