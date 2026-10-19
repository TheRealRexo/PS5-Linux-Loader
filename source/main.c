#include "ftp.h"
#include "hv_defeat_13xx.h"
#include "loader.h"
#include "prepare_resume.h"
#include "utils.h"
#include "webui.h"
#include <unistd.h>

int main(void) {
  if (setup_env()) {
    notify("Something went wrong while initiating.\n"
           "Please make sure your fw is 13.00, 13.20 or 13.60.");
    return -1;
  }

  if (fw != 0x1300 && fw != 0x1320 && fw != 0x1360) {
    notify("Unsupported firmware.\n"
           "This build supports 13.00, 13.20 and 13.60.");
    return -1;
  }

  /* WebUI gate: let the user review detected files / adjust paths, but
   * never block forever. Falls back to defaults on timeout or error. */
  {
    struct webui_settings ws;
    webui_settings_init(&ws);

    /* Start the FTP server (anonymous) rooted at the upload dir so large
     * *.img files can be copied in while the WebUI serves HTTP. */
    if (ftp_server_start(ws.upload_dir, 21) > 0)
      notify("FTP server started on port 21 (root %s).", ws.upload_dir);

    notify("WebUI test build.\nServing config page on port 8080...");

    if (webui_run(&ws, 8080, 120) == 0 && ws.submitted) {
      loader_set_paths_from_webui(&ws);
      loader_set_overrides_from_webui(&ws);
      loader_set_boot_from_webui(&ws);
      notify("WebUI settings applied. Continuing boot...\n");
    } else {
      notify("WebUI timed out or skipped.\n"
             "Booting with default paths...\n");
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
