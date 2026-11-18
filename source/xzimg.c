/* xzimg.c -- streaming XZ decompression (parts -> raw .img)
 *
 * PS5-independent wrapper around the vendored LZMA SDK XzUnpacker
 * (source/lzma/, public domain). The same translation unit compiles
 * for the PS5 payload (make) and for the host unit test
 * (tools/host_xz_test.c); it uses only POSIX open/read/write/stat.
 *
 * Z7_ST must be defined on the command line (single-thread build; the
 * multi-thread decoder is irrelevant here and pulls in MtDec.c).
 *
 * The parts are pure byte chunks of one XZ stream (any split point is
 * valid), so they are fed to the decoder sequentially, in order. A part
 * is opened only when needed and closed when exhausted, so peak disk
 * usage is one part plus the raw image; peak RAM is the LZMA2
 * dictionary (<= 64 MiB for the CI images) plus ~1.5 MiB of scratch.
 * Block CRCs, the stream index CRC and the footer are verified while
 * decoding; corruption fails the call.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "7zAlloc.h"
#include "7zCrc.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "xzimg.h"

#define XZIMG_INBUF (1u << 20)  /* 1 MiB input carry buffer */
#define XZIMG_OUTBUF (1u << 20) /* 1 MiB output scratch */

static const ISzAlloc g_xzimg_alloc = { SzAlloc, SzFree };

static int write_full(int fd, const void *buf, size_t n) {
  const uint8_t *p = (const uint8_t *)buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    p += (size_t)w;
    n -= (size_t)w;
  }
  return 0;
}

int xzimg_decode(const char *const *part_paths, int nparts, int out_fd,
                 uint64_t *out_bytes,
                 void (*on_progress)(double frac, void *ctx), void *ctx) {
  if (!part_paths || nparts < 1 || out_fd < 0)
    return -1;

  /* Total input size = sum of the parts, for progress reporting. */
  uint64_t total_in = 0;
  for (int i = 0; i < nparts; i++) {
    struct stat st;
    if (stat(part_paths[i], &st) != 0 || st.st_size < 0)
      return -1;
    total_in += (uint64_t)st.st_size;
  }
  if (total_in == 0)
    return -1;

  CrcGenerateTable();
  Crc64GenerateTable();

  CXzUnpacker xz;
  XzUnpacker_Construct(&xz, &g_xzimg_alloc);

  Byte *inbuf = (Byte *)malloc(XZIMG_INBUF);
  Byte *outbuf = (Byte *)malloc(XZIMG_OUTBUF);
  if (!inbuf || !outbuf) {
    XzUnpacker_Free(&xz);
    free(inbuf);
    free(outbuf);
    return -1;
  }

  int failed = 0;
  uint64_t in_consumed = 0;
  uint64_t decoded = 0;
  size_t carry = 0; /* inbuf[0..carry) not yet consumed by the decoder */

  for (int pi = 0; pi < nparts && !failed; pi++) {
    int last_part = (pi == nparts - 1);
    int fd = open(part_paths[pi], O_RDONLY);
    if (fd < 0) {
      failed = 1;
      break;
    }

    for (;;) {
      if (carry == 0) {
        ssize_t n = read(fd, inbuf, XZIMG_INBUF);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          failed = 1;
          break;
        }
        if (n == 0)
          break; /* part exhausted; next part (or final flush) */
        carry = (size_t)n;
      }

      SizeT src_len = carry;
      SizeT dest_len = XZIMG_OUTBUF;
      ECoderStatus status;
      SRes res = XzUnpacker_Code(&xz, outbuf, &dest_len, inbuf, &src_len,
                                 /* srcFinished */ 0, CODER_FINISH_ANY, &status);
      if (res != SZ_OK) {
        failed = 1;
        break;
      }
      if (dest_len > 0 &&
          write_full(out_fd, outbuf, (size_t)dest_len) != 0) {
        failed = 1;
        break;
      }
      in_consumed += (uint64_t)src_len;
      decoded += (uint64_t)dest_len;
      carry -= (size_t)src_len;
      if (carry > 0)
        memmove(inbuf, inbuf + src_len, carry);
      if (on_progress && src_len > 0)
        on_progress((double)in_consumed / (double)total_in, ctx);
    }
    close(fd);

    if (!last_part)
      continue; /* more input may follow */

    /* Final part exhausted: the decoder drains the 12-byte footer and
     * stream index byte-by-byte, so it needs repeated calls with empty
     * input until the stream is verified complete. */
    for (;;) {
      SizeT src_len = carry;
      SizeT dest_len = XZIMG_OUTBUF;
      ECoderStatus status;
      SRes res = XzUnpacker_Code(&xz, outbuf, &dest_len, inbuf, &src_len,
                                 /* srcFinished */ 1, CODER_FINISH_ANY, &status);
      if (dest_len > 0 &&
          write_full(out_fd, outbuf, (size_t)dest_len) != 0) {
        failed = 1;
        break;
      }
      in_consumed += (uint64_t)src_len;
      decoded += (uint64_t)dest_len;
      carry = 0;
      if (XzUnpacker_IsStreamWasFinished(&xz) == True)
        break; /* stream verified complete */
      if (res != SZ_OK) {
        failed = 1; /* actual decoder error */
        break;
      }
      /* Otherwise status == NEEDS_MORE_INPUT and the stream isn't
       * finished yet; loop continues with empty inbuf (carry=0)
       * and srcFinished=1 to drain the 12-byte footer + index CRC. */
    }
  }

  if (!failed && XzUnpacker_IsStreamWasFinished(&xz) != True)
    failed = 1;

  if (out_bytes)
    *out_bytes = failed ? 0 : decoded;

  XzUnpacker_Free(&xz);
  free(inbuf);
  free(outbuf);
  return failed ? -1 : 0;
}