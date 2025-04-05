#include "http.h"
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFFER_SIZE 1024
#define HTTP_OK "HTTP/1.0 200 OK\r\n"
#define HTTP_NOT_FOUND "HTTP/1.0 404 Not Found\r\n"
#define HTTP_BAD_REQUEST "HTTP/1.0 400 Bad Request\r\n"
#define HTTP_METHOD_NOT_ALLOWED "HTTP/1.0 405 Method Not Allowed\r\nAllow: GET, HEAD\r\n"

#define HTML_CONTENT_TYPE "Content-Type: text/html\r\n"
#define TEXT_CONTENT_TYPE "Content-Type: text/plain\r\n"
#define CSS_CONTENT_TYPE "Content-Type: text/css\r\n"
#define JS_CONTENT_TYPE "Content-Type: text/javascript\r\n"
#define JPEG_CONTENT_TYPE "Content-Type: image/jpeg\r\n"
#define PNG_CONTENT_TYPE "Content-Type: image/png\r\n"
#define GIF_CONTENT_TYPE "Content-Type: image/gif\r\n"

// #define PATH_LEN 1024
#define CONTENT_LEN_BUF 100
#define CONTENT_TERM_LEN 5
#define FILE_EXT_LEN 5
#define SIZE_404_MSG 20

#define INDEX_FILE_PATH "/index.html"

// don't need html because it's the default
#define JS_EXT "sj"
#define CSS_EXT "ssc"
#define JPG_EXT "gpj"
#define JPEG_EXT "gepj"
#define PNG_EXT "gnp"
#define GIF_EXT "fig"
#define TXT_EXT "txt"
#if (defined(__APPLE__) && defined(__MACH__))
    #define FILE_PATH_LEN 11
#endif

#if defined(__linux__)
    #define FILE_PATH_LEN 12
#endif

static void set_request_method(char *req_header, const char *buffer);
static int  has_valid_first_line(const char *buffer);
static int  has_valid_headers(const char *buffer);

/*
    Test function to verify dynamic updates to shared library behavior.

    This function is used during runtime to test whether the server reflects changes
    made to the shared library (e.g., switching between `toupper` and `tolower`).
    The shared object should be updated while the server is running.

    @param
    str: The input string to process to either upper or lowercase
 */
void my_function(const char *str)
{
    for(size_t i = 0; i < strlen(str); i++)
    {
        printf("%c", toupper(str[i]));
    }
}

/*
    Extracts the request path from the HTTP request header

    @param
    req_path: The path of the requested file
    buffer: The full HTTP request header
 */
void set_request_path(char *req_path, const char *buffer)
{
    char c;
    int  i = 0;
    int  j = 0;

    // Start reading the buffer
    c = buffer[i];

    // Skip characters until the first space (end of HTTP method)
    // This is because header was already confirmed at this point
    while(c != ' ')
    {
        c = buffer[++i];
    }

    // Move past the space to start of request path
    c = buffer[++i];

    // Copy chars from buffer to req_path until next space
    while(c != ' ' && j < BUFFER_SIZE)
    {
        req_path[j++] = c;
        c             = buffer[++i];
    }

    // Null-terminate req_path
    req_path[j] = '\0';

    // Debug: print the extracted request
    printf("request path: %s\n", req_path);
}

/*
    Opens a file at the specified path and retrieves its file descriptor and metadata

    @param
    request_path: The path of the file to open
    file_fd: Stores the file descriptor of the file
    file_state: Stores the file's metadata
 */
static void open_file_at_path(const char *request_path, int *file_fd, struct stat *file_stat)
{
    const char *base_path = "./resources";
    size_t      base_len  = strlen(base_path);
    size_t      total_len = base_len + strlen(request_path) + 1;

    char *path = (char *)malloc(total_len);
    if(path == NULL)
    {
        perror("malloc failed for path");
        *file_fd = -1;
        return;
    }

    // Build full path
    strcpy(path, base_path);
    strcat(path, request_path);

    printf("file path: %s\n", path);

    *file_fd = open(path, O_RDONLY | O_CLOEXEC);
    stat(path, file_stat);

#if (defined(__APPLE__) && defined(__MACH__))
    printf("File size of %s: %lld bytes\n", path, file_stat->st_size);
#endif

#if defined(__linux__)
    printf("File size of %s: %ld bytes\n", path, file_stat->st_size);
#endif

    printf("File descriptor: %d\n", *file_fd);

    // Free the allocated memory
    free(path);
}

/*
    Reads the content of a file at the specified path and writes it to a string

    @param
    content_string: Where the text content of a file will be stored
    length: Length of the content
    file_path: The path to the file being read

    @return
    0: File was read successfully and stored in content_string
    -1: An error occurred while reading the file
    -2: The requested file was not found, 404 page loaded instead
    -3: Memory allocation failed while creating content_string
 */
static int write_to_content_string(char **content_string, unsigned long *length, const char *file_path)
{
    char         c;                                     // Temp character for read functions
    struct stat  file_stat;                             // Holds file metadata
    struct stat *fileStat = &file_stat;                 // Pointer to file metadata
    int          file_fd;                               // File descriptor for the file
    char        *path;                                  // String that will store the file path
    const char  *MSG_404 = "<p>404 NOT FOUND</p>\0";    // 404 error message
    int          retval  = 0;                           // Return value

    // Check if the requested file path is "/"
    if(strcmp(file_path, "/") == 0)
    {
        // Allocate memory for the index path
        path = (char *)malloc(sizeof(char) * (FILE_PATH_LEN + 1));
        if(path == NULL)
        {
            perror("malloc");
            return -3;
        }

        // Copy the index path
        for(size_t i = 0; i < strlen(INDEX_FILE_PATH); i++)
        {
            path[i] = INDEX_FILE_PATH[i];
        }
        path[strlen(INDEX_FILE_PATH)] = '\0';
    }
    else
    {
        // Allocate memory for the requested file path
        path = (char *)malloc(sizeof(char) * (strlen(file_path) + 1));
        if(path == NULL)
        {
            perror("malloc");
            return -3;
        }

        // Copy the file path
        for(size_t i = 0; i < strlen(file_path); i++)
        {
            path[i] = file_path[i];
        }
        path[strlen(file_path)] = '\0';
    }

    // Open the file at the specified path
    open_file_at_path(path, &file_fd, fileStat);

    // Free the allocated path memory
    free(path);

    // If file could not be opened, served the 404 error page
    if(file_fd == -1)
    {
        // If the 404 file is missing, return an error message
        perror("webserver (open: 404 html msg file has been moved or deleted)");
        *content_string = (char *)malloc((sizeof(char) * SIZE_404_MSG) + 1);
        if(*content_string == NULL)
        {
            perror("webserver (malloc)");
            close(file_fd);
            return -3;
        }
        for(int i = 0; i <= SIZE_404_MSG; i++)
        {
            (*content_string)[i] = MSG_404[i];
        }
        close(file_fd);
        return -2;
    }

#if (defined(__APPLE__) && defined(__MACH__))
    printf("filestat st_size: %lld\n", fileStat->st_size);
#endif

#if defined(__linux__)
    printf("filestat st_size: %ld\n", fileStat->st_size);
#endif

    // Allocate memory for the content string
    *content_string = (char *)malloc(sizeof(char) * ((size_t)fileStat->st_size + 1));
    if(*content_string == NULL)
    {
        perror("webserver (malloc)");
        close(file_fd);
        return -3;
    }

    // Read the contents of the file into content_string
    for(int i = 0; i < fileStat->st_size; i++)
    {
        ssize_t valread = read(file_fd, &c, sizeof(char));
        if(valread < 0)
        {
            perror("webserver (read content string)");
            close(file_fd);
            free(*content_string);
            return -1;
        }
        (*content_string)[i] = c;
        (*length)++;
    }
    (*content_string)[(*length)] = '\0';
    printf("content_string: %s\n", *content_string);

    // Close the file descriptor
    close(file_fd);

    // we don't want to free the content_string here because we need it to stay allocated
    // in order to put it in the response_string in handle_client

    return retval;
}

/*
    Sets the content-type header based on the file extension in the requested path

    @param
    request_path: The path of the requested file
    content_type_string: A string where the appropriate content-type will be stored
 */
// cppcheck-suppress constParameterPointer
static void set_content_type_from_file_extension(const char *request_path, char *content_type_string)
{
    size_t req_path_i                   = strlen(request_path) - 1;
    int    file_ext_i                   = 0;
    char   file_extension[FILE_EXT_LEN] = {0};

    // grab the last chars up to '.' of the request_path
    printf("request path index: %zu\n", req_path_i);
    while(request_path[req_path_i] != '.' && req_path_i > 0 && file_ext_i < FILE_EXT_LEN)
    {
        file_extension[file_ext_i++] = request_path[req_path_i--];
    }
    printf("file_extension: %s\n", file_extension);

    if(strcmp(file_extension, TXT_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, TEXT_CONTENT_TYPE, strlen(TEXT_CONTENT_TYPE));
        content_type_string[strlen(TEXT_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, TEXT_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }

    else if(strcmp(file_extension, JS_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, JS_CONTENT_TYPE, strlen(JS_CONTENT_TYPE));
        content_type_string[strlen(JS_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, JS_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }
    else if(strcmp(file_extension, CSS_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, CSS_CONTENT_TYPE, strlen(CSS_CONTENT_TYPE));
        content_type_string[strlen(CSS_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, CSS_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }
    else if(strcmp(file_extension, JPG_EXT) == 0 || strcmp(file_extension, JPEG_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, JPEG_CONTENT_TYPE, strlen(JPEG_CONTENT_TYPE));
        content_type_string[strlen(TEXT_JPEG_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, JPEG_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }
    else if(strcmp(file_extension, PNG_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, PNG_CONTENT_TYPE, strlen(PNG_CONTENT_TYPE));
        content_type_string[strlen(PNG_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, PNG_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }
    else if(strcmp(file_extension, GIF_EXT) == 0)
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, GIF_CONTENT_TYPE, strlen(GIF_CONTENT_TYPE));
        content_type_string[strlen(GIF_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, GIF_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }
    else
    {
#if (defined(__APPLE__) && defined(__MACH__))
        strncpy(content_type_string, HTML_CONTENT_TYPE, strlen(HTML_CONTENT_TYPE));
        content_type_string[strlen(HTML_CONTENT_TYPE)] = '\0';
#endif

#if defined(__linux__)
        strncpy(content_type_string, HTML_CONTENT_TYPE, BUFFER_SIZE - 1);
        content_type_string[BUFFER_SIZE - 1] = '\0';
#endif
    }

    printf("set content type header to: %s\n", content_type_string);
}

/*
    Appends a message to the response string

    @param
    response: Where the message will be appended
    msg: The message to be copied
 */
static void append_msg_to_response_string(char *response, const char *msg)
{
    strncpy(response, msg, strlen(msg));
    response[strlen(msg)] = '\0';
}

/*
    Converts an integer into a string

    @param
    string: Where the converted value will be stored
    n: The number to be converted
 */
static void int_to_string(char *string, unsigned long n)
{
    char          buffer[TEN] = {0};
    int           digits      = 0;
    unsigned long i           = n;

    // If n is zero
    if(n == 0)
    {
        string[0] = '0';
        string[1] = '\0';
        return;
    }

    // Extract digits and store them in reverse order
    while(i > 0)
    {
        buffer[digits++] = (char)((i % TEN) + '0');
        i                = i / TEN;
        //        printf("%lu\n", i);
    }
    //    printf("digits: %d\n", digits);

    // Reverse the order of the digits in the buffer
    for(int j = 0; j < digits; j++)
    {
        string[j] = buffer[digits - j - 1];
    }
    string[digits] = '\0';
}

/*
    Appends a Content-Length header to the HTTP response string
    This one is special because it has the extra \r\n and needs to be constructed with the appropriate length

    @param
    response_string: Where the content-length will be appended
    length: Length of the HTTP body
 */
static void append_content_length_msg(char *response_string, unsigned long length)
{
    char content_len_buffer[CONTENT_LEN_BUF];
    char content_length_msg[BUFFER_SIZE] = "Content-Length: ";

    // Convert the length value to a string and store it in content_len_buffer
    int_to_string(content_len_buffer, length);
    //    printf("content length: %s\n", content_len_buffer);

    // Append the length
    strncat(content_length_msg, content_len_buffer, strlen(content_len_buffer));

    // Append a trailing CRLF sequence
    strncat(content_length_msg, "\r\n\r\n", CONTENT_TERM_LEN);

    //    printf("content_length_msg: %s\n", content_length_msg);
    //    printf("length: %lu\n", length);

    // Append the content-length header
    strncat(response_string, content_length_msg, strlen(content_length_msg) + 1);

    //    printf("response string: %s\n", response_string);
}

/*
    Appends the body and a trailing CRLF sequence to the HTTP response string

    @param
    response_string: Contains the HTTP response
    content_string: The body content to be appended
    length: The length of the string to be appended
*/
static void append_body(char *response_string, const char *content_string, unsigned long length)
{
    if(content_string != NULL)
    {
        strncat(response_string, content_string, length);

#if (defined(__APPLE__) && defined(__MACH__))
        strncat(response_string, "\r\n", 2);
#endif

#if defined(__linux__)
        strcat(response_string, "\r\n");
#endif
    }
}

/*
    Sends the HTTP response to the client

    @param
    newsockfd: The client's socket file descriptor
    response_string: The HTTP response string to be sent

    @return
    0: Response successfully sent
    -1: An error occurred while writing to the socket
 */
static int write_to_client(int newsockfd, const char *response_string)
{
    ssize_t valwrite;
    valwrite = write(newsockfd, response_string, strlen(response_string));
    printf("valwrite: %zd\n", valwrite);
    fflush(stdout);
    if(valwrite < 0)
    {
        perror("webserver (write)");
        return -1;
    }
    printf("successfully wrote to client\n");
    return 0;
}

/*
    Reads the content of a file at the specified path and writes it as binary data to the specified file descriptor.

    @param
    fd: The file descriptor to which the binary content will be written
    file_path: The path to the file being read

    @return
    0: File was read successfully and written to the file descriptor
    -1: An error occurred while reading or writing the file
    -2: The requested file was not found (404 error)
    -3: Memory allocation failed during processing
 */
static int write_to_content_binary(int fd, const char *file_path)
{
    struct stat  file_stat;                // Holds file metadata
    struct stat *fileStat = &file_stat;    // Pointer to file metadata
    int          file_fd;                  // File descriptor for the file
    char        *path;                     // String to store the file path
    int          retval = 0;               // Return value
    ssize_t      bytes_read;
    ssize_t      bytes_written;
    char        *buffer;

    // Check if the requested file path is "/"
    if(strcmp(file_path, "/") == 0)
    {
        // Allocate memory for the index path
        path = (char *)malloc(sizeof(char) * (FILE_PATH_LEN + 1));
        if(path == NULL)
        {
            perror("malloc");
            return -3;
        }

        // Copy the index path
        strncpy(path, INDEX_FILE_PATH, FILE_PATH_LEN);
        path[FILE_PATH_LEN] = '\0';
    }
    else
    {
        // Allocate memory for the requested file path
        path = (char *)malloc(sizeof(char) * (strlen(file_path) + 1));
        if(path == NULL)
        {
            perror("malloc");
            return -3;
        }

        // Copy the file path
        strncpy(path, file_path, strlen(file_path));
        path[strlen(file_path)] = '\0';
    }

    // Open the file at the specified path
    open_file_at_path(path, &file_fd, fileStat);

    // Free the allocated path memory
    free(path);

    // If file could not be opened, serve the 404 error page (returning -2 signals this)
    if(file_fd == -1)
    {
        printf("file fd could not be opened for binary file\n");
        return -2;
    }

#if (defined(__APPLE__) && defined(__MACH__))
    printf("File size: %lld bytes\n", fileStat->st_size);
#endif

#if defined(__linux__)
    printf("File size: %ld bytes\n", fileStat->st_size);
#endif

    // Allocate memory for the binary content
    buffer = (char *)malloc((size_t)fileStat->st_size);

    if(buffer == NULL)
    {
        perror("webserver (malloc)");
        close(file_fd);
        return -3;
    }

    // Read the entire file into buffer
    bytes_read = read(file_fd, buffer, (size_t)fileStat->st_size);
    if(bytes_read < 0)
    {
        perror("webserver (read binary file)");
        free(buffer);
        close(file_fd);
        return -1;
    }

    bytes_written = write(fd, buffer, (size_t)fileStat->st_size);
    if(bytes_written < 0)
    {
        perror("Error writing to destination socket");
        free(buffer);
        close(fd);
        close(file_fd);
        return -1;
    }

    // Close the file descriptors and free dynamic memory
    close(file_fd);
    free(buffer);
    printf("Succesfully wrote binary file to client\n");
    return retval;    // Success
}

/*
    Processes an incoming HTTP request from a client, constructing an HTTP response
    and sending it back to the client

    @param
    newsockfd: socket fd for the client
    request_path: file path requested by the client
    is_head: flag indicating whether the HTTP request is a HEAD request
    is_img: flag indicating that the HTTP request is for an image

    @return
    0: The HTTP response was successfully sent to the client
    -1: An error occurred while generating the HTTP response body
    -2: The requested file was not found
    -3: Memory allocatio for the response failed
 */
int handle_client(int newsockfd, const char *request_path, int is_head, int is_img)
{
    char  *response_string;                     // The Full HTTP response
    char  *content_string = {0};                // HTTP response body
    char **content_ptr    = &content_string;    // Pointer to dynamically allocated resources
    // TODO: malloc content_type_line
    char          content_type_line[BUFFER_SIZE] = {0};    // Content-type header
    int           valread;                                 // Result of file read operation
    unsigned long length          = 0;                     // Length of response body
    unsigned long response_length = 0;                     // Total length of HTTP response
    int           result;

    // we malloc the content_string in this function
    // length also gets set to the length of the body in this function
    valread = write_to_content_string(content_ptr, &length, request_path);

    if(valread == -1)
    {
        perror("webserver (http response body)");
        return -1;
    }

    // set content type
    if(valread == -2)
    {
        set_content_type_from_file_extension(".html", content_type_line);
    }
    else
    {
        set_content_type_from_file_extension(request_path, content_type_line);
    }
    printf("content_type_line: %s\n", content_type_line);

    if(strcmp(request_path, "/405.txt") == 0)
    {
        // The method is unsupported
        response_length = strlen(HTTP_METHOD_NOT_ALLOWED) + strlen(content_type_line) + CONTENT_LEN_BUF + length;
        response_string = (char *)malloc(sizeof(char) * (response_length + 1));
        if(response_string == NULL)
        {
            perror("webserver (malloc)");
            free(content_string);
            return -3;
        }
        append_msg_to_response_string(response_string, HTTP_METHOD_NOT_ALLOWED);
        strncat(response_string, content_type_line, strlen(content_type_line) + 1);
        append_content_length_msg(response_string, length);
        append_body(response_string, *content_ptr, length);

        printf("writing 405 content to response: %s\n", response_string);
        write_to_client(newsockfd, response_string);    // Send 405 response
        free(content_string);
        free(response_string);
        return 0;
    }
    // length of response_string = (HTTP HEADER LEN) + content length string length + body length
    if(valread == -2)
    {
        // Serve 404 response
        response_length = strlen(HTTP_NOT_FOUND) + strlen(content_type_line) + CONTENT_LEN_BUF + length;
        response_string = (char *)malloc(sizeof(char) * (response_length + 1));
        if(response_string == NULL)
        {
            perror("webserver (malloc)");
            if(is_img == -1)
            {
                free(content_string);
            }
            return -3;
        }
        append_msg_to_response_string(response_string, HTTP_NOT_FOUND);
        strncat(response_string, content_type_line, strlen(content_type_line) + 1);
        append_content_length_msg(response_string, length);
        append_body(response_string, *content_ptr, length);
        write_to_client(newsockfd, response_string);    // Send 404 response
        free(content_string);
        free(response_string);
        return -2;
    }

    if(strcmp(request_path, "/400.txt") == 0)
    {
        // The request is bad
        response_length = strlen(HTTP_BAD_REQUEST) + strlen(content_type_line) + CONTENT_LEN_BUF + length;
        response_string = (char *)malloc(sizeof(char) * (response_length + 1));
        if(response_string == NULL)
        {
            perror("webserver (malloc)");
            free(content_string);
            return -3;
        }
        append_msg_to_response_string(response_string, HTTP_BAD_REQUEST);
        strncat(response_string, content_type_line, strlen(content_type_line) + 1);
        append_content_length_msg(response_string, length);
        write_to_client(newsockfd, response_string);    // Send 400 response
        free(content_string);
        free(response_string);
        return -1;
    }
    // if it's an image we write directly to the socket
    if(is_img == 0 && is_head == -1)
    {
        int retval   = 0;
        int writeval = 0;
        printf("it's an image!!!\n");
        response_length = strlen(HTTP_OK) + strlen(content_type_line) + CONTENT_LEN_BUF + length;
        response_string = (char *)malloc(sizeof(char) * (response_length + 1));
        if(response_string == NULL)
        {
            perror("webserver (malloc)");
            free(content_string);
            return -3;
        }

        append_msg_to_response_string(response_string, HTTP_OK);
        strncat(response_string, content_type_line, strlen(content_type_line) + 1);
        append_content_length_msg(response_string, length);
        printf("newsockfd: %d\n", newsockfd);
        writeval = write_to_client(newsockfd, response_string);
        printf("writeval: %d\n", writeval);
        if(writeval < 0)
        {
            perror("Error writing to client");
            retval = -1;
            goto cleanup;
        }
        printf("writing to content binary\n");
        if(write_to_content_binary(newsockfd, request_path) < 0)
        {
            perror("Error writing content to client");
            retval = -1;
            goto cleanup;
        }

    cleanup:
        if(response_string)
        {
            free(response_string);
        }
        if(content_string)
        {
            free(content_string);
        }
        return retval;
    }
    // Request was successful
    printf("request was successful");
    response_length = strlen(HTTP_OK) + strlen(content_type_line) + CONTENT_LEN_BUF + length;
    response_string = (char *)malloc(sizeof(char) * (response_length + 1));
    if(response_string == NULL)
    {
        perror("webserver (malloc)");
        free(content_string);
        return -3;
    }
    append_msg_to_response_string(response_string, HTTP_OK);

    // Append the content-type header
    strncat(response_string, content_type_line, strlen(content_type_line) + 1);

    // append content length section (can only do this once we have the body)
    // but must be appended before the body
    append_content_length_msg(response_string, length);

    // append body section, only if not a HEAD request
    if(is_head == -1)
    {
        append_body(response_string, *content_ptr, length);
    }

    // free allocated memory for the body
    free(content_string);

    printf("\n\n\n newsockfd: %d response_string: %s\n", newsockfd, response_string);
    result = write_to_client(newsockfd, response_string);
    free(response_string);

    // write to client
    return result;
}

/*
    Checks if the HTTP request is for an image

    @param
    buffer: The full HTTP request header

    @return
    0: The request target is an image
    -1: The request target is not an image
 */
int is_img_request(const char *buffer)
{
    int  i = 0;
    char c = buffer[i];

    // Traverse until the first '.'
    while(c != '.' && c != '\0' && i < BUFFER_SIZE)
    {
        c = buffer[++i];
    }

    // If no '.' was found, it's not an image request
    if(c != '.')
    {
        return -1;
    }

    // Check the next few characters to identify the file extension
    if(strncmp(&buffer[i], ".jpg", FILE_EXT_LEN - 1) == 0 || strncmp(&buffer[i], ".jpeg", FILE_EXT_LEN) == 0 || strncmp(&buffer[i], ".png", FILE_EXT_LEN - 1) == 0 || strncmp(&buffer[i], ".gif", FILE_EXT_LEN - 1) == 0)
    {
        return 0;
    }

    return -1;
}

/*
    Checks if the header contains a valid HTTP request method, the first line is valid, and the headers are valid

    @param
    req_header: The HTTP request method
    buffer: The full HTTP request header

    @return
    0: The buffer contains a valid HTTP request
    -1: The buffer does not contain a valid HTTP request
 */
int is_http_request(const char *buffer)
{
    int  valid_firstline = 0;
    int  valid_headers   = 0;
    char req_header[REQ_HEADER_LEN + 1];

    set_request_method(req_header, buffer);

    printf("_______---________\n\n\nreq_header inside is_http_request: %s\n\n\n", req_header);

    // Check if the method in req_header is a valid HTTP method
    if(strcmp(req_header, "GET") != 0 && strcmp(req_header, "HEAD") != 0 && strcmp(req_header, "POST") != 0 && strcmp(req_header, "PUT") != 0 && strcmp(req_header, "DELETE") != 0 && strcmp(req_header, "CONNECT") != 0 && strcmp(req_header, "OPTIONS") != 0 &&
       strcmp(req_header, "TRACE") != 0 && strcmp(req_header, "PATCH") != 0)
    {
        printf("\n\n\n-------RETURNING -1------\n\n\n");
        return -1;
    }

    // Check if the first line is valid
    valid_firstline = has_valid_first_line(buffer);

    // Check if the headers are valid
    valid_headers = has_valid_headers(buffer);
    if(valid_firstline == -1 || valid_headers == -1)
    {
        return -1;
    }
    return 0;
}

/*
    Checks if the request has a valid first line like METHOD URI HTTP/x{x}\r\n
    This function assumes the HTTP method has already been validated

    @param
    buffer: The buffer containing the HTTP request

    @return
    0: The request has a valid first line
    -1: The request is invalid
 */
static int has_valid_first_line(const char *buffer)
{
    int  i = 0;
    char c = buffer[i];

    // Traverse until the first space
    while(c != ' ' && i < BUFFER_SIZE)
    {
        c = buffer[++i];
    }

    // Check that there is no URI before HTTP/
    if(i < BUFFER_SIZE - 4)
    {
        if(buffer[i + 1] == 'H' && buffer[i + 2] == 'T' && buffer[i + 3] == 'T' && buffer[i + 4] == 'P' && buffer[i + FILE_EXT_LEN] == '/')
        {
            return -1;
        }
    }

    // Traverse until the next space, end of the uRI
    while(c != ' ' && i < BUFFER_SIZE)
    {
        c = buffer[++i];
    }

    // Check that the URI does not end with a '/'
    if(i < BUFFER_SIZE - 1 && buffer[i + 1] != '/')
    {
        return -1;
    }

    // Move past the URI
    c = buffer[++i];

    // Traverse until the next space
    while(c != ' ' && i < BUFFER_SIZE)
    {
        c = buffer[++i];
    }
    i++;

    // will return -1 if there is no HTTP/x{x}
    if(buffer[i] != 'H' || buffer[i + 1] != 'T' || buffer[i + 2] != 'T' || buffer[i + 3] != 'P' || buffer[i + 4] != '/')
    {
        return -1;
    }

    // Traverse until '\r'
    while(c != '\r')
    {
        c = buffer[++i];
    }

    // Check if the line ends with \r\n
    if(buffer[i + 1] != '\n')
    {
        return -1;
    }
    return 0;
}

/*
    Checks to make sure headers have colons and end in \r\n\r\n, and each ends with \r\n
    This assumes the request line has already been validate

    @param
    buffer: Holds the entire HTTP request

    @return
    0: The headers are valid
    -1: The headers are invalid
 */
static int has_valid_headers(const char *buffer)
{
    int  i              = 0;
    char c              = buffer[i];
    int  final_rn_found = -1;

    // go to the first \r\n (which is right before the headers)
    while(i < BUFFER_SIZE - 1 && c != '\r')
    {
        c = buffer[++i];
    }
    c = buffer[++i];    // buffer is now \n

    // Process the headers
    while(i < BUFFER_SIZE && final_rn_found == -1)
    {
        // find a colon
        while(i < BUFFER_SIZE - 1 && c != ':')
        {
            c = buffer[++i];
        }
        if(i > BUFFER_SIZE - 1)
        {
            return -1;
        }
        // make sure there is at least 1 char after the colon
        c = buffer[i];

        // find the \r\n
        while(i < BUFFER_SIZE - 3 && c != '\r')
        {
            c = buffer[++i];
        }
        if(i > BUFFER_SIZE - 2)
        {
            return -1;
        }
        if(buffer[++i] != '\n')
        {
            return -1;
        }
        // we have already incremeted i one past the \r\n

        // if we don't hit another \r we havent hit the end of the headers
        if(buffer[++i] != '\r')
        {
            continue;
        }

        // check if the next characters are \r\n, marking the end of the header
        if(buffer[i] == '\r' && buffer[i + 1] == '\n')
        {
            final_rn_found = 0;
        }
        break;
    }
    return final_rn_found;
}

/*
    Extracts the HTTP method from the request header

    @param
    req_header: Where the HTTP request method will be stored
    buffer: The full HTTP request header
 */
static void set_request_method(char *req_header, const char *buffer)
{
    char c;
    int  i = 0;
    int  j = 0;

    // Read the buffer
    c = buffer[i];

    // Copy chars from buffer to req_header until first space
    while(c != ' ' && j < REQ_HEADER_LEN)
    {
        req_header[j++] = c;
        c               = buffer[++i];
    }

    // Null-terminate req_header
    req_header[j] = '\0';

    // Debug: print the extracted request
    printf("request path: %s\n", req_header);
    printf("request path length: %d\n", (int)strlen(req_header));
}
