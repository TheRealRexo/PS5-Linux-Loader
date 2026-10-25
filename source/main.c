#include "ftp.h"
#include "hv_defeat_13xx.h"
#include "loader.h"
#include "prepare_resume.h"
#include "utils.h"
#include "webui.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>


static void ensure_ext1_mounted(void) {
  mkdir("/mnt/ext1", 0777);
  /* If /mnt/ext1 is already a mountpoint (OrbisOS exposed the M.2), done. */
  struct stat a, b;
  if (stat("/mnt/ext1", &a) == 0 && stat("/mnt", &b) == 0 &&
      a.st_dev != b.st_dev) {
    notify("M.2 storage already mounted at /mnt/ext1.\n");
    return;
  }
  /* Best-effort: try common M.2 device nodes / filesystems. */
  const char *devs[] = {"/dev/nda0", "/dev/nda0p1", "/dev/nvme0",
                        "/dev/nvme0n1", "/dev/ext1", "/dev/da0",
                        "/dev/da0s1", NULL};
  const char *fstypes[] = {"exfat", "msdosfs", "ufs", "ext2fs", NULL};
  for (int i = 0; devs[i]; i++) {
    for (int j = 0; fstypes[j]; j++) {
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "mount -t %s %s /mnt/ext1 2>/dev/null",
               fstypes[j], devs[i]);
      if (system(cmd) == 0) {
        notify("Mounted %s (%s) at /mnt/ext1.\n", devs[i], fstypes[j]);
        return;
      }
    }
  }
  notify("M.2 storage not auto-mounted at /mnt/ext1; "
         "upload to a USB path or mount it manually.\n");
}

int main(void) {
  if (setup_env()) {
    notify("Something went wrong while initiating.\n"
           "Please make sure your fw is 13.00, 13.20 or 13.60.");
    return -1;
  }

  ensure_ext1_mounted();

  if (fw != 0x1300 && fw != 0x1320 && fw != 0x1360) {
    notify("Unsupported firmware.\n"
           "This build supports 13.00, 13.20 and 13.60.");
    return -1;
  }

  /* WebUI gate: serve until the user explicitly submits. There is no
   * auto-boot, so the page stays open while the user transfers the .img
   * (via FTP on port 21) and configures the boot/install options. The
   * loader then derives cmdline (root label) and VRAM from the .img. */
  {
    struct webui_settings ws;
    webui_settings_init(&ws);

    /* Start the FTP server (anonymous) rooted at the upload dir so large
     * *.img files can be copied in while the WebUI serves HTTP. */
    if (ftp_server_start(ws.upload_dir, 21) > 0)
      notify("FTP server started on port 21 (root %s).", ws.upload_dir);

    notify("WebUI on port 8080. Configure and click Boot Linux "
           "(no auto-boot; transfer .img via FTP port 21 if needed)...");

    for (;;) {
      if (webui_run(&ws, 8080, 300) == 0 && ws.submitted) {
        loader_set_paths_from_webui(&ws);
        loader_set_overrides_from_webui(&ws);
        loader_set_boot_from_webui(&ws);
        notify("WebUI settings applied. Continuing boot...\n");
        break;
      }
      notify("WebUI awaits your selection (no auto-boot). "
             "Configure and click Boot Linux.\n");
      sleep(2);
    }
  }

  if (fetch_linux(&linux_i)) {
    notify("Something went wrong while installing linux files.\n");
    return -1;
  }

  void *shellcode_kernel;
  size_t shellcode_kernel_len;
  if (prepare_resume(&shellcode_kernel, &shellcode_kernel_len)) {
    notify("Something went wrong while preparing resume.\n");
    return -1;
  }

  if (hv_defeat_13xx(shellcode_kernel, shellcode_kernel_len))
    goto err;

  notify("Finished preparation. Going to rest mode in 5 seconds.\n"
         "Please wait for the orange light to stop blinking and then "
         "wake up to Linux :)\n");

  sleep(5);
  enter_rest_mode();

  while (1) {
    sleep(30);
  }

  return 0;

err:
  notify("Something went wrong while defeating Hypervisor.\n"
         "Please make sure your fw is 13.00, 13.20 or 13.60.");
  return -1;
}
