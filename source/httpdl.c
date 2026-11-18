#include "httpdl.h"
#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Provide the imported symbols. */
int sceNetInit(void);
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);
int sceSslInit(size_t);
int sceSslTerm(int);
int sceHttp2Init(int, int, size_t, int);
int sceHttp2Term(int);
int sceHttp2CreateTemplate(int, const char*, int, int);
int sceHttp2DeleteTemplate(int);
int sceHttp2CreateRequestWithURL(int, const char*, const char*, uint64_t);
int sceHttp2DeleteRequest(int);
int sceHttp2SendRequest(int, const void*, size_t);
int sceHttp2GetStatusCode(int, int*);
int sceHttp2ReadData(int, void*, size_t);

static int g_netMemId = -1, g_sslCtxId = -1, g_httpCtxId = -1;
static int g_tmplId = -1, g_reqId = -1;

static int net_init(void) {
  if (sceNetInit() != 0) return -1;
  g_netMemId = sceNetPoolCreate("ps5-linux-loader", 32*1024, 0);
  if (g_netMemId < 0) return -1;
  g_sslCtxId = sceSslInit(256*1024);
  if (g_sslCtxId < 0) return -1;
  g_httpCtxId = sceHttp2Init(g_netMemId, g_sslCtxId, 256*1024, 1);
  if (g_httpCtxId < 0) return -1;
  g_tmplId = sceHttp2CreateTemplate(g_httpCtxId, "ps5-linux-loader/1.0", 3, 1);
  if (g_tmplId < 0) return -1;
  return 0;
}

static void net_fini(void) {
  if (g_reqId != -1) { sceHttp2DeleteRequest(g_reqId); g_reqId = -1; }
  if (g_tmplId != -1) { sceHttp2DeleteTemplate(g_tmplId); g_tmplId = -1; }
  if (g_httpCtxId != -1) { sceHttp2Term(g_httpCtxId); g_httpCtxId = -1; }
  if (g_sslCtxId != -1) { sceSslTerm(g_sslCtxId); g_sslCtxId = -1; }
  if (g_netMemId != -1) { sceNetPoolDestroy(g_netMemId); g_netMemId = -1; }
}

int http_download_parts(const char *const *urls, int nparts, int out_fd) {
  if (!urls || nparts < 1 || out_fd < 0) return -1;
  /* Download each part sequentially and concatenate bytes into out_fd.
   * The parts are byte chunks of one XZ stream, so sequential download
   * followed by sequential feeding to xzimg_decode will produce a raw .img.
   * This function just writes the raw HTTP data to out_fd in order.
   */
  int failed = 0;
  char buf[0x8000];
  for (int i = 0; i < nparts && !failed; i++) {
    /* Temporarily download each part to a temp file, then concatenate.
     * Full streaming-into-out_fd would require hooking into HTTP read
     * callbacks, but this is sufficient for the WebUI integration.
     */
    char part_path[512];
    snprintf(part_path, sizeof(part_path), "/tmp/ps5_parts/%d.img.xz", i);
    /* Ensure temp dir exists */
    mkdir("/tmp/ps5_parts", 0755);
    if (http_download(urls[i], part_path) != 0) {
      failed = 1;
      break;
    }
    /* Read the downloaded part and write to out_fd */
    int part_fd = open(part_path, O_RDONLY);
    if (part_fd < 0) { failed = 1; break; }
    ssize_t n;
    while ((n = read(part_fd, buf, sizeof(buf))) > 0) {
      if (write(out_fd, buf, n) != n) { failed = 1; break; }
    }
    close(part_fd);
    unlink(part_path); /* cleanup */
    if (failed) break;
  }
  return failed ? -1 : 0;
}

int http_download(const char *url, const char *out_path) {
  if (!url || !out_path) return -1;
  if (net_init() != 0) { net_fini(); return -1; }

  g_reqId = sceHttp2CreateRequestWithURL(g_tmplId, "GET", url, 0);
  if (g_reqId < 0) { net_fini(); return -1; }

  if (sceHttp2SendRequest(g_reqId, NULL, 0) != 0) { net_fini(); return -1; }

  int status = -1;
  if (sceHttp2GetStatusCode(g_reqId, &status) != 0 || status != 200) {
    net_fini(); return -1;
  }

  int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { net_fini(); return -1; }

  char buf[0x4000];
  int n;
  while ((n = sceHttp2ReadData(g_reqId, buf, sizeof(buf))) > 0) {
    if (write(fd, buf, n) != n) { close(fd); net_fini(); return -1; }
  }
  close(fd);
  net_fini();
  return (n < 0) ? -1 : 0;
}

