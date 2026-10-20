#include "loader.h"
#include "config.h"
#include "firmware.h"
#include "linux.h"
#include "utils.h"
#include "webui.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>

const char *file_paths[] = {
    "/mnt/usb0/",           "/mnt/usb1/",           "/mnt/usb2/",
    "/mnt/usb3/",           "/mnt/ext1/",           "/mnt/usb0/PS5/Linux/",
    "/mnt/usb1/PS5/Linux/", "/mnt/usb2/PS5/Linux/", "/mnt/usb3/PS5/Linux/",
};

/* Runtime search paths / overrides, optionally set by the WebUI before boot. */
static const char **active_paths = file_paths;
static int active_num_paths = (int)(sizeof(file_paths) / sizeof(file_paths[0]));

/* Packed "key\0val\0key\0val\0" pairs from the WebUI, or NULL. */
static char *webui_overrides = NULL;
static char *webui_overrides_end = NULL;

/* Boot/install selection set by the WebUI (passed to the kernel cmdline). */
static char webui_image[WEBUI_VAL_LEN] = {0};
static char webui_target[64] = {0};
static int webui_size_gb = 0;
static int webui_mode = WEBUI_MODE_BOOT;
static int webui_vram_mb = 0;


/* Read the ext4 volume label from a .img file (superblock at offset 1024). */
static void read_ext4_label(const char *imgpath, char *label, size_t labsize) {
  label[0] = 0;
  if (!imgpath || !imgpath[0]) return;
  int fd = open(imgpath, O_RDONLY);
  if (fd < 0) return;
  /* ext4 superblock starts at byte 1024 */
  if (lseek(fd, 1024, SEEK_SET) < 0) { close(fd); return; }
  unsigned char sb[1024];
  if (read(fd, sb, sizeof(sb)) != (ssize_t)sizeof(sb)) { close(fd); return; }
  close(fd);
  /* s_volume_name is at offset 0x38 (56) within the superblock, 16 bytes */
  size_t off = 0x38;
  size_t n = 0;
  while (n < 16 && n < labsize - 1 && sb[off + n] != 0) {
    label[n] = (char)sb[off + n];
    n++;
  }
  label[n] = 0;
}

long find_and_get_size_of_file(const char *filename, char *found_path);
int find_and_read_file(const char *filename, void *buf, size_t bufsize);

void loader_set_paths_from_webui(const struct webui_settings *s) {
  if (!s || s->num_paths <= 0)
    return;
  /* s->paths is an array of char[]; build a pointer table into it. */
  static const char *ptrs[WEBUI_MAX_PATHS];
  int n = s->num_paths;
  if (n > WEBUI_MAX_PATHS)
    n = WEBUI_MAX_PATHS;
  for (int i = 0; i < n; i++)
    ptrs[i] = s->paths[i];
  active_paths = ptrs;
  active_num_paths = n;
}

void loader_set_overrides_from_webui(const struct webui_settings *s) {
  free(webui_overrides);
  webui_overrides = NULL;
  webui_overrides_end = NULL;
  if (!s || s->num_overrides <= 0)
    return;

  /* Compute packed size: for each pair, key + NUL + val + NUL. */
  size_t need = 0;
  for (int i = 0; i < s->num_overrides; i++)
    need += strlen(s->override_keys[i]) + 1 + strlen(s->override_vals[i]) + 1;
  need += 1;

  char *buf = malloc(need);
  if (!buf)
    return;
  char *p = buf;
  for (int i = 0; i < s->num_overrides; i++) {
    size_t k = strlen(s->override_keys[i]) + 1;
    size_t v = strlen(s->override_vals[i]) + 1;
    memcpy(p, s->override_keys[i], k);
    p += k;
    memcpy(p, s->override_vals[i], v);
    p += v;
  }
  *p = 0;
  webui_overrides = buf;
  webui_overrides_end = buf + need;
}

void loader_set_boot_from_webui(const struct webui_settings *s) {
  webui_image[0] = 0;
  webui_target[0] = 0;
  webui_size_gb = 0;
  webui_mode = WEBUI_MODE_BOOT;
  webui_vram_mb = 0;
  if (!s)
    return;
  snprintf(webui_image, sizeof(webui_image), "%s", s->image);
  snprintf(webui_target, sizeof(webui_target), "%s", s->target);
  webui_size_gb = s->size_gb;
  webui_mode = s->mode;
  webui_vram_mb = s->vram_mb;
}

/* Look up `filename` in a packed key\\0val\\0 list. Returns val or NULL. */
static const char *lookup_packed(const char *list, const char *list_end,
                                 const char *filename) {
  if (!list)
    return NULL;
  size_t needle_len = strlen(filename);
  for (const char *p = list; p < list_end;) {
    size_t key_len = strlen(p);
    if (key_len == needle_len && !strncmp(p, filename, needle_len))
      return p + key_len + 1;
    p += key_len + 1 + strlen(p + key_len + 1) + 1;
  }
  return NULL;
}

static const char *get_overridden_filename(const char *filename) {
  /* WebUI overrides take precedence. */
  const char *from_webui = lookup_packed(webui_overrides, webui_overrides_end,
                                         filename);
  if (from_webui)
    return from_webui;

  static int state = 0;
  static char *overrides_start = nullptr;
  static char *overrides_end = nullptr;

  if (state == 0) {
    state = 1;
    char found_path[256];
    ssize_t size = find_and_get_size_of_file("path-override.txt", found_path);
    if (size > 0) {
      overrides_start = malloc(size + 1);
      overrides_end = overrides_start + size + 1;
      if (read_file(found_path, overrides_start, size) == size) {
        state = 2;
        for (char *p = overrides_start; p < overrides_end; p++)
          if (*p == '\n')
            *p = 0;
        // make sure the last string is null-terminated
        overrides_start[size] = 0;
      }
    }
  }

  // overrides not found, or unreadable, or currently looking for it
  if (state == 1)
    return filename;

  size_t needle_len = strlen(filename);
  for (const char *p = overrides_start; p < overrides_end;) {
    size_t haystack_len = strlen(p);
    if (haystack_len > needle_len && !strncmp(p, filename, needle_len) &&
        p[needle_len] == '=')
      return p + needle_len + 1;
    p += haystack_len + 1;
  }

  // haven't found an override, return original filename
  return filename;
}

long find_and_get_size_of_file(const char *filename, char *found_path) {
  char full_path[256];
  struct stat st;

  filename = get_overridden_filename(filename);

  for (int i = 0; i < active_num_paths; i++) {
    snprintf(full_path, sizeof(full_path), "%s%s", active_paths[i], filename);

    if (stat(full_path, &st) == 0) {
      notify("File '%s' found in '%s'\n", filename, active_paths[i]);
      strcpy(found_path, full_path);
      return st.st_size;
    }
  }

  return -1;
}

int find_and_read_file(const char *filename, void *buf, size_t bufsize) {
  char full_path[256];
  struct stat st;

  for (int i = 0; i < active_num_paths; i++) {
    snprintf(full_path, sizeof(full_path), "%s%s", active_paths[i], filename);

    if (stat(full_path, &st) == 0) {
      notify("File '%s' found in '%s'\n", filename, active_paths[i]);
      return read_file(full_path, buf, bufsize);
    }
  }

  return -1;
}

int read_file(const char *path, void *buf, size_t bufsize) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return fd;
  int r = read(fd, buf, bufsize);
  close(fd);
  return r;
}

void trim_newline(char *s) {
  while (*s != '\0') {
    if (*s == '\r' || *s == '\n') {
      *s = '\0';
      break;
    }
    s++;
  }
}

int fetch_linux(struct linux_info *info) {
  char bzimage_path[256];
  char initrd_path[256];

  size_t bzimage_size = find_and_get_size_of_file("bzImage", bzimage_path);
  if (bzimage_size < 0) {
    notify("File bzImage not found at default paths - Aborting\n");
    return -1;
  }

  void *bzimage =
      mmap(NULL, ALIGN_UP(bzimage_size, PAGE_SIZE), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (bzimage == MAP_FAILED) {
    notify("[-] Error could not allocate bzimage.\n");
    return -1;
  }

  bzimage_size =
      read_file(bzimage_path, bzimage, ALIGN_UP(bzimage_size, PAGE_SIZE));
  if (bzimage_size < 0) {
    notify("Something went wrong while reading bzImage - Aborting\n");
    return -1;
  }

  size_t initrd_size = find_and_get_size_of_file("initrd.img", initrd_path);
  if (bzimage_size < 0) {
    notify("File bzImage not found at default paths - Aborting\n");
    return -1;
  }

  void *initrd =
      mmap(NULL, ALIGN_UP(initrd_size, PAGE_SIZE), PROT_READ | PROT_WRITE,
           MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (initrd == MAP_FAILED) {
    notify("[-] Error could not allocate initrd.\n");
    return -1;
  }

  initrd_size =
      read_file(initrd_path, initrd, ALIGN_UP(initrd_size, PAGE_SIZE));
  if (initrd_size < 0) {
    notify("Something went wrong while reading initrd - Aborting\n");
    return -1;
  }

  if (dump_device_firmwares(initrd_path) < 0) {
    notify(
        "Something went wrong while dumping device firmwares - Continuing\n");
  }

  size_t vram_size;
  if (webui_vram_mb > 0) {
    vram_size = (size_t)webui_vram_mb * 1024 * 1024;
    printf("Using WebUI VRAM setting: %d MiB\n", webui_vram_mb);
  } else {
    char buf_vram[16] = {};
    int ret = find_and_read_file("vram.txt", buf_vram, sizeof(buf_vram) - 1);
    if (ret < 0) {
      printf(
          "File vram.txt not found at default paths - Using static fallback\n");
      vram_size = VRAM_SIZE;
    } else {
      trim_newline(buf_vram);
      vram_size = strtoull(buf_vram, NULL, 16);
      if (vram_size == 0) {
        printf("Seems like the configured vram value is wrong - Using static "
               "fallback\n");
        vram_size = VRAM_SIZE;
      }
    }
  }

  char cmdline[2048] = {};
  int ret = find_and_read_file("cmdline.txt", cmdline, sizeof(cmdline) - 1);
  if (ret < 0) {
    printf("File cmdline.txt not found at default paths - Using static "
           "fallback\n");
    strcpy(cmdline, CMD_LINE);
  } else {
    trim_newline(cmdline);
  }

  /* Regenerate the root label from the selected image so loop-mount matches. */
  if (webui_image[0]) {
    char lbl[32] = {0};
    read_ext4_label(webui_image, lbl, sizeof(lbl));
    if (lbl[0]) {
      /* replace root=LABEL=... in cmdline */
      char *p = strstr(cmdline, "root=LABEL=");
      if (p) {
        char *end = p + strlen("root=LABEL=");
        char *space = strchr(end, ' ');
        char buf[2048];
        size_t pre = (size_t)(p - cmdline);
        snprintf(buf, sizeof(buf), "%.*sroot=LABEL=%s%s", (int)pre, cmdline, lbl,
                 space ? space : "");
        snprintf(cmdline, sizeof(cmdline), "%s", buf);
        printf("Regenerated root label from image: %s\n", lbl);
      }
    }
  }

  /* Append the WebUI boot/install selection (consumed by /init). */
  size_t cl = strlen(cmdline);
  if (webui_image[0] && cl + strlen(webui_image) + 32 < sizeof(cmdline)) {
    cl += (size_t)snprintf(cmdline + cl, sizeof(cmdline) - cl, " ps5.image=%s",
                           webui_image);
  }
  if (webui_target[0] && strcmp(webui_target, "none") &&
      cl + strlen(webui_target) + 32 < sizeof(cmdline)) {
    cl += (size_t)snprintf(cmdline + cl, sizeof(cmdline) - cl, " ps5.target=%s",
                           webui_target);
  }
  if (webui_size_gb > 0 && cl + 32 < sizeof(cmdline)) {
    cl += (size_t)snprintf(cmdline + cl, sizeof(cmdline) - cl,
                           " ps5.size_gb=%d", webui_size_gb);
  }
  if (webui_mode == WEBUI_MODE_INSTALL && cl + 32 < sizeof(cmdline)) {
    cl += (size_t)snprintf(cmdline + cl, sizeof(cmdline) - cl,
                           " ps5.mode=install");
  }

  info->linux_info = kernel_cave_linux_info;
  info->bzimage = kernel_cave_bzImage;
  info->bzimage_size = bzimage_size;
  info->initrd = kernel_cave_bzImage + ALIGN_UP(bzimage_size, PAGE_SIZE);
  info->initrd_size = initrd_size;
  info->vram_size = vram_size;
  info->kit_type = (int)get_kit_type();
  strcpy(info->cmdline, cmdline);

  uint64_t page = alloc_page();
  kwrite(pa_to_dmap(page), info, sizeof(struct linux_info));
  install_page_syscore(kernel_cave_linux_info, page, 0);

  for (int i = 0; i < bzimage_size; i += PAGE_SIZE) {
    install_page_syscore(info->bzimage + i,
                         vtophys_user((uintptr_t)bzimage + i), 0);
  }

  for (int i = 0; i < initrd_size; i += PAGE_SIZE) {
    install_page_syscore(info->initrd + i, vtophys_user((uintptr_t)initrd + i),
                         0);
  }

  return 0;
}
