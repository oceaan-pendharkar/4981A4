#ifndef HTTP_H
#define HTTP_H

#include <sys/stat.h>

#define REQ_HEADER_LEN 8
#define TEN 10
#define LEN_405 9

void my_function(const char *str);
void set_request_path(char *req_path, const char *buffer);
int  handle_client(int newsockfd, const char *request_path, int is_head, int is_img);
int  write_to_content_string(char **content_string, unsigned long *length, const char *file_path);
void open_file_at_path(const char *request_path, int *file_fd, struct stat *file_stats);
void set_content_type_from_file_extension(const char *request_path, char *content_type_string);
void append_msg_to_response_string(char *response, const char *msg);
void append_content_length_msg(char *response_string, unsigned long length);
void int_to_string(char *string, unsigned long n);
void append_body(char *response_string, const char *content_string, unsigned long length);
int  write_to_client(int newsockfd, const char *response_string);
int  write_to_content_binary(int fd, const char *file_path);
int  is_img_request(const char *buffer);
int  is_http_request(const char *buffer);
int  has_valid_first_line(const char *buffer);
int  has_valid_headers(const char *buffer);
void set_request_method(char *req_header, const char *buffer);

#endif
