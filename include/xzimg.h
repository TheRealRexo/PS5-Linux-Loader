/* xzimg.h -- streaming XZ decompression (parts -> raw .img)
 *
 * PS5-independent wrapper around the vendored LZMA SDK XzUnpacker
 * (source/lzma/, public domain). Compiles unchanged for the PS5 payload
 * and for the host unit test (tools/host_xz_test.c).
 */
#ifndef XZIMG_H
#define XZIMG_H

#include <stddef.h>
#include <stdint.h>

/*
 * Decompress an XZ stream split across `nparts` files and write the raw
 * (uncompressed) image to out_fd (already open for writing).
 *
 * `part_paths[0..nparts-1]` are the XZ parts in order: for a single
 * file, nparts == 1 and part_paths[0] is the .img.xz; for a split image
 * they are <name>.xz, <name>.xz.part2, <name>.xz.part3, ... (any byte
 * boundary is valid -- the parts are pure byte chunks of one stream).
 *
 * Parts are opened lazily, in order, and each is fully consumed before
 * the next is opened, so peak disk usage is one part plus the raw image.
 * Peak RAM is the LZMA2 dictionary (<= 64 MiB for the CI images) plus a
 * ~1.5 MiB scratch buffer. The XZ stream is verified (block CRCs, index
 * and footer) as it decodes; any corruption fails the call.
 *
 * on_progress is optional (NULL ok): called with the fraction of input
 * consumed in [0, 1] as decoding proceeds (may not reach 1.0 exactly).
 *
 * Returns 0 on success (full raw image written, stream verified);
 * *out_bytes (optional) is set to the number of raw bytes written.
 * Returns -1 on error; in that case out_fd holds a partial image and the
 * caller should remove it.
 */
int xzimg_decode(const char *const *part_paths, int nparts, int out_fd,
                 uint64_t *out_bytes,
                 void (*on_progress)(double frac, void *ctx), void *ctx);

#endif
