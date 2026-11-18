#ifndef HTTPDL_H
#define HTTPDL_H
#include <stddef.h>

/* Download `url` to `out_path` using the PS5 native HTTP/2 + SSL stack.
 * Returns 0 on success (HTTP 200), -1 on failure. */
int http_download(const char *url, const char *out_path);

/* Download `nparts` URLs (parts of one .img.xz) sequentially, concatenating
 * their bytes into `out_fd`. The parts are byte chunks of one XZ stream;
 * the caller can then feed the resulting file to xzimg_decode() to obtain
 * a raw bootable .img. Returns 0 on success, -1 on failure. */
int http_download_parts(const char *const *urls, int nparts, int out_fd);

#endif
