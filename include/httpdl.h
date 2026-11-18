#ifndef HTTPDL_H
#define HTTPDL_H
#include <stddef.h>

/* Download `url` to `out_path` using the PS5 native HTTP/2 + SSL stack.
 * Returns 0 on success (HTTP 200), -1 on failure. */
int http_download(const char *url, const char *out_path);

/* Download `nparts` URLs (parts of one .img.xz) to /tmp/ps5_parts/ and
 * return their paths in `part_paths_out` (caller must free each string).
 * The parts are byte chunks of one XZ stream; feed the paths to
 * xzimg_decode() to obtain a raw bootable .img. Returns 0 on success,
 * -1 on failure. */
int http_download_parts(const char *const *urls, int nparts,
                        char **part_paths_out);

#endif
