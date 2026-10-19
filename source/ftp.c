#include "ftp.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FTP_CMD_MAX 4096
#define FTP_PATH_MAX 1024
#define FTP_LINE_MAX 2048

struct ftp_conn {
  int ctrl;
  int pasv;   /* listening data socket, -1 if none */
  char cwd[FTP_PATH_MAX]; /* virtual path, always absolute */
  const char *root;
  int logged_in;
  int rest; /* REST restart offset (for STOR/RETR) */
};

/* ---- tiny I/O helpers ---- */

static void ftp_send(struct ftp_conn *c, const char *fmt, ...) {
  char buf[FTP_LINE_MAX];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 3, fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  buf[n] = '\r';
  buf[n + 1] = '\n';
  buf[n + 2] = 0;
  size_t len = (size_t)n + 2;
  size_t off = 0;
  while (off < len) {
    ssize_t w = send(c->ctrl, buf + off, len - off, 0);
    if (w <= 0)
      return;
    off += (size_t)w;
  }
}

/* Read one CRLF-terminated command line into out (without CRLF). */
static int ftp_read_line(int fd, char *out, size_t outsz) {
  size_t o = 0;
  while (o + 1 < outsz) {
    char ch;
    ssize_t n = recv(fd, &ch, 1, 0);
    if (n <= 0)
      return -1;
    if (ch == '\n') {
      if (o > 0 && out[o - 1] == '\r')
        o--;
      out[o] = 0;
      return 0;
    }
    out[o++] = ch;
  }
  out[outsz - 1] = 0;
  return 0;
}

/* ---- path handling (virtual, rooted at c->root) ---- */

static void vnormalize(const char *cwd, const char *arg, char *virt,
                       size_t n) {
  char buf[FTP_PATH_MAX];
  if (arg[0] == '/')
    snprintf(buf, sizeof(buf), "%s", arg);
  else
    snprintf(buf, sizeof(buf), "%s/%s", cwd, arg);

  char comps[64][128];
  int top = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buf, "/", &save); tok;
       tok = strtok_r(NULL, "/", &save)) {
    if (!strcmp(tok, "."))
      continue;
    if (!strcmp(tok, "..")) {
      if (top > 0)
        top--;
      continue;
    }
    if (top < 64) {
      snprintf(comps[top], sizeof(comps[top]), "%s", tok);
      top++;
    }
  }
  if (top == 0) {
    snprintf(virt, n, "/");
    return;
  }
  size_t o = 0;
  virt[0] = 0;
  for (int i = 0; i < top && o + 2 < n; i++)
    o += (size_t)snprintf(virt + o, n - o, "/%s", comps[i]);
}

/* Map a virtual path to a physical one under root. */
static void vphysical(const char *root, const char *virt, char *out,
                      size_t outsz) {
  if (!strcmp(virt, "/"))
    snprintf(out, outsz, "%s", root);
  else
    snprintf(out, outsz, "%s%s", root, virt);
}

/* ---- passive data connection ---- */

static int ftp_data_open(struct ftp_conn *c) {
  if (c->pasv < 0)
    return -1;
  int d = accept(c->pasv, NULL, NULL);
  close(c->pasv);
  c->pasv = -1;
  return d;
}

static void ftp_pasv(struct ftp_conn *c) {
  if (c->pasv >= 0) {
    close(c->pasv);
    c->pasv = -1;
  }
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    ftp_send(c, "425 Can't open data connection.");
    return;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = 0;
  if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(s, 1) != 0) {
    close(s);
    ftp_send(c, "425 Can't open data connection.");
    return;
  }
  /* Advertise the address the client used to reach us. */
  struct sockaddr_in self, peer;
  socklen_t sl = sizeof(self);
  getsockname(c->ctrl, (struct sockaddr *)&self, &sl);
  socklen_t pl = sizeof(peer);
  getpeername(c->ctrl, (struct sockaddr *)&peer, &pl);
  socklen_t nl = sizeof(a);
  getsockname(s, (struct sockaddr *)&a, &nl);
  uint32_t ip = self.sin_addr.s_addr ? self.sin_addr.s_addr : peer.sin_addr.s_addr;
  unsigned char *b = (unsigned char *)&ip;
  unsigned p = ntohs(a.sin_port);
  c->pasv = s;
  ftp_send(c, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", b[0], b[1], b[2],
           b[3], p >> 8, p & 0xff);
}

/* ---- directory listing ---- */

static void ftp_list(struct ftp_conn *c, int names_only, const char *arg) {
  char virt[FTP_PATH_MAX];
  vnormalize(c->cwd, arg && *arg ? arg : ".", virt, sizeof(virt));
  char phys[FTP_PATH_MAX];
  vphysical(c->root, virt, phys, sizeof(phys));

  int d = ftp_data_open(c);
  if (d < 0) {
    ftp_send(c, "425 Use PASV first.");
    return;
  }
  DIR *dir = opendir(phys);
  if (!dir) {
    close(d);
    ftp_send(c, "550 Failed to open directory.");
    return;
  }
  ftp_send(c, "150 Here comes the directory listing.");
  struct dirent *de;
  char line[FTP_LINE_MAX];
  while ((de = readdir(dir))) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    if (names_only) {
      int n = snprintf(line, sizeof(line), "%s\r\n", de->d_name);
      send(d, line, (size_t)n, 0);
      continue;
    }
    char full[FTP_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", phys, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    char perms[11] = "-rw-r--r--";
    if (S_ISDIR(st.st_mode))
      perms[0] = 'd';
    else if (S_ISLNK(st.st_mode))
      perms[0] = 'l';
    char when[64];
    struct tm tmv;
    time_t mt = st.st_mtime;
    localtime_r(&mt, &tmv);
    strftime(when, sizeof(when), "%b %e %H:%M", &tmv);
    int n = snprintf(line, sizeof(line), "%s 1 owner group %12lld %s %s\r\n",
                     perms, (long long)st.st_size, when, de->d_name);
    if (n > 0)
      send(d, line, (size_t)n, 0);
  }
  closedir(dir);
  close(d);
  ftp_send(c, "226 Directory send OK.");
}

/* ---- transfers ---- */

static void ftp_retr(struct ftp_conn *c, const char *arg) {
  char virt[FTP_PATH_MAX];
  vnormalize(c->cwd, arg, virt, sizeof(virt));
  char phys[FTP_PATH_MAX];
  vphysical(c->root, virt, phys, sizeof(phys));

  int fd = open(phys, O_RDONLY);
  if (fd < 0) {
    ftp_send(c, "550 Failed to open file.");
    return;
  }
  if (c->rest > 0)
    lseek(fd, c->rest, SEEK_SET);
  c->rest = 0;
  struct stat st;
  fstat(fd, &st);

  int d = ftp_data_open(c);
  if (d < 0) {
    close(fd);
    ftp_send(c, "425 Use PASV first.");
    return;
  }
  ftp_send(c, "150 Opening data connection (%lld bytes).",
           (long long)st.st_size);

  char buf[65536];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = send(d, buf + off, (size_t)(r - off), 0);
      if (w <= 0)
        goto done;
      off += w;
    }
  }
done:
  close(fd);
  close(d);
  ftp_send(c, "226 Transfer complete.");
}

static void ftp_stor(struct ftp_conn *c, const char *arg) {
  char virt[FTP_PATH_MAX];
  vnormalize(c->cwd, arg, virt, sizeof(virt));
  char phys[FTP_PATH_MAX];
  vphysical(c->root, virt, phys, sizeof(phys));

  int flags = O_WRONLY | O_CREAT | (c->rest > 0 ? 0 : O_TRUNC);
  int fd = open(phys, flags, 0644);
  if (fd < 0) {
    ftp_send(c, "550 Failed to create file.");
    return;
  }
  if (c->rest > 0)
    lseek(fd, c->rest, SEEK_SET);
  c->rest = 0;

  int d = ftp_data_open(c);
  if (d < 0) {
    close(fd);
    ftp_send(c, "425 Use PASV first.");
    return;
  }
  ftp_send(c, "150 Ok to send data.");

  char buf[65536];
  ssize_t r;
  while ((r = recv(d, buf, sizeof(buf), 0)) > 0) {
    ssize_t off = 0;
    while (off < r) {
      ssize_t w = write(fd, buf + off, (size_t)(r - off));
      if (w <= 0)
        goto done;
      off += w;
    }
  }
done:
  close(fd);
  close(d);
  ftp_send(c, "226 Transfer complete.");
}

/* ---- command dispatch ---- */

static void ftp_session(struct ftp_conn *c) {
  ftp_send(c, "220 PS5 Linux Loader FTP ready.");
  char line[FTP_CMD_MAX];
  while (ftp_read_line(c->ctrl, line, sizeof(line)) == 0) {
    char *sp = strchr(line, ' ');
    char *arg = NULL;
    if (sp) {
      *sp = 0;
      arg = sp + 1;
    }
    for (char *p = line; *p; p++)
      if (*p >= 'a' && *p <= 'z')
        *p -= 32;
    if (!strcmp(line, "USER")) {
      c->logged_in = 1;
      ftp_send(c, "331 Anonymous login ok, send any password.");
    } else if (!strcmp(line, "PASS") || !strcmp(line, "ACCT")) {
      c->logged_in = 1;
      ftp_send(c, "230 Login successful.");
    } else if (!c->logged_in) {
      ftp_send(c, "530 Please login with USER and PASS.");
    } else if (!strcmp(line, "SYST")) {
      ftp_send(c, "215 UNIX Type: L8");
    } else if (!strcmp(line, "FEAT")) {
      ftp_send(c, "211-Features:");
      ftp_send(c, " SIZE");
      ftp_send(c, " MDTM");
      ftp_send(c, " REST STREAM");
      ftp_send(c, " PASV");
      ftp_send(c, "211 End");
    } else if (!strcmp(line, "PWD") || !strcmp(line, "XPWD")) {
      ftp_send(c, "257 \"%s\" is current directory.", c->cwd);
    } else if (!strcmp(line, "CWD")) {
      char virt[FTP_PATH_MAX];
      vnormalize(c->cwd, arg && *arg ? arg : "/", virt, sizeof(virt));
      char phys[FTP_PATH_MAX];
      vphysical(c->root, virt, phys, sizeof(phys));
      struct stat st;
      if (stat(phys, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(c->cwd, sizeof(c->cwd), "%s", virt);
        ftp_send(c, "250 Directory changed.");
      } else {
        ftp_send(c, "550 Failed to change directory.");
      }
    } else if (!strcmp(line, "CDUP")) {
      vnormalize(c->cwd, "..", c->cwd, sizeof(c->cwd));
      ftp_send(c, "250 Directory changed.");
    } else if (!strcmp(line, "TYPE")) {
      ftp_send(c, "200 Type set.");
    } else if (!strcmp(line, "MODE") || !strcmp(line, "STRU")) {
      ftp_send(c, "200 Command okay.");
    } else if (!strcmp(line, "NOOP")) {
      ftp_send(c, "200 NOOP ok.");
    } else if (!strcmp(line, "PASV")) {
      ftp_pasv(c);
    } else if (!strcmp(line, "LIST")) {
      ftp_list(c, 0, arg);
    } else if (!strcmp(line, "NLST")) {
      ftp_list(c, 1, arg);
    } else if (!strcmp(line, "RETR")) {
      ftp_retr(c, arg ? arg : "");
    } else if (!strcmp(line, "STOR") || !strcmp(line, "APPE")) {
      ftp_stor(c, arg ? arg : "");
    } else if (!strcmp(line, "REST")) {
      c->rest = arg ? atoi(arg) : 0;
      ftp_send(c, "350 Restarting at %d.", c->rest);
    } else if (!strcmp(line, "SIZE")) {
      char virt[FTP_PATH_MAX];
      vnormalize(c->cwd, arg ? arg : "", virt, sizeof(virt));
      char phys[FTP_PATH_MAX];
      vphysical(c->root, virt, phys, sizeof(phys));
      struct stat st;
      if (stat(phys, &st) == 0)
        ftp_send(c, "213 %lld", (long long)st.st_size);
      else
        ftp_send(c, "550 Could not get file size.");
    } else if (!strcmp(line, "MDTM")) {
      char virt[FTP_PATH_MAX];
      vnormalize(c->cwd, arg ? arg : "", virt, sizeof(virt));
      char phys[FTP_PATH_MAX];
      vphysical(c->root, virt, phys, sizeof(phys));
      struct stat st;
      if (stat(phys, &st) == 0) {
        struct tm tmv;
        time_t mt = st.st_mtime;
        gmtime_r(&mt, &tmv);
        char tbuf[32];
        strftime(tbuf, sizeof(tbuf), "%Y%m%d%H%M%S", &tmv);
        ftp_send(c, "213 %s", tbuf);
      } else {
        ftp_send(c, "550 Could not get file time.");
      }
    } else if (!strcmp(line, "DELE")) {
      char virt[FTP_PATH_MAX];
      vnormalize(c->cwd, arg ? arg : "", virt, sizeof(virt));
      char phys[FTP_PATH_MAX];
      vphysical(c->root, virt, phys, sizeof(phys));
      if (unlink(phys) == 0)
        ftp_send(c, "250 File deleted.");
      else
        ftp_send(c, "550 Delete failed.");
    } else if (!strcmp(line, "QUIT")) {
      ftp_send(c, "221 Goodbye.");
      break;
    } else {
      ftp_send(c, "502 Command not implemented.");
    }
  }
}

static void ftp_serve_conn(int ctrl, const char *root) {
  struct ftp_conn c;
  memset(&c, 0, sizeof(c));
  c.ctrl = ctrl;
  c.pasv = -1;
  c.root = root;
  snprintf(c.cwd, sizeof(c.cwd), "/");
  ftp_session(&c);
  if (c.pasv >= 0)
    close(c.pasv);
  close(ctrl);
}

int ftp_server_start(const char *root, int port) {
  if (!root || port <= 0)
    return 0;

  pid_t pid = fork();
  if (pid < 0)
    return -1;
  if (pid > 0)
    return (int)pid; /* parent keeps serving HTTP */

  /* child: become the FTP server */
  signal(SIGCHLD, SIG_IGN);
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    _exit(1);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons((uint16_t)port);
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 4) != 0)
    _exit(1);

  printf("[ftp] listening on port %d (root=%s)\n", port, root);

  for (;;) {
    int cfd = accept(srv, NULL, NULL);
    if (cfd < 0)
      continue;
    pid_t cc = fork();
    if (cc == 0) {
      close(srv);
      ftp_serve_conn(cfd, root);
      _exit(0);
    }
    close(cfd);
  }
}
