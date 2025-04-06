#ifndef HTTP_H
#define HTTP_H

#include <sys/stat.h>

#define REQ_HEADER_LEN 8
#define TEN 10
#define LEN_405 9

void my_function(const char *str);
void set_request_path(char *req_path, const char *buffer);
int  handle_client(int newsockfd, const char *request_path, int is_head, int is_img);
int  handle_post_request(const char *buffer, int client_fd);
int  is_img_request(const char *buffer);
int  is_http_request(const char *buffer);

#if(defined(__APPLE__) && defined(__MACH__))
int handle_post_request_mac(const char *buffer, int client_fd);
#endif

#endif
