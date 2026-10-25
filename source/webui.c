#include "webui.h"
#include "httpdl.h"
#include "utils.h"
#include "images_manifest.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_REQ 16384
#define UPLOAD_CHUNK 65536
#define MAX_IMGS 64
#define FM_MAX_ENTRIES 256

static const char *DEFAULT_PATHS[] = {
    "/mnt/usb0/",           "/mnt/usb1/",           "/mnt/usb2/",
    "/mnt/usb3/",           "/mnt/ext1/",           "/mnt/usb0/PS5/Linux/",
    "/mnt/usb1/PS5/Linux/", "/mnt/usb2/PS5/Linux/", "/mnt/usb3/PS5/Linux/",
};

static const char *TARGETS[][2] = {
    {"none", "None (boot only)"}, {"usb0", "USB port 0"},
    {"usb1", "USB port 1"},       {"usb2", "USB port 2"},
    {"usb3", "USB port 3"},       {"m2", "Internal M.2 SSD"}};

void webui_settings_init(struct webui_settings *s) {
  memset(s, 0, sizeof(*s));
  int n = (int)(sizeof(DEFAULT_PATHS) / sizeof(DEFAULT_PATHS[0]));
  for (int i = 0; i < n && i < WEBUI_MAX_PATHS; i++) {
    snprintf(s->paths[i], WEBUI_PATH_LEN, "%s", DEFAULT_PATHS[i]);
  }
  s->num_paths = (n < WEBUI_MAX_PATHS) ? n : WEBUI_MAX_PATHS;
  snprintf(s->target, sizeof(s->target), "none");
  s->mode = WEBUI_MODE_BOOT;
  s->size_gb = 0;
  s->vram_mb = 0;
  /* default upload / file-manager dir: first existing search path */
  for (int i = 0; i < s->num_paths; i++) {
    struct stat st;
    if (stat(s->paths[i], &st) == 0 && S_ISDIR(st.st_mode)) {
      snprintf(s->upload_dir, WEBUI_PATH_LEN, "%s", s->paths[i]);
      break;
    }
  }
  if (!s->upload_dir[0])
    snprintf(s->upload_dir, WEBUI_PATH_LEN, "/mnt/usb0/");
}

/* ---- small helpers ---- */

static void html_escape(const char *in, char *out, size_t outsize) {
  size_t o = 0;
  for (const char *p = in; *p && o + 7 < outsize; p++) {
    switch (*p) {
    case '&':
      o += (size_t)snprintf(out + o, outsize - o, "&amp;");
      break;
    case '<':
      o += (size_t)snprintf(out + o, outsize - o, "&lt;");
      break;
    case '>':
      o += (size_t)snprintf(out + o, outsize - o, "&gt;");
      break;
    case '"':
      o += (size_t)snprintf(out + o, outsize - o, "&quot;");
      break;
    default:
      out[o++] = *p;
      break;
    }
  }
  out[o] = 0;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return 0;
}

static void url_decode(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '+') {
      *o++ = ' ';
    } else if (*p == '%' && p[1] && p[2]) {
      *o++ = (char)((hexval(p[1]) << 4) | hexval(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
}

static void send_str(int fd, const char *s) {
  size_t n = strlen(s);
  while (n) {
    ssize_t w = send(fd, s, n, 0);
    if (w <= 0)
      return;
    s += w;
    n -= (size_t)w;
  }
}

static void send_ok_headers(int fd) {
  send_str(fd,
           "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html; charset=utf-8\r\n"
           "Connection: close\r\n\r\n");
}

static void send_redirect(int fd) {
  send_str(fd,
           "HTTP/1.1 303 See Other\r\n"
           "Location: /\r\n"
           "Content-Length: 0\r\n"
           "Connection: close\r\n\r\n");
}

static void send_plain(int fd, int code, const char *body) {
  char hdr[128];
  snprintf(hdr, sizeof(hdr),
           "HTTP/1.1 %d\r\nContent-Type: text/html; charset=utf-8\r\n"
           "Content-Length: %zu\r\nConnection: close\r\n\r\n",
           code, strlen(body));
  send_str(fd, hdr);
  send_str(fd, body);
}

/* Pull a header value (case-insensitive). Returns 1 if found. */
static int header_get(const char *req, const char *key, char *out,
                      size_t outsize) {
  const char *p = req;
  size_t klen = strlen(key);
  while (*p) {
    const char *line_end = strstr(p, "\r\n");
    if (!line_end)
      break;
    size_t line_len = (size_t)(line_end - p);
    if (line_len > klen) {
      int match = 1;
      for (size_t i = 0; i < klen; i++) {
        char a = p[i], b = key[i];
        if (a >= 'A' && a <= 'Z')
          a += 32;
        if (b >= 'A' && b <= 'Z')
          b += 32;
        if (a != b) {
          match = 0;
          break;
        }
      }
      if (match && p[klen] == ':') {
        const char *v = p + klen + 1;
        while (v < line_end && (*v == ' ' || *v == '\t'))
          v++;
        size_t vlen = (size_t)(line_end - v);
        if (vlen >= outsize)
          vlen = outsize - 1;
        memcpy(out, v, vlen);
        out[vlen] = 0;
        return 1;
      }
    }
    p = line_end + 2;
  }
  return 0;
}

/* Extract urlencoded field `name` from body into out. */
static int form_get(const char *body, const char *name, char *out,
                    size_t outsize) {
  out[0] = 0;
  size_t nlen = strlen(name);
  const char *p = body;
  while (*p) {
    const char *eq = strchr(p, '=');
    const char *amp = strchr(p, '&');
    if (!eq)
      break;
    if ((size_t)(eq - p) == nlen && !strncmp(p, name, nlen)) {
      const char *v = eq + 1;
      size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
      if (vlen >= outsize)
        vlen = outsize - 1;
      memcpy(out, v, vlen);
      out[vlen] = 0;
      url_decode(out);
      return 1;
    }
    if (!amp)
      break;
    p = amp + 1;
  }
  return 0;
}

/* Split multiline text into settings paths (append trailing slash). */
static void parse_paths(struct webui_settings *s, char *text) {
  s->num_paths = 0;
  char *save = NULL;
  for (char *line = strtok_r(text, "\r\n", &save);
       line && s->num_paths < WEBUI_MAX_PATHS;
       line = strtok_r(NULL, "\r\n", &save)) {
    if (!*line)
      continue;
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] != '/' && len + 1 < WEBUI_PATH_LEN) {
      line[len] = '/';
      line[len + 1] = 0;
    }
    snprintf(s->paths[s->num_paths], WEBUI_PATH_LEN, "%s", line);
    s->num_paths++;
  }
}

/* Split multiline key=value text into settings overrides. */
static void parse_overrides(struct webui_settings *s, char *text) {
  s->num_overrides = 0;
  char *save = NULL;
  for (char *line = strtok_r(text, "\r\n", &save);
       line && s->num_overrides < WEBUI_MAX_OVERRIDES;
       line = strtok_r(NULL, "\r\n", &save)) {
    if (!*line)
      continue;
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    snprintf(s->override_keys[s->num_overrides], WEBUI_KEY_LEN, "%s", line);
    snprintf(s->override_vals[s->num_overrides], WEBUI_VAL_LEN, "%s", eq + 1);
    s->num_overrides++;
  }
}

/* ---- file scanning ---- */

static void append_found_row(char *html, size_t htmlsz, const char *dir,
                             const char *name) {
  char full[256];
  struct stat st;
  snprintf(full, sizeof(full), "%s%s", dir, name);
  if (stat(full, &st) != 0)
    return;
  char esc_path[512];
  html_escape(full, esc_path, sizeof(esc_path));
  size_t used = strlen(html);
  snprintf(html + used, htmlsz - used,
           "<tr><td>%s</td><td class=num>%lld</td></tr>", esc_path,
           (long long)st.st_size);
}

struct img_entry {
  char path[512];
  long long size;
};

static int collect_imgs(const struct webui_settings *s, struct img_entry *out,
                        int max) {
  int n = 0;
  for (int i = 0; i < s->num_paths && n < max; i++) {
    DIR *d = opendir(s->paths[i]);
    if (!d)
      continue;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
      size_t l = strlen(de->d_name);
      if (l <= 4 || strcmp(de->d_name + l - 4, ".img"))
        continue;
      char full[512];
      snprintf(full, sizeof(full), "%s%s", s->paths[i], de->d_name);
      struct stat st;
      if (stat(full, &st) != 0 || !S_ISREG(st.st_mode))
        continue;
      snprintf(out[n].path, sizeof(out[n].path), "%s", full);
      out[n].size = (long long)st.st_size;
      n++;
    }
    closedir(d);
  }
  return n;
}

/* Build the scan table (found boot files). Caller frees. */
static char *build_scan_table(const struct webui_settings *s) {
  size_t cap = 16384;
  char *html = calloc(1, cap);
  if (!html)
    return NULL;
  static const char *NEEDED[] = {"bzImage", "initrd.img", "cmdline.txt",
                                 "vram.txt"};
  for (int i = 0; i < s->num_paths; i++) {
    for (size_t k = 0; k < sizeof(NEEDED) / sizeof(NEEDED[0]); k++) {
      size_t used = strlen(html);
      if (used + 512 >= cap)
        break;
      append_found_row(html, cap, s->paths[i], NEEDED[k]);
    }
  }
  if (!html[0])
    snprintf(html, cap,
             "<tr><td colspan=2 class=dim>No files found in the search paths."
             "</td></tr>");
  return html;
}

/* Build the OS-image radio list. Caller frees. */
static char *build_image_list(const struct webui_settings *s) {
  size_t cap = 16384;
  char *html = calloc(1, cap);
  if (!html)
    return NULL;
  static struct img_entry imgs[MAX_IMGS];
  int n = collect_imgs(s, imgs, MAX_IMGS);

  size_t used = strlen(html);
  snprintf(html + used, cap - used,
           "<label class=opt><input type=radio name=image value='' %s>"
           "Auto-detect (first *.img on the search paths)</label><br>",
           s->image[0] ? "" : "checked");
  for (int i = 0; i < n; i++) {
    char esc[1024];
    html_escape(imgs[i].path, esc, sizeof(esc));
    used = strlen(html);
    if (used + 1200 >= cap)
      break;
    snprintf(html + used, cap - used,
             "<label class=opt><input type=radio name=image value='%s' %s>"
             "%s <span class=dim>(%lld MB)</span></label><br>",
             esc, (!strcmp(s->image, imgs[i].path)) ? "checked" : "", esc,
             imgs[i].size / (1024 * 1024));
  }
  if (n == 0) {
    used = strlen(html);
    snprintf(html + used, cap - used,
             "<span class=dim>No *.img found on the search paths. "
             "Upload one below or via FTP.</span>");
  }
  return html;
}

/* Render the official-OS-image download table from the embedded manifest. */
static void render_manifest(int fd) {
  send_str(fd, "<h2>Official OS images (download)</h2>"
               "<p class=sub>Download the .img.xz from R2, decompress it to "
               "get the .img, then transfer the .img to the PS5 (USB stick "
               "or internal M.2 via FTP port 21). The boot files "
               "(bzImage, initrd.img, cmdline.txt, vram.txt) come from the "
               "matching *-boot.tar.gz release asset; the loader derives "
               "cmdline (root label) and VRAM from the selected .img.</p>"
               "<table>"
               "<tr><td>Distro</td><td class=num>.img.xz</td>"
               "<td>Download (R2)</td></tr>");
  for (size_t i = 0; i < images_manifest_count; i++) {
    char esc_name[256], esc_url[512], esc_file[256];
    html_escape(images_manifest[i].name, esc_name, sizeof(esc_name));
    html_escape(images_manifest[i].img_url, esc_url, sizeof(esc_url));
    const char *file = strrchr(images_manifest[i].img_url, '/');
    file = file ? file + 1 : images_manifest[i].img_url;
    html_escape(file, esc_file, sizeof(esc_file));
    send_str(fd, "<tr><td>");
    send_str(fd, esc_name);
    send_str(fd, "</td><td class=num>");
    send_str(fd, esc_file);
    send_str(fd, "</td><td><a href='");
    send_str(fd, esc_url);
    send_str(fd, "'>link</a>"
               "<form method=post action='/download' style='display:inline;margin-left:6px'>"
               "<input type=hidden name=url value='");
    send_str(fd, esc_url);
    send_str(fd, "'>"
               "<input type=hidden name=dir value='");
    send_str(fd, "");
    send_str(fd, "'>"
               "<button class='btn' type=submit>Download here</button></form></td></tr>");
  }
  send_str(fd, "</table>");
}

/* File-manager listing for `dir`. Caller frees. */
static char *build_file_listing(const char *dir) {
  size_t cap = 32768;
  char *html = calloc(1, cap);
  if (!html)
    return NULL;
  DIR *d = opendir(dir);
  if (!d) {
    snprintf(html, cap, "<p class=dim>Cannot open %s: %s</p>", dir,
             strerror(errno));
    return html;
  }
  struct dirent *de;
  int count = 0;
  while ((de = readdir(d)) && count < FM_MAX_ENTRIES) {
    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
      continue;
    char full[1024];
    snprintf(full, sizeof(full), "%s%s%s", dir,
             (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", de->d_name);
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    char esc_name[512], esc_full[2048], esc_dir[2048];
    html_escape(de->d_name, esc_name, sizeof(esc_name));
    html_escape(full, esc_full, sizeof(esc_full));
    html_escape(dir, esc_dir, sizeof(esc_dir));
    size_t used = strlen(html);
    if (used + 2500 >= cap)
      break;
    if (S_ISDIR(st.st_mode)) {
      snprintf(html + used, cap - used,
               "<tr><td><a href='/files?dir=%s'>%s/</a></td>"
               "<td class=num>dir</td><td></td></tr>",
               esc_full, esc_name);
    } else {
      snprintf(html + used, cap - used,
               "<tr><td>%s</td><td class=num>%lld</td>"
               "<td><form method=post action=/delete style='display:inline'>"
               "<input type=hidden name=dir value='%s'>"
               "<input type=hidden name=name value='%s'>"
               "<button class='btn del' type=submit>Delete</button>"
               "</form></td></tr>",
               esc_name, (long long)st.st_size, esc_dir, esc_name);
    }
    count++;
  }
  closedir(d);
  if (!html[0])
    snprintf(html, cap, "<tr><td colspan=3 class=dim>Empty.</td></tr>");
  return html;
}

/* ---- page rendering ---- */

static void render_page(int fd, const struct webui_settings *s,
                        const char *fm_dir) {
  char esc_paths[WEBUI_MAX_PATHS * WEBUI_PATH_LEN * 2];
  char esc_ovr[WEBUI_MAX_OVERRIDES * (WEBUI_KEY_LEN + WEBUI_VAL_LEN + 4)];
  size_t po = 0, oo = 0;
  esc_paths[0] = 0;
  esc_ovr[0] = 0;
  for (int i = 0; i < s->num_paths; i++) {
    char tmp[WEBUI_PATH_LEN * 2];
    html_escape(s->paths[i], tmp, sizeof(tmp));
    po += (size_t)snprintf(esc_paths + po, sizeof(esc_paths) - po, "%s\n", tmp);
  }
  for (int i = 0; i < s->num_overrides; i++) {
    char tmp[(WEBUI_KEY_LEN + WEBUI_VAL_LEN) * 2];
    html_escape(s->override_keys[i], tmp, sizeof(tmp));
    oo += (size_t)snprintf(esc_ovr + oo, sizeof(esc_ovr) - oo, "%s=", tmp);
    html_escape(s->override_vals[i], tmp, sizeof(tmp));
    oo += (size_t)snprintf(esc_ovr + oo, sizeof(esc_ovr) - oo, "%s\n", tmp);
  }

  char *scan = build_scan_table(s);
  char *imgs = build_image_list(s);
  char *files = build_file_listing(fm_dir);

  const char *dir = fm_dir && *fm_dir ? fm_dir : s->upload_dir;
  char esc_dir[2048];
  html_escape(dir, esc_dir, sizeof(esc_dir));

  send_ok_headers(fd);
  send_str(fd,
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'>"
           "<title>PS5 Linux Loader</title><style>"
           "body{background:#0b0e14;color:#c9d1d9;font:14px/1.5 system-ui,"
           "sans-serif;margin:0;padding:24px}"
           ".wrap{max-width:820px;margin:0 auto}"
           "h1{font-size:20px;color:#58a6ff;margin:0 0 4px}"
           ".sub{color:#8b949e;margin:0 0 20px}"
           "h2{font-size:15px;color:#58a6ff;border-bottom:1px solid #21262d;"
           "padding-bottom:6px;margin:24px 0 10px}"
           "table{width:100%;border-collapse:collapse;font-size:13px}"
           "td{padding:5px 8px;border-bottom:1px solid #161b22;vertical-align:"
           "middle}"
           "td.num{text-align:right;color:#8b949e;font-family:monospace}"
           "td.dim{color:#8b949e}"
           "a{color:#58a6ff;text-decoration:none}a:hover{text-decoration:underline}"
           "textarea,input[type=text]{width:100%;box-sizing:border-box;"
           "background:#161b22;color:#c9d1d9;border:1px solid #30363d;"
           "border-radius:6px;padding:8px;font:13px/1.4 monospace;margin:4px 0 14px}"
           "input[type=number]{width:120px;background:#161b22;color:#c9d1d9;"
           "border:1px solid #30363d;border-radius:6px;padding:6px}"
           "select{background:#161b22;color:#c9d1d9;border:1px solid #30363d;"
           "border-radius:6px;padding:6px}"
           ".btn{background:#238636;color:#fff;border:0;border-radius:6px;"
           "padding:9px 16px;font-size:14px;cursor:pointer;margin-right:8px}"
           ".btn.alt{background:#21262d;color:#c9d1d9}"
           ".btn.del{background:#4d1f1f;color:#f0b7b7;padding:3px 10px;"
           "font-size:12px}"
           "label{color:#8b949e;font-size:13px}"
           ".opt{color:#c9d1d9;display:block;padding:2px 0}"
           ".row{display:flex;gap:16px;flex-wrap:wrap;align-items:center;"
           "margin:6px 0 14px}"
           ".box{background:#11151c;border:1px solid #21262d;border-radius:8px;"
           "padding:12px 14px}"
           "</style></head><body><div class=wrap>"
           "<h1>PS5 Linux Loader</h1>"
           "<p class=sub>Pick an OS image and target, manage files, then boot. "
           "There is no auto-boot: configure the options below and click "
           "Boot Linux. Transfer large .img files over FTP (port 21, "
           "anonymous login) while this page is open.</p>");

  send_str(fd, "<h2>Detected files</h2><table>");
  if (scan)
    send_str(fd, scan);
  send_str(fd, "</table>");
  free(scan);

  send_str(fd, "<h2>Boot / install</h2><form method=post action=/boot>"
               "<div class=box><label>OS image</label>");
  if (imgs)
    send_str(fd, imgs);
  send_str(fd, "<div class=row><label>Target</label><select name=target>");
  for (size_t i = 0; i < sizeof(TARGETS) / sizeof(TARGETS[0]); i++) {
    send_str(fd, "<option value='");
    send_str(fd, TARGETS[i][0]);
    send_str(fd, "'");
    if (!strcmp(s->target, TARGETS[i][0]))
      send_str(fd, " selected");
    send_str(fd, ">");
    send_str(fd, TARGETS[i][1]);
    send_str(fd, "</option>");
  }
  send_str(fd, "</select>"
               "<label>Install size&nbsp;(GB, 0 = full)</label>"
               "<input type=number name=size_gb min=0 value='");
  {
    char num[16];
    snprintf(num, sizeof(num), "%d", s->size_gb);
    send_str(fd, num);
  }
  send_str(fd, "'>"
               "<label>VRAM&nbsp;(MiB, 0 = auto)</label>"
               "<input type=number name=vram_mb min=0 value='");
  {
    char num[16];
    snprintf(num, sizeof(num), "%d", s->vram_mb);
    send_str(fd, num);
  }
  send_str(fd, "'>"
               "<label class=opt><input type=radio name=mode value=boot");
  if (s->mode == WEBUI_MODE_BOOT)
    send_str(fd, " checked");
  send_str(fd, "> Boot only</label>"
               "<label class=opt><input type=radio name=mode value=install");
  if (s->mode == WEBUI_MODE_INSTALL)
    send_str(fd, " checked");
  send_str(fd, "> Install to target</label></div></div>");

  render_manifest(fd);

  send_str(fd, "<h2>Search &amp; overrides</h2>"
               "<label>Search paths (one per line)</label>"
               "<textarea name=paths rows=6 spellcheck=false>");
  send_str(fd, esc_paths);
  send_str(fd,
           "</textarea><label>Filename overrides (key=value per line, e.g. "
           "bzImage=/mnt/usb0/custom/bzImage)</label>"
           "<textarea name=overrides rows=3 spellcheck=false>");
  send_str(fd, esc_ovr);
  send_str(fd, "</textarea>"
               "<button class='btn' type=submit>Boot Linux</button>"
               "</form>"
               "<form method=post action=/skip style='display:inline'>"
               "<button class='btn alt' type=submit>Boot with defaults"
               "</button></form>");

  /* Hardware / driver support table */
  {
    int wifi_fw = 0;
    for (int i = 0; i < s->num_paths; i++) {
      char full[256];
      struct stat st;
      snprintf(full, sizeof(full), "%slib/nxp/pcieuartiw620_combo_v1.bin",
               s->paths[i]);
      /* ensure trailing slash handling */
      if (s->paths[i][0] && s->paths[i][strlen(s->paths[i]) - 1] != '/')
        snprintf(full, sizeof(full), "%s/lib/nxp/pcieuartiw620_combo_v1.bin",
                 s->paths[i]);
      if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        wifi_fw = 1;
        break;
      }
    }
    send_str(fd, "<h2>Hardware / driver support</h2><table>"
                 "<tr><td>GPU</td><td>AMD Radeon (Oberon)</td>"
                 "<td>amdgpu</td><td>Supported</td></tr>"
                 "<tr><td>CPU</td><td>AMD Zen 2</td>"
                 "<td>-</td><td>Supported</td></tr>"
                 "<tr><td>WiFi</td><td>MediaTek MT7921 (pcieuartiw620)</td>"
                 "<td>mt7921e</td><td>");
    send_str(fd, wifi_fw ? "Firmware present" : "Firmware missing");
    send_str(fd, "</td></tr>"
                 "<tr><td>Bluetooth</td><td>MediaTek MT7921</td>"
                 "<td>btusb</td><td>Supported</td></tr>"
                 "<tr><td>Ethernet</td><td>Realtek RTL8111/8168</td>"
                 "<td>r8169</td><td>Supported</td></tr>"
                 "<tr><td>Audio</td><td>AMD ACP</td>"
                 "<td>snd_acp3x</td><td>Supported</td></tr>"
                 "<tr><td>NVMe (M.2)</td><td>-</td>"
                 "<td>nvme</td><td>Supported</td></tr>"
                 "<tr><td>USB</td><td>-</td>"
                 "<td>xhci</td><td>Supported</td></tr>"
                 "</table>");
  }

  send_str(fd, "<h2>Files</h2>"
               "<form method=get action=/files class=row>"
               "<input type=text name=dir value='");
  send_str(fd, esc_dir);
  send_str(fd, "' placeholder='/mnt/usb0/'>"
               "<button class='btn alt' type=submit>List</button></form>"
               "<form method=post action='/upload?dir=");
  send_str(fd, esc_dir);
  send_str(fd, "' enctype=multipart/form-data class=row>"
               "<input type=file name=file>"
               "<button class='btn' type=submit>Upload here</button></form>"
               "<table>");
  if (files)
    send_str(fd, files);
  send_str(fd, "</table>"
               "<p class=sub>Or copy large images over FTP (port 21, "
               "anonymous login) into the same directory.</p>"
               "</div></body></html>");
  free(imgs);
  free(files);
}

/* ---- upload (multipart/form-data, streamed) ---- */

static char *find_mem(const char *hay, size_t haylen, const char *needle,
                      size_t nlen) {
  if (nlen == 0 || haylen < nlen)
    return NULL;
  for (size_t i = 0; i + nlen <= haylen; i++)
    if (!memcmp(hay + i, needle, nlen))
      return (char *)(hay + i);
  return NULL;
}

/* Read from cfd into acc until it can no longer grow; returns bytes held. */
static int recv_more(int cfd, char *acc, size_t acclen, size_t cap) {
  if (acclen >= cap)
    return -1;
  ssize_t n = recv(cfd, acc + acclen, cap - acclen, 0);
  if (n <= 0)
    return -1;
  return (int)(acclen + (size_t)n);
}

/*
 * Handle a multipart/form-data upload of a single file part, writing it into
 * `dir`. Returns 0 on success. Streams so multi-GB images are fine.
 */
static int handle_upload(int cfd, const char *req, const char *header_end,
                         int clen, int body_buffered, const char *dir) {
  char ctype[256] = {0};
  header_get(req, "Content-Type", ctype, sizeof(ctype));
  char *bp = strstr(ctype, "boundary=");
  if (!bp)
    return -1;
  bp += 9;
  if (*bp == '"')
    bp++;
  char boundary[200];
  size_t bl = 0;
  while (*bp && *bp != '"' && *bp != ';' && *bp != '\r' && *bp != '\n' &&
         bl < sizeof(boundary) - 1)
    boundary[bl++] = *bp++;
  boundary[bl] = 0;
  if (!bl)
    return -1;

  /* body position + length */
  const char *body = header_end + 4;
  size_t body_in_buf = (body_buffered > 0) ? (size_t)body_buffered : 0;
  if (body_in_buf > (size_t)clen)
    body_in_buf = (size_t)clen;

  char delim[256];
  snprintf(delim, sizeof(delim), "\r\n--%s", boundary);
  size_t dlen = strlen(delim);

  /* accumulator to search for the terminating delimiter */
  size_t cap = UPLOAD_CHUNK + dlen * 2;
  char *acc = malloc(cap);
  if (!acc)
    return -1;
  size_t acclen = 0;

  /* seed with any buffered body */
  if (body_in_buf) {
    memcpy(acc, body, body_in_buf);
    acclen = body_in_buf;
  }

  /* find end of part headers (first \r\n\r\n) -> data start */
  size_t hdr_end = 0;
  int found_hdr = 0;
  for (;;) {
    char *p = find_mem(acc, acclen, "\r\n\r\n", 4);
    if (p) {
      hdr_end = (size_t)(p - acc) + 4;
      found_hdr = 1;
      break;
    }
    if (acclen >= cap - 1) {
      free(acc);
      return -1;
    }
    int r = recv_more(cfd, acc, acclen, cap - 1);
    if (r < 0) {
      free(acc);
      return -1;
    }
    acclen = (size_t)r;
  }
  if (!found_hdr) {
    free(acc);
    return -1;
  }

  /* parse filename from part headers */
  char filename[256] = "upload.img";
  {
    char *cd = find_mem(acc, hdr_end, "filename=\"", 10);
    if (cd) {
      char *q = cd + 10;
      size_t k = 0;
      while (*q && *q != '"' && k < sizeof(filename) - 1)
        filename[k++] = *q++;
      filename[k] = 0;
    }
  }

  /* target file: dir + basename(filename) */
  const char *base = strrchr(filename, '/');
  base = base ? base + 1 : filename;
  char outpath[1024];
  snprintf(outpath, sizeof(outpath), "%s%s%s", dir,
           (dir[0] && dir[strlen(dir) - 1] == '/') ? "" : "/", base);
  char tmppath[1100];
  snprintf(tmppath, sizeof(tmppath), "%s.part", outpath);

  int fd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    free(acc);
    return -1;
  }

  /* stream data from hdr_end until the delimiter */
  size_t off = hdr_end;
  int done = 0;
  while (!done) {
    char *p = find_mem(acc + off, acclen - off, delim, dlen);
    if (p) {
      size_t write_len = (size_t)(p - (acc + off));
      if (write_len)
        write(fd, acc + off, write_len);
      done = 1;
      break;
    }
    /* keep a dlen-byte tail so a delimiter split across reads is caught */
    if (acclen - off > dlen) {
      size_t flush = acclen - off - dlen;
      write(fd, acc + off, flush);
      memmove(acc, acc + off + flush, dlen);
      acclen = dlen;
      off = 0;
    } else {
      /* shift remaining to front and read more */
      memmove(acc, acc + off, acclen - off);
      acclen -= off;
      off = 0;
    }
    if (acclen >= cap - 1) {
      /* safety: force a flush to avoid unbounded growth */
      write(fd, acc, acclen - dlen);
      memmove(acc, acc + acclen - dlen, dlen);
      acclen = dlen;
    }
    size_t cap_for_read = cap - 1;
    ssize_t n = recv(cfd, acc + acclen, cap_for_read - acclen, 0);
    if (n <= 0) {
      /* client closed: finalize what we have */
      if (acclen > off)
        write(fd, acc + off, acclen - off);
      break;
    }
    acclen += (size_t)n;
  }

  close(fd);
  free(acc);
  /* atomic-ish publish */
  if (rename(tmppath, outpath) != 0) {
    unlink(tmppath);
    return -1;
  }
  printf("[webui] uploaded %s (%s)\n", outpath, filename);
  return 0;
}

/* ---- network ---- */

static int get_local_ip(char *out, size_t outsize) {
  struct ifaddrs *ifa, *p;
  out[0] = 0;
  if (getifaddrs(&ifa) != 0)
    return -1;
  for (p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
      continue;
    if (!strcmp(p->ifa_name, "lo0"))
      continue;
    struct sockaddr_in *sa = (struct sockaddr_in *)p->ifa_addr;
    if (!inet_ntop(AF_INET, &sa->sin_addr, out, outsize))
      continue;
    freeifaddrs(ifa);
    return 0;
  }
  freeifaddrs(ifa);
  return -1;
}

/* Read the full request head (up to \r\n\r\n) into req. Returns total. */
static int read_request(int cfd, char *req, int max) {
  int total = 0;
  while (total < max - 1) {
    ssize_t n = recv(cfd, req + total, max - 1 - total, 0);
    if (n <= 0)
      break;
    total += (int)n;
    req[total] = 0;
    if (strstr(req, "\r\n\r\n"))
      break;
  }
  req[total] = 0;
  return total;
}

static int read_body(int cfd, char *req, int total, int clen) {
  char *body = strstr(req, "\r\n\r\n");
  if (!body)
    return 0;
  body += 4;
  int have = total - (int)(body - req);
  while (have < clen && have < MAX_REQ - 1) {
    ssize_t n = recv(cfd, body + have, MAX_REQ - 1 - have, 0);
    if (n <= 0)
      break;
    have += (int)n;
  }
  if (have < 0)
    have = 0;
  body[have] = 0;
  return have;
}

int webui_run(struct webui_settings *s, int port, int timeout_sec) {
  int srv = socket(AF_INET, SOCK_STREAM, 0);
  if (srv < 0)
    return -1;

  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);

  if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(srv);
    return -1;
  }
  if (listen(srv, 4) != 0) {
    close(srv);
    return -1;
  }

  char ip[64];
  if (get_local_ip(ip, sizeof(ip)) != 0)
    snprintf(ip, sizeof(ip), "<PS5-IP>");
  printf("[webui] listening on http://%s:%d\n", ip, port);
  notify("Open http://%s:%d to configure (FTP on port 21).\n"
         "Auto-boots in ~2 min if unused.",
         ip, port);

  time_t deadline = time(NULL) + timeout_sec;
  int result = -1;

  while (time(NULL) < deadline) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(srv, &rfds);
    struct timeval tv;
    long remaining = (long)(deadline - time(NULL));
    if (remaining < 0)
      remaining = 0;
    tv.tv_sec = remaining;
    tv.tv_usec = 0;

    int r = select(srv + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0)
      break;

    int cfd = accept(srv, NULL, NULL);
    if (cfd < 0)
      continue;

    char *req = malloc(MAX_REQ);
    if (!req) {
      close(cfd);
      continue;
    }
    int total = read_request(cfd, req, MAX_REQ);
    if (total == 0) {
      free(req);
      close(cfd);
      continue;
    }

    char method[8] = {0};
    char path[512] = {0};
    sscanf(req, "%7s %511s", method, path);

    char reqpath[512], query[1024];
    query[0] = 0;
    snprintf(reqpath, sizeof(reqpath), "%s", path);
    char *qm = strchr(reqpath, '?');
    if (qm) {
      *qm = 0;
      snprintf(query, sizeof(query), "%s", qm + 1);
    }

    int was_boot = 0;

    if (!strcmp(method, "GET") &&
        (!strcmp(reqpath, "/") || !strcmp(reqpath, "/index"))) {
      render_page(cfd, s, s->upload_dir);
      close(cfd);
    } else if (!strcmp(method, "GET") && !strcmp(reqpath, "/files")) {
      char dec[1024] = {0};
      form_get(query, "dir", dec, sizeof(dec));
      render_page(cfd, s, dec[0] ? dec : s->upload_dir);
      close(cfd);
    } else if (!strcmp(method, "POST") && !strcmp(reqpath, "/boot")) {
      char clen_str[32] = {0};
      int clen = 0;
      if (header_get(req, "Content-Length", clen_str, sizeof(clen_str)))
        clen = atoi(clen_str);
      read_body(cfd, req, total, clen);
      char *body = strstr(req, "\r\n\r\n");
      body = body ? body + 4 : req + total;

      char *paths_buf = calloc(1, WEBUI_MAX_PATHS * WEBUI_PATH_LEN + 64);
      char *ovr_buf = calloc(
          1, WEBUI_MAX_OVERRIDES * (WEBUI_KEY_LEN + WEBUI_VAL_LEN + 4) + 64);
      char val[WEBUI_VAL_LEN];
      if (paths_buf && form_get(body, "paths", paths_buf,
                                WEBUI_MAX_PATHS * WEBUI_PATH_LEN + 64))
        parse_paths(s, paths_buf);
      if (ovr_buf && form_get(body, "overrides", ovr_buf,
                              WEBUI_MAX_OVERRIDES *
                                      (WEBUI_KEY_LEN + WEBUI_VAL_LEN + 4) +
                                  64))
        parse_overrides(s, ovr_buf);
      free(paths_buf);
      free(ovr_buf);

      if (form_get(body, "image", val, sizeof(val)))
        snprintf(s->image, sizeof(s->image), "%s", val);
      if (form_get(body, "target", val, sizeof(val)))
        snprintf(s->target, sizeof(s->target), "%s", val);
      if (form_get(body, "size_gb", val, sizeof(val)))
        s->size_gb = atoi(val);
      if (form_get(body, "vram_mb", val, sizeof(val)))
        s->vram_mb = atoi(val);
      if (form_get(body, "mode", val, sizeof(val)))
        s->mode = strcmp(val, "install") ? WEBUI_MODE_BOOT : WEBUI_MODE_INSTALL;

      s->submitted = 1;
      result = 0;
      was_boot = 1;
      send_plain(cfd, 200,
                 "<!doctype html><html><body style='background:#0b0e14;"
                 "color:#c9d1d9;font:14px system-ui;padding:24px'>"
                 "Starting&hellip; you can close this tab.</body></html>");
      close(cfd);
    } else if (!strcmp(method, "POST") && !strcmp(reqpath, "/skip")) {
      s->submitted = 0;
      result = -1;
      send_redirect(cfd);
      close(cfd);
    } else if (!strcmp(method, "POST") && !strcmp(reqpath, "/delete")) {
      char clen_str[32] = {0};
      int clen = 0;
      if (header_get(req, "Content-Length", clen_str, sizeof(clen_str)))
        clen = atoi(clen_str);
      read_body(cfd, req, total, clen);
      char *body = strstr(req, "\r\n\r\n");
      body = body ? body + 4 : req + total;
      char dir[1024] = {0}, name[512] = {0};
      form_get(body, "dir", dir, sizeof(dir));
      form_get(body, "name", name, sizeof(name));
      if (dir[0] && name[0]) {
        char full[1600];
        snprintf(full, sizeof(full), "%s%s%s", dir,
                 (dir[strlen(dir) - 1] == '/') ? "" : "/", name);
        if (unlink(full) == 0)
          printf("[webui] deleted %s\n", full);
      }
      send_redirect(cfd);
      close(cfd);
    } else if (!strcmp(method, "POST") && !strcmp(reqpath, "/upload")) {
      char clen_str[32] = {0};
      int clen = 0;
      if (header_get(req, "Content-Length", clen_str, sizeof(clen_str)))
        clen = atoi(clen_str);
      char dir[1024];
      snprintf(dir, sizeof(dir), "%s", s->upload_dir);
      form_get(query, "dir", dir, sizeof(dir));
      char *hdrend = strstr(req, "\r\n\r\n");
      int body_buffered = 0;
      if (hdrend)
        body_buffered = total - (int)(hdrend + 4 - req);
      int rc = -1;
      if (hdrend)
        rc = handle_upload(cfd, req, hdrend, clen, body_buffered, dir);
      if (rc == 0)
        send_redirect(cfd);
      else
        send_plain(cfd, 500, "Upload failed.");
      close(cfd);
    } else if (!strcmp(method, "POST") && !strcmp(reqpath, "/download")) {
      char clen_str[32] = {0};
      int clen = 0;
      if (header_get(req, "Content-Length", clen_str, sizeof(clen_str)))
        clen = atoi(clen_str);
      read_body(cfd, req, total, clen);
      char *body = strstr(req, "\r\n\r\n");
      body = body ? body + 4 : req + total;
      char url[1024] = {0}, dir[1024] = {0};
      form_get(body, "url", url, sizeof(url));
      form_get(body, "dir", dir, sizeof(dir));
      if (!dir[0])
        snprintf(dir, sizeof(dir), "%s", s->upload_dir);
      url_decode(url);
      url_decode(dir);
      if (url[0]) {
        char out[2048];
        const char *fn = strrchr(url, '/');
        fn = fn ? fn + 1 : "downloaded.img.xz";
        snprintf(out, sizeof(out), "%s%s%s", dir,
                 (dir[strlen(dir)-1] == '/') ? "" : "/", fn);
        notify("Downloading %s -> %s\n", url, out);
        if (http_download(url, out) == 0)
          send_plain(cfd, 200, "Download complete. You can close this tab.");
        else
          send_plain(cfd, 500, "Download failed.");
      } else {
        send_plain(cfd, 400, "Missing url.");
      }
      close(cfd);
    } else {
      send_plain(cfd, 404, "Not found.");
      close(cfd);
    }

    free(req);
    if (was_boot)
      break;
  }

  close(srv);
  return result;
}
