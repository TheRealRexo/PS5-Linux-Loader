# ps5-linux-loader (13.xx)

**ps5-linux** leverages patched HV vulnerabilities to transform your PS5 Phat, Slim and Pro console running 3.00-13.60 firmwares into a highly capable Linux PC, unlocking its full hardware potential for desktop use. Powered by 8 CPU cores (16 threads) at 3.5 GHz and a GPU at 2.23 GHz, it provides enough performance to run Steam games and various emulators with impressive fluidity.

This repository is a **firmware-13.xx fork** of the official [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader). It is based on the 13.60 Fix (#46) merge and re-adds the multi-firmware 13.xx path, so it supports **13.00, 13.20 and 13.60**. On top of the upstream payload it adds a built-in **WebUI + anonymous FTP server** for managing Linux images, a set of **M.2 install tools** that store the OS as a single `.img` file on the internal SSD, and **USB-less boot**.

**Credit / provenance.** The exploit, HV and Linux-boot logic is the upstream work of the ps5-linux authors (see Credits). This fork packages it, restores the 13.xx firmware path, and layers the WebUI/FTP/M.2 tooling on top.

## USB-less boot

The payload downloads the OS image directly from the R2 mirror (in-payload "Download here"), derives `cmdline.txt` (root label) and `vram.txt` from the selected `.img`, and the patched initrd loop-mounts the `.img` from the M.2. No USB stick is required: the PS5 kernel ships the NVMe driver and shifted-GPT support, and `ps5-osinstall` (baked into every wrapped image) handles installation.

## Features

- HDMI 4K60 video and audio output
- M.2 SSD as dedicated Linux partition
- All USB ports usable for peripherals
- BD drive usable via custom ahci driver
- Internal Bluetooth usable via custom xhci driver
- Ethernet port usable via custom gbe driver
- Hardware video decode, encode and transcode (VCN) via VAAPI

## PS5 firmware

ps5-linux is supported on the following firmwares:

**PS5 Phat and Slim:** 3.00, 3.10, 3.20, 3.21 (without M.2 support); 4.00, 4.02, 4.03, 4.50, 4.51; 5.00, 5.02, 5.10, 5.50; 6.00, 6.02, 6.50; 7.00, 7.01, 7.20, 7.40, 7.60, 7.61; 11.60 (regular PS5 only, M.2 not tested yet; the kernel needs KASLR, so do not pass `nokaslr`, `mem=` or `memmap=`); 13.00, 13.20, 13.60 with M.2 support.

**PS5 Pro:** 13.60 with M.2 support.

Support for 1.xx and 2.xx firmwares may be added in the future, but we will not prioritize this effort.

> This fork focuses on the 13.xx line. 13.60 is tested; 13.00/13.20 are untested.

## Hardware

Required: USB drive with minimum 64 GB (ideally external SSD) to install and run Linux; USB keyboard/mouse (dongles supported). Optional: USB WLAN adapter for WLAN internet access; M.2 SSD compatible on PS5 to run Linux from SSD; Bluetooth dongle to connect with PS5 DualSense controller.

## Configure PS5 settings

- VERY IMPORTANT: Enable Rest Mode features: Settings → System → Power Saving → Features Available in Rest Mode → Supply Power to USB Ports = Always.
- VERY IMPORTANT: Disable HDMI Device Link: Settings → HDMI → Enable HDMI Device Link.
- Recommended: Disable automatic updates: Settings → System Software → System Software Update and Settings.
- Recommended: Disable automatic error reporting: Settings → System Software → Report System Software Errors Automatically.
- If you reset your PS5 settings or reinstall the FW, reapply these.

## Installation

1. **Get a Linux image** — pre-built from [ps5-linux-image](https://github.com/ps5-linux/ps5-linux-image) (recommended `ps5-ubuntu2604.img.xz`), or build your own with `build_image.sh --distro ubuntu2604`.
2. **Flash the image to USB** (min 64 GB; external SSD recommended):
   ```
   sudo dd if=ps5-ubuntu2604.img of=/dev/sdX bs=4M status=progress conv=fsync
   ```
   Windows: use Balena Etcher.
3. **Plug the USB drive** into a supported port (front bottom Type-C or rear Type-A; avoid the front top Type-A USB 2.0 port).
4. **Run the jailbreak** per your firmware (umtx2 for 3.00-5.50, Y2JB for 6.00-7.61, WebKit Autoloader for 7.61-13.60).
5. **Send the payload:**
   ```
   socat -t 99999999 - TCP:$PS5IP:9021 < ps5-linux-loader.elf
   ```
   The console enters rest mode; wait for the orange LED to stop blinking, then press power to boot Linux (LED turns white on success).

If the LED is white but the screen is black: remove `video=DP-1:1920x1080@60` from `cmdline.txt`, toggle HDCP, try another display/capture device, or add `amdgpu.force_1080p=1` to `cmdline.txt`.

## First boot

Configure your system and memorize your login password. Then: disable screen saver (buggy); possibly re-enable Wired/WLAN; hold kernel packages (`sudo apt-mark hold linux-generic ...`); install Firefox (`sudo snap install firefox`); update mesa; clone [ps5-linux-tools](https://github.com/ps5-linux/ps5-linux-tools) and build. For Marvell WLAN chips, install [ps5-linux-mwifiex](https://github.com/ps5-linux/ps5-linux-mwifiex).

## M.2 installation

You can use an M.2 SSD exclusively for Linux. Attach it per the official guide, reformat on the PS5 if previously used for games, then from a running Linux:

```
cd ps5-linux-tools
sudo ./m2_init
# reboot, relaunch Linux, then:
sudo ./m2_install.sh --install $PATH_TO_YOUR_IMG
sudo ./m2_exec.sh
```

To always boot from M.2, edit the label in `/boot/efi/cmdline.txt` from `root=LABEL=ubuntu2604` to `root=LABEL=ubuntu2604-m2`.

**This fork's M.2 flow** keeps the OS as a single `.img` file inside a data partition (more resilient). Use `m2_init` + `ps5-osinstall` (see below) and the in-payload WebUI/FTP to go fully USB-less.

## M.2 install tools (this fork)

- `m2_init` — writes the hidden GPT (`--device`, `--size-gb`).
- `ps5-osinstall` — creates the data partition and copies the `.img` onto it (`--target m2`, `--img`, `--label`).
- `ps5-osexec` — kexec into a Linux root partition/label (raw-partition installs).

Loop-mounted `.img` boot: the initramfs `init` re-scans the NVMe GPT (BLKRRPART), searches M.2 first then USB, loop-mounts the first `*.img` it finds, and hands off to systemd. The payload regenerates `root=LABEL=...` from the selected image's ext4 label automatically.

⚠️ Coexistence caveat: the M.2 is treated as Linux-exclusive. A soft PS5 format usually leaves the Linux data partition (and the `.img`) intact, but a repair/hard format erases it. Back up your image.

## Fan & boost control

```
cd ps5-linux-tools
sudo ./ps5_control --fan on
sudo ./ps5_control --boost on
```

Always turn on fan when you turn on boost.

## Updating ps5-linux

Download `.deb`/`.pkg.tar.zst` from [ps5-linux-patches](https://github.com/ps5-linux/ps5-linux-patches) and install as normal packages.

## The WebUI

While the payload runs it serves a page on `http://$PS5IP:8080` and an anonymous FTP server on `ftp://$PS5IP:21`. Use the WebUI to:

- Review the auto-detected `bzImage` / `initrd.img` / `cmdline.txt`;
- Pick the OS image and the boot/install target, set the VRAM size (MiB, 0 = auto);
- Download official OS images directly from the R2 mirror (**Download here**);
- Upload or delete files in the boot directory (including `/mnt/ext1` for USB-free setups);
- View the Hardware / driver support table.

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

## FAQ

- **Will higher >=14.00 firmwares be supported?** No.
- **Why can I not use M.2 on 3.xx?** Because the PS5 fails to boot with it attached.
- **Can I dual-boot Linux and PS5 OS?** No, this is a soft-mod. You need to re-run the exploit to boot into Linux.
- **Can I put Linux into standby and resume?** No, not supported.
- **Can I continue using my PS5 if I install Linux?** Yes, the internal SSD is not modified.
- **Can I use the PS5's NIC/WLAN module in Linux?** WLAN supported for Marvell chipsets (Phat/Slim) and MediaTek MT7925 (Pro); Ethernet supported on all models.
- **Does hardware video decode and transcode work?** Yes. Decode: H.264, HEVC Main/Main 10, VP9 Profile 0/2, VC-1, MPEG-2. Encode: H.264, HEVC I-frames only.
- **Does the DualSense controller work?** Yes, via internal Bluetooth and Bluetooth dongle.
- **What resolutions and refresh rates are supported?** 1080p, 1440p, 2160p at 60 Hz broadly. 1440p@120 Hz confirmed on DELL S3225QC.
- **After reboot, I get a "Repairing" screen.** Yes, harmless.
- **Can I do this with just a PS5, no other devices?** Yes now — the payload can download the OS image directly from the R2 mirror and store it on the M.2.

## Tips and tricks

- If you see graphical issues, add `RADV_DEBUG=nohiz`.
- Adjust the kernel cmdline in `cmdline.txt` in the FAT32 partition.
- Adjust VRAM size in `vram.txt` (default 512 MB = `0x20000000`, enables Dynamic VRAM).
- Monitor hotswap may work but won't change resolution automatically.
- Some monitors have a black screen if `video=DP-1:` is set. Confirmed working without it: MSI MAG274Q QD E2, DELL S2721DGF, DELL U2515H (1440p@60 Hz); possibly LG 27GL850, Lenovo Legion Y27q, ViewSonic Elite XG270QG.

## Bugs

- Screen saver does not work properly.
- HDMI audio output does not work on some monitors.
- HDMI 1440p and 2160p video output does not work on some monitors.
- VCN HEVC encode has corrupt chroma with P-frames; use I-frame only (`-g 1 -bf 0`) or H.264.

## Credits

Upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) and its contributors, including:

- theflow: ps5-linux-loader, ps5-linux-patches, ps5-linux-tools
- c0w: ps5-linux-loader
- resulknad: ps5-linux-image
- rmuxnet: ps5 ethernet driver
- fail0verflow: prosperous
- flatz: HV exploit
- cragson: HV exploit implementation
- john-tornblom: PS5 SDK
- echostretch: Offsets and testing
- kirathenotebook: Betatesting and readme contribution
- 15432: Tests on BC-250
- DeathlySin: PS5 Pro support (E820 memory map, I/O fix)
- gzimbric: 11.60 support

Related work: [PS5Dev/Byepervisor-Reborn](https://github.com/PS5Dev/Byepervisor-Reborn) (13.xx rest-mode hypervisor exploit; bug found by theflow0). See the upstream repository for the complete contributor list.

## License

See `LICENSE`.
