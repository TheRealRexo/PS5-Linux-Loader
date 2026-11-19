#include "httpdl.h"
#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
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
/* GitHub release assets 302-redirect to the CDN; the SCE HTTP2 client
 * does not follow redirects by default, so every download returned a
 * non-200 status and failed. Enable auto-redirect on the request. */
int sceHttp2SetAutoRedirect(int, int);

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

int http_download_parts(const char *const *urls, int nparts, char **part_paths_out) {
  if (!urls || nparts < 1 || !part_paths_out) return -1;
  /* Download each part to a temp file under /tmp/ps5_parts/ and keep it
   * on disk. The caller feeds the resulting paths to xzimg_decode() to
   * decompress the concatenated XZ stream into a raw .img, then unlinks
   * the parts. Parts are byte chunks of one XZ stream, so any split point
   * is valid for the decoder. */
  if (mkdir("/tmp/ps5_parts", 0755) != 0 && errno != EEXIST) return -1;
  int failed = 0;
  for (int i = 0; i < nparts && !failed; i++) {
    char part_path[512];
    snprintf(part_path, sizeof(part_path), "/tmp/ps5_parts/%d.img.xz", i);
    if (http_download(urls[i], part_path) != 0) {
      failed = 1;
      break;
    }
    part_paths_out[i] = strdup(part_path);
    if (!part_paths_out[i]) { failed = 1; break; }
  }
  return failed ? -1 : 0;
}

int http_download(const char *url, const char *out_path) {
  if (!url || !out_path) return -1;
  if (net_init() != 0) { net_fini(); return -1; }

  g_reqId = sceHttp2CreateRequestWithURL(g_tmplId, "GET", url, 0);
  if (g_reqId < 0) { net_fini(); return -1; }

  /* Follow GitHub's 302 redirect to the release-asset CDN. */
  if (sceHttp2SetAutoRedirect(g_reqId, 1) != 0) { net_fini(); return -1; }

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

