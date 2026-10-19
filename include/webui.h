#ifndef WEBUI_H
#define WEBUI_H

#include <stddef.h>

#define WEBUI_MAX_PATHS 16
#define WEBUI_PATH_LEN 128
#define WEBUI_MAX_OVERRIDES 32
#define WEBUI_KEY_LEN 64
#define WEBUI_VAL_LEN 192

/* Selectable targets for install/boot (USB slots + internal M.2). */
#define WEBUI_MAX_TARGETS 8

enum webui_mode {
  WEBUI_MODE_BOOT = 0, /* just boot the selected image */
  WEBUI_MODE_INSTALL = 1, /* install the image onto the target, then boot */
};

struct webui_settings {
  int num_paths;
  char paths[WEBUI_MAX_PATHS][WEBUI_PATH_LEN];

  int num_overrides;
  char override_keys[WEBUI_MAX_OVERRIDES][WEBUI_KEY_LEN];
  char override_vals[WEBUI_MAX_OVERRIDES][WEBUI_VAL_LEN];

  /* --- OS image selection --- */
  char image[WEBUI_VAL_LEN]; /* full path to chosen *.img, "" = auto-detect */
  char target[64];            /* "usb0".."usb3" | "m2" | "" = none */
  int size_gb;                /* install size in GB, 0 = image's natural size */
  int mode;                   /* enum webui_mode */

  /* VRAM size in MiB for the booted Linux; 0 = use vram.txt / default. */
  int vram_mb;

  /* Directory for uploads / file-manager operations. */
  char upload_dir[WEBUI_PATH_LEN];

  int submitted; /* 1 if user confirmed via WebUI, 0 if timed out/skipped */
};

/* Populate settings with the loader's default paths (no overrides). */
void webui_settings_init(struct webui_settings *s);

/*
 * Serve a one-shot WebUI on the given port until the user submits or the
 * timeout (seconds) elapses. Never blocks forever; returns 0 on submit,
 * -1 on timeout or any network error (caller proceeds with defaults).
 */
int webui_run(struct webui_settings *s, int port, int timeout_sec);

#endif
