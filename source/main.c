#include "hv_defeat_13xx.h"
#include "loader.h"
#include "prepare_resume.h"
#include "utils.h"
#include <unistd.h>

int main(void) {
  if (setup_env()) {
    notify("Something went wrong while initiating.\n"
           "Please make sure your fw is 13.60.");
    return -1;
  }

  if (fw != 0x1360) {
    notify("Unsupported firmware.\nThis build only supports 13.60.");
    return -1;
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
         "Please make sure your fw is 13.60.");
  return -1;
}
