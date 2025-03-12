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

static void setup_signal_handler(void);
static void sigint_handler(int signum);

// this variable should not be moved to a .h file
static volatile sig_atomic_t exit_flag = 0;    // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

int main(int arg, const char *argv[])
{
    void *handle;
    void (*my_func)(const char *);                                     // Declare a function pointer for the function from the shared lib
    fd_set             readfds;                                        // Set of file descriptors for select
    size_t             max_clients;                                    // Maximum number of clients that can connect
    struct sockaddr_in host_addr;                                      // Server's address structure
    unsigned int       host_addrlen;                                   // Length of the server address
    int               *client_sockets = (int *)malloc(sizeof(int));    // Array of active client sockets
    int                sd             = 0;                             // Temp variable for socket descriptor

    // Create client address
    struct sockaddr_in client_addr;
    unsigned int       client_addrlen = sizeof(client_addr);
    // Create a TCP socket
    int           server_fd = socket(AF_INET, SOCK_STREAM, 0);    // NOLINT(android-cloexec-socket)
    struct kevent event;
    int           fd = open("http.so", O_RDONLY | O_CLOEXEC);
    int           kq = kqueue();
    if(server_fd == -1)
    {
        perror("webserver (socket)");
        free(client_sockets);
        close(fd);
        return 1;
    }
    if(kq == -1)
    {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }
    if(fd == -1)
    {
        perror("open");
        exit(EXIT_FAILURE);
    }
    EV_SET(&event, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_WRITE | NOTE_DELETE, 0, NULL);

    if(kevent(kq, &event, 1, NULL, 0, NULL) == -1)
    {
        perror("kevent");
        exit(EXIT_FAILURE);
    }

    handle = dlopen("../http.so", RTLD_NOW);
    if(!handle)
    {
        free((void *)client_sockets);
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        close(fd);
        close(server_fd);
        return 1;
    }

    // (Debugging) Print program arguments
    printf("%d\n", arg);
    printf("%s\n", argv[0]);

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
        close(fd);
        free(client_sockets);
        dlclose(handle);
        return 1;
    }
    printf("Socket successfully bound to address\n");

    // Listen for incoming connections
    if(listen(server_fd, SOMAXCONN) != 0)
    {
        perror("webserver (listen)");
        close(fd);
        close(server_fd);
        free(client_sockets);
        dlclose(handle);
        return 1;
    }
    printf("Server listening for connections\n\n");

    while(!exit_flag)
    {
        int             max_fd;                          // Maximum file descriptor for select
        int             activity;                        // Number of ready file descriptors
        int             sockn;                           // Temporary socket descriptor
        ssize_t         valread;                         // For read operations
        char            buffer[BUFFER_SIZE] = {0};       // Buffer for storing incoming data
        struct timespec timeout             = {0, 0};    // Non-blocking
        int             nev                 = kevent(kq, NULL, 0, &event, 1, &timeout);
        if(nev == -1)
        {
            perror("kevent");
            close(fd);
            close(server_fd);
            free(client_sockets);
            return 1;
        }

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
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif

        // Start with the server socket
        max_fd = server_fd;

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

        // Wait for activity on one of the monitored sockets
        activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if(activity < 0)
        {
            perror("Select error");
            continue;
        }

        if(FD_ISSET(server_fd, &readfds))
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
        {
            int *temp;
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

            // Get client address
            sockn = getsockname(newsockfd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addrlen);
            if(sockn < 0)
            {
                perror("webserver (getsockname)");
                continue;
            }
        }

        for(size_t i = 0; i < max_clients; i++)
        {
            sd = client_sockets[i];

#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
            if(FD_ISSET(sd, &readfds))
            {
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
                // Read from the socket: this is the request
                valread = read(sd, buffer, BUFFER_SIZE);
                if(valread < 0)
                {
                    perror("webserver (read)");
                    close(sd);
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
                    FD_CLR(sd, &readfds);    // Remove the closed socket from the set
#if defined(__FreeBSD__) && defined(__GNUC__)
    #pragma GCC diagnostic pop
#endif
                    client_sockets[i] = 0;
                }
                else
                {
                    const char *http_response = "HTTP/1.1 200 OK\r\n"
                                                "Content-Type: text/plain\r\n"
                                                "Content-Length: 13\r\n"
                                                "\r\n"
                                                "Hello, world!";
                    printf("[%s:%u]\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    if(nev > 0)
                    {
                        if(event.fflags & NOTE_WRITE)
                        {
                            printf("so file modified, reloading lib\n");
                            dlclose(handle);    // close current shared library handle

                            // dynamic loading of shared library
                            handle = dlopen("../http.so", RTLD_NOW);
                            if(!handle)
                            {
                                free((void *)client_sockets);
                                fprintf(stderr, "dlopen failed: %s\n", dlerror());
                                close(server_fd);
                                return 1;
                            }
                        }
                        if(event.fflags & NOTE_DELETE)
                        {
                            printf("so file deleted\n");
                            close(fd);
                            my_func = NULL;
                            fd      = open("http.so", O_RDONLY | O_CLOEXEC);
                            if(fd < 0)
                            {
                                perror("new http.so not found");
                                break;
                            }
                            EV_SET(&event, (uintptr_t)fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, NOTE_WRITE | NOTE_DELETE, 0, NULL);

                            if(kevent(kq, &event, 1, NULL, 0, NULL) == -1)
                            {
                                perror("kevent");
                                exit(EXIT_FAILURE);
                            }
                        }
                    }

                    // retrieving symbol (the function name in the lib is my_function)
                    *(void **)(&my_func) = dlsym(handle, "my_function");
                    if(!my_func)
                    {
                        fprintf(stderr, "dlsym failed: %s\n", dlerror());
                        close(server_fd);
                        dlclose(handle);
                        free(client_sockets);
                        return EXIT_FAILURE;
                    }

                    // test function from shared library
                    my_func(buffer);
                    printf("\n");
                    send(sd, http_response, strlen(http_response), 0);

                    client_sockets[i] = 0;    // Remove from client list
                }
            }
        }
    }
    dlclose(handle);    // close shared library handle
    free((void *)client_sockets);
    close(server_fd);
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
