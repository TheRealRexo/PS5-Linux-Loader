# ps5-linux-loader (13.xx)

## USB-less boot

The payload downloads the OS image directly from the R2 mirror (in-payload "Download here"), derives `cmdline.txt` (root label) and `vram.txt` from the selected `.img`, and the patched initrd loop-mounts the `.img` from the M.2. No USB stick is required: the PS5 kernel ships the NVMe driver and shifted-GPT support, and `ps5-osinstall` (baked into every wrapped image) handles installation.

A **firmware-13.xx** fork of the official [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) that boots Linux on **rest-mode resume** by defeating the PS5 hypervisor.

This fork is based on the 13.60 Fix (#46) merge and re-adds the multi-firmware 13.xx path, so it supports **13.00, 13.20 and 13.60**. On top of the upstream payload it adds a built-in **WebUI + anonymous FTP server** for managing Linux images, and a set of **M.2 install tools** that store the OS as a single `.img` file on the internal SSD.

**Credit / provenance.** The exploit, HV and Linux-boot logic is the upstream work of the ps5-linux authors (see Credits). This fork packages it, restores the 13.xx firmware path, and layers the WebUI/FTP/M.2 tooling on top.

## Firmware

| Firmware | Status | Notes |
|---|---|---|
| 13.60 | ✅ Tested | Developed and verified on hardware (PS5 Phat & Slim). |
| 13.20 | ⚠️ Untested | Offsets + 13xx shellcode restored from upstream. |
| 13.00 | ⚠️ Untested | Offsets + 13xx shellcode restored from upstream. |

Any other firmware is refused by the payload. The exploit used here is the same rest-mode `startup_mp` bug that Byepervisor-Reborn targets; 13.00/13.20 are provided for parity with it but have not been validated on real hardware by this fork.

## What's new in this fork

- **Multi-firmware 13.xx support** — 13.00 / 13.20 / 13.60, selected automatically from the running firmware. The 13xx kernel shellcode is committed (`source/shellcode_kernel_13xx.c`), so a plain `make` still produces the payload — no sub-builds.
- **Built-in WebUI** (served on port 8080 while the payload runs, no auto-boot):
  - Detects boot files (`bzImage`, `initrd.img`, `cmdline.txt`) across USB search paths (and `/mnt/ext1`).
  - OS-image picker, boot/install target selector and install-size field.
  - VRAM setting (MiB, 0 = auto from `vram.txt`).
  - File manager (list / upload / delete) for the boot directory, including `/mnt/ext1` for USB-free setups.
  - In-payload **Download here** button to fetch official OS images from the R2 mirror directly onto the console.
  - Hardware / driver support table (GPU, WiFi/Bluetooth chipset, Ethernet, audio, NVMe, USB) with chipset names and driver status, including whether the PS5 WiFi firmware has been dumped.
- **Anonymous FTP server** (port 21, rooted at the upload dir) so multi-GB `*.img` files can be copied to the console without the WebUI's HTTP upload limits.
- **M.2 install tools** (`tools/`) that place the OS as a `.img` file on the internal NVMe SSD:
  - `m2_init` — writes the hidden GPT (`--device`, `--size-gb`).
  - `ps5-osinstall` — creates the data partition and copies the `.img` onto it (`--target m2`, `--img`, `--label`).
  - `ps5-osexec` — kexec into a Linux root partition/label (raw-partition installs).
- **Loop-mounted `.img` boot** — the initramfs `init` re-scans the NVMe GPT (BLKRRPART) so the hidden PS5 partitions appear, searches the M.2 (NVMe) first, then USB, loop-mounts the first `*.img` it finds and hands off to systemd. The payload also regenerates `root=LABEL=...` from the selected image's ext4 label automatically.

## Requirements

- Required: USB keyboard (dongles supported).
- Boot files (`bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt`) and the Linux `.img` can live on a USB drive or on the internal M.2 SSD (`/mnt/ext1`). A USB drive is only needed if you are not using the M.2 flow.
- Optional: USB WLAN adapter (Marvell chipsets only), USB mouse, Bluetooth dongle for DualSense, internal M.2 SSD for a dedicated Linux `.img`.

## Configure PS5 settings

- Very important: enable Rest Mode power — Settings → System → Power Saving → Features Available in Rest Mode → Supply Power to USB Ports = Always.
- Very important: disable HDMI Device Link — Settings → HDMI → Enable HDMI Device Link = off.
- Recommended: disable automatic updates and automatic error reporting.
- If you reset your PS5 settings or reinstall the firmware, reapply these.

## Quick start

### Option A — boot from a USB image

1. Get a Linux image from [ps5-linux-image](https://github.com/ps5-linux/ps5-linux-image) and unpack it.
2. Flash it to USB (min 16 GB; external SSD recommended):
   ```
   sudo dd if=ps5-ubuntu2604.img of=/dev/sdX bs=4M status=progress conv=fsync
   ```
3. Plug the USB into a supported port (front bottom Type-C or rear Type-A).
4. Run your 13.00 / 13.20 / 13.60 kernel exploit to reach a payload loader, then send the payload:
   ```
   socat -t 99999999 - TCP:$PS5IP:9021 < ps5-linux-loader.elf
   ```
5. The console enters rest mode. Wait for the orange LED to stop blinking and turn static, then press power to boot Linux (LED turns white on success).

### Option B — install the OS as a `.img` on the internal M.2

The M.2 flow keeps the OS as a single file inside a data partition, which is more resilient to the PS5 touching the drive than a raw root partition. Format the M.2 in the PS5 first, then from a running Linux:

```
# write the hidden GPT (adjust size as needed)
sudo ./m2_init --device /dev/nvme0n1 --size-gb 128

# create the data partition and copy the image onto it as a file
sudo PATH=$PWD:$PATH ./ps5-osinstall --img /mnt/usb/ps5-arch.img \
     --target m2 --device /dev/nvme0n1 --size-gb 128 --label PS5M2 --yes
```

`ps5-osinstall` writes a `ps5-os-image` marker describing the install. On boot the initramfs finds the `.img` on the M.2, loop-mounts it, and systemd resumes onto the image's own root filesystem.

**USB-free boot:** copy the boot files (`bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt`) plus your `.img` onto the M.2 (`/mnt/ext1`) via the WebUI / FTP. The payload adds `/mnt/ext1` to its search paths and tries to mount the M.2 automatically, so no USB is required after that. You can also use the in-payload **Download here** button to fetch an official image straight from the R2 mirror.

⚠️ **Coexistence caveat.** The official guide treats the M.2 as Linux-exclusive. A PS5 soft format is expected to leave the Linux data partition (and therefore the `.img`) intact, but a repair / hard format will erase it. Back up your image.

## The WebUI

While the payload runs it serves a page on `http://$PS5IP:8080` and an anonymous FTP server on `ftp://$PS5IP:21`. Use the WebUI to:

- Review the auto-detected `bzImage` / `initrd.img` / `cmdline.txt`;
- Pick the OS image and the boot/install target, and set the VRAM size (MiB, 0 = auto);
- Download official OS images directly from the R2 mirror (**Download here**);
- Upload or delete files in the boot directory (including to `/mnt/ext1` for USB-free setups);
- View the Hardware / driver support table (GPU, WiFi/Bluetooth chipset, Ethernet, audio, NVMe, USB) with chipset names and driver status, including whether the PS5 WiFi firmware has been dumped.

There is **no auto-boot**: the page stays open while you configure and transfer files; click **Boot Linux** when ready. Large images can be pushed over FTP into the same directory.

## Releases

Each tagged release ships:

- `ps5-linux-loader.elf` + `m2_init` / `ps5-osinstall` / `ps5-osexec` (with checksums).
- Wrapped OS images `ps5-<distro>-os.img.xz` (official ps5-linux images with the M.2/storage tooling baked into the root filesystem) for `ubuntu2604, arch, cachyos, fedora, proxmox, debian, bazzite, bazzite-deck, steamos`.
- `ps5-<distro>-boot.tar.gz` containing `bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt` for USB-free / manual setups.
- `images.json` — manifest of the latest accessible `.img` download URLs (R2 mirror).

Images larger than GitHub's 2 GB per-asset limit ship only their checksums, descriptors and boot tarballs here; the `.xz` is available via the R2 mirror listed in `images.json`.

## Building from source

Install the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), then:

```
git clone https://github.com/TheRealRexo/PS5-Linux-Loader
cd PS5-Linux-Loader
make
```

The payload is written to `bin/ps5-linux-loader.elf`. A plain `make` is sufficient — the 13xx kernel shellcode is committed, so there are no sub-builds.

Set `PS5_PAYLOAD_SDK` (and `LLVM_CONFIG` if needed) before building:

```
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
make
```

## Repository layout

```
source/         payload sources (main, hv_defeat_13xx, prepare_resume, offsets, webui, ftp, httpdl, ...)
include/        headers
shellcode_1360.c        committed 13.60 kernel + AP shellcode
shellcode_kernel_13xx.c committed 13.00/13.20 kernel shellcode
tools/          m2_init, ps5-osinstall, ps5-osexec, inject-os-image.sh, patch-initrd.sh, images.json
initrd/         patched init (loop-mount + BLKRRPART rescan)
.github/        CI that builds the payload and wraps OS images into a release
```

## Q&A

- **Q: Which firmwares work?** A: 13.00, 13.20, 13.60 (13.60 tested; 13.00/13.20 untested).
- **Q: Can I dual-boot Linux and the PS5 OS?** A: Practically yes, but technically you always have to boot the PS5 OS first to run the payload if you want to run Linux.
- **Q: Can I keep using my PS5 normally?** A: Yes, this doesn't modify your PS5 OS in any way.
- **Q: After reboot I get a "Repairing Console Storage" / "Your PS5 wasn't turned off properly" screen.** A: Expected, and harmless.
- **Q: LED is white but the screen is black.** A: Try removing `video=DP-1:1920x1080@60` from `cmdline.txt`, toggle HDCP, try another display/capture device, or add `amdgpu.force_1080p=1`.
- **Q: Can I do this with just a PS5, no other devices?** A: Yes now — the payload can download the OS image directly from the R2 mirror and store it on the M.2, so no USB is needed once the M.2 flow is set up.
- **Q: Why do I need a keyboard, why not just let me use my controller?** A: Reason 1 is because bluetooth drivers could potentially not work on your console and Reason 2 being that there's at least 2 bluetooth signals in your house and there's no way for Linux to tell which one specifically is the controller you're holding unless you connect it manually WHICH NEEDS A KEYBOARD TO DO.
- **Q: Bluetooth/Wifi doesn't work on my ps5 chipset, what do I do?** A: Buy a USB 3 Wifi and/or Bluetooth dongle (yes the dongle can do both if you get a good one).
- **Q: Does this work on the PS5 Pro and PS5 slim?** A: It SHOULD work BUT some drivers like Bluetooth or wifi will probably not work.

## Credits

Upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) and its contributors, including:

- theflow: ps5-linux-loader, ps5-linux-patches, ps5-linux-tools
- c0w: ps5-linux-loader
- resulknad: ps5-linux-image
- rmuxnet: ps5 ethernet driver
- Ouroboroshen: 13.60 Fix (#46)
- fail0verflow: prosperous
- flatz: HV exploit
- cragson: HV exploit implementation
- john-tornblom: PS5 SDK
- echostretch: Offsets and testing
- kirathenotebook: Betatesting and readme contribution
- 15432: Tests on BC-250

Related work: [PS5Dev/Byepervisor-Reborn](https://github.com/PS5Dev/Byepervisor-Reborn) (13.xx rest-mode hypervisor exploit; bug found by theflow0). See the upstream repository for the complete contributor list.

## License

See `LICENSE`.
