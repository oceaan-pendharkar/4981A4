#include "../include/http.h"
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define CHILDREN 3

static void setup_signal_handler(void);
static void sigint_handler(int signum);
static int  handle_client(struct sockaddr_in client_addr, int client_fd, void *handle);
int         recv_fd(int socket);
int         send_fd(int socket, int fd);

// this variable should not be moved to a .h file
static volatile sig_atomic_t exit_flag = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

int main(int arg, const char *argv[])
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
    int                worker_sockets[CHILDREN][2];                    // Stores UNIX socket pairs for each worker
    pid_t              child_pids[CHILDREN] = {0};
    pid_t              monitor;
    int                server_fd;

    // Create client address
    struct sockaddr_in client_addr;
    unsigned int       client_addrlen = sizeof(client_addr);

    // initialize shared library
    handle = dlopen("../http.so", RTLD_NOW);
    if(!handle)
    {
        free((void *)client_sockets);
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    // todo: grab date of last edit to http.so

    // create domain socket for server -> monitor
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, dsfd) == -1)
    {
        perror("webserver (socketpair)");
        free(client_sockets);
        dlclose(handle);
        return 1;
    }

    // create domain socket for monitor -> worker
    // this is my solution for making sure only one child gets a fd from the monitor
    for(int i = 0; i < CHILDREN; i++)
    {
        if(socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]) == -1)
        {
            perror("webserver (socketpair)");
            free(client_sockets);
            dlclose(handle);
            return 1;
        }
    }

    // fork the monitor
    monitor = fork();
    if(monitor == -1)
    {
        perror("fork");
        exit(EXIT_FAILURE);
    }
    if(monitor == 0)
    {
        while(1)
        {
            int   status;
            pid_t dead_worker;
            int   next_worker = 0;

            int client_fd;
            // close the end of ds we're going to monitor for in select
            close(dsfd[0]);

            // recvmsg: receive client socket from server
            client_fd = recv_fd(dsfd[1]);
            if(client_fd == -1)
            {
                perror("webserver (recv_fd)");
                close(dsfd[1]);
                dlclose(handle);
                free(client_sockets);
                return 1;
            }

            // todo: round robin
            // hard coding to send to first worker for now
            send_fd(worker_sockets[next_worker][0], client_fd);

            printf("received fd in monitor and sent to worker: %d\n", client_fd);

            // pre-fork children
            for(int i = 0; i < CHILDREN; i++)
            {
                pid_t pid = fork();
                if(pid < 0)
                {
                    perror("webserver (fork)");
                    dlclose(handle);
                    free(client_sockets);
                    return 1;
                }
                if(pid == 0)
                {
                    while(1)
                    {
                        int sockn;
                        int handle_result;
                        int fd = recv_fd(worker_sockets[i][1]);    // recv_fd from monitor
                        if(fd == -1)
                        {
                            perror("webserver: worker (recv_fd)");
                            dlclose(handle);
                            free(client_sockets);
                            return 1;
                        }

                        printf("Received client fd in child: %d\n", fd);

                        // check .so object to see if it's new
                        // if new: dlclose, dlopen, dlsym (set handle)

                        // Get client address
                        sockn = getsockname(fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addrlen);
                        if(sockn < 0)
                        {
                            perror("webserver (getsockname)");
                            continue;
                        }
                        // handle_client()
                        printf("handling client...\n");
                        handle_result = handle_client(client_addr, fd, handle);
                        if(handle_result == 1)
                        {
                            // todo: kill this process ?
                            printf("Dlsym failed in a child worker\n");
                        }

                        // sendmsg: send the fd back to the parent
                        send_fd(dsfd[1], client_fd);
                        printf("sent client fd back to server: %d\n", client_fd);
                        close(client_fd);
                        close(fd);
                    }
                }
                else
                {
                    child_pids[i] = pid;
                    close(worker_sockets[i][1]);    // Close worker's end in monitor
                }
            }

            // monitor code

            dead_worker = waitpid(-1, &status, WNOHANG);    // Check for worker deaths
            if(dead_worker > 0)
            {
                printf("Worker %d died, restarting...\n", dead_worker);

                for(int i = 0; i < CHILDREN; i++)
                {
                    if(child_pids[i] == dead_worker)
                    {
                        pid_t new_pid;
                        close(worker_sockets[i][0]);
                        close(worker_sockets[i][1]);

                        socketpair(AF_UNIX, SOCK_STREAM, 0, worker_sockets[i]);
                        new_pid = fork();
                        if(new_pid == 0)
                        {
                            close(worker_sockets[i][0]);    // close montor's end of socket pair
                            // give the new child their client code
                            while(1)
                            {
                                int sockn;
                                int fd = recv_fd(worker_sockets[i][1]);
                                if(fd == -1)
                                {
                                    perror("webserver (recv_fd)");
                                    continue;
                                }
                                if(client_fd < 0)
                                {
                                    continue;
                                }

                                // Get client address
                                sockn = getsockname(fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addrlen);
                                if(sockn < 0)
                                {
                                    perror("webserver (getsockname)");
                                    continue;
                                }

                                handle_client(client_addr, fd, handle);
                                close(client_fd);
                            }
                        }
                        else if(new_pid > 0)
                        {
                            child_pids[i] = new_pid;
                            close(worker_sockets[i][1]);
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
    }

    // Create a TCP socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);    // NOLINT(android-cloexec-socket)
    if(server_fd == -1)
    {
        perror("webserver (socket)");
        free(client_sockets);
        return 1;
    }

    // (Debugging) Print program arguments
    printf("program arg: %d\n", arg);
    printf("program argv[0]: %s\n", argv[0]);

    // Set up Signal Handler
    setup_signal_handler();

    // Initialize client socket, address and number as zero or null
    //    client_sockets = NULL;
    max_clients = 0;
    memset(&client_addr, 0, sizeof(client_addr));

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
        return 1;
    }
    printf("Socket successfully bound to address\n");

    // Listen for incoming connections
    if(listen(server_fd, SOMAXCONN) != 0)
    {
        perror("webserver (listen)");
        close(server_fd);
        free(client_sockets);
        dlclose(handle);
        return 1;
    }
    printf("Server listening for connections\n\n");

    // Clear the socket set
#ifndef __clang_analyzer__
    FD_ZERO(&readfds);
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
    printf("adding client sockets\n");
    // Add the client sockets to the set
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

    printf("entering loop\n\n");
    while(!exit_flag)
    {
        int activity;    // Number of ready file descriptors

        printf("maxfd: %d\n", max_fd);

        // Wait for activity on one of the monitored sockets
        activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if(activity < 0)
        {
            perror("Select error");
            continue;
        }

        printf("select ok\n\n");

        if(FD_ISSET(server_fd, &readfds))
        {
            int *temp;
            int  received_fd_from_client;
            // Accept incoming connections
            int newsockfd = accept(server_fd, (struct sockaddr *)&host_addr, (socklen_t *)&host_addrlen);
            if(newsockfd < 0)
            {
                perror("webserver (accept)");
                continue;
            }
            printf("connection accepted\n");

            // Increase the size of the client_sockets array
            max_clients++;
            temp = (int *)realloc(client_sockets, sizeof(int) * max_clients);

            if(temp == NULL)
            {
                perror("realloc");
                free(client_sockets);
                exit(EXIT_FAILURE);
            }
            else
            {
                client_sockets                  = temp;
                client_sockets[max_clients - 1] = newsockfd;
            }
            printf("Sending client fd %d\n", newsockfd);
            send_fd(dsfd[0], newsockfd);
            received_fd_from_client = recv_fd(dsfd[0]);
            printf("received fd from client: %d\n", received_fd_from_client);

#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
            FD_CLR(sd, &readfds);    // Remove the closed socket from the set
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
            client_sockets[max_clients - 1] = 0;
            close(received_fd_from_client);
        }

        if(FD_ISSET(dsfd[0], &readfds))
        {
            printf("received fd from client on domain socket\n");
            // re-enable client fd for reading in select
        }
    }
    dlclose(handle);    // close shared library handle
    free((void *)client_sockets);
    close(server_fd);

    // close domain socket fds
    close(dsfd[0]);
    close(dsfd[1]);

    for(int i = 0; i < CHILDREN; i++)
    {
        close(worker_sockets[i][0]);
        close(worker_sockets[i][1]);
    }
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

static int handle_client(struct sockaddr_in client_addr, int client_fd, void *handle)
{
    void (*my_func)(const char *);     // function pointer for the function from the shared lib
    char buffer[BUFFER_SIZE] = {0};    // Buffer for storing incoming data

    const char *http_response = "HTTP/1.1 200 OK\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 13\r\n"
                                "\r\n"
                                "Hello, world!";

    ssize_t valread;
    printf("[%s:%u]\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

    valread = read(client_fd, buffer, BUFFER_SIZE);
    if(valread < 0)
    {
        perror("webserver (read)");
        return 1;
    }

    printf("retrieving func from shared library\n\n");
    // retrieving symbol (the function name in the lib is my_function)
    *(void **)(&my_func) = dlsym(handle, "my_function");
    if(!my_func)
    {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        dlclose(handle);
        return 1;
    }

    printf("calling func %p\n", *(void **)(&my_func));
    // test function from shared library
    my_func(buffer);
    printf("\n");
    send(client_fd, http_response, strlen(http_response), 0);

    return 0;
}

// taken from the domain socket notes
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

// copied from domain socket notes
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
