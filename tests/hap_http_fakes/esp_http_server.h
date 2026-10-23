#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef struct {
    size_t content_len;
    int fd;
} httpd_req_t;
int httpd_req_to_sockfd(httpd_req_t *req);
int httpd_req_recv(httpd_req_t *req, char *buffer, size_t size);
