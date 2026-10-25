#ifndef HTTPDL_H
#define HTTPDL_H
#include <stddef.h>

/* Download `url` to `out_path` using the PS5 native HTTP/2 + SSL stack.
 * Returns 0 on success (HTTP 200), -1 on failure. */
int http_download(const char *url, const char *out_path);
#endif
