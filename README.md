# ps5-linux-loader (fork)

**ps5-linux-loader** boots Linux on a jailbroken PS5 (rest-mode resume) by defeating the hypervisor. Powered by 8 CPU cores (16 threads) at 3.5 GHz and a GPU at 2.23 GHz, it turns your PS5 Phat, Slim or Pro running **3.00–13.60** into a capable desktop PC for Steam games and emulators.

This repository is **TheRealRexo's fork** of the upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader). It adds a built-in WebUI + anonymous FTP server, M.2 install tools that store the OS as a single `.img` file, USB-less boot, and an in-payload "Download here" button to fetch official OS images straight from the R2 mirror.

## Features

- HDMI 4K60 video and audio output
- M.2 SSD as dedicated Linux partition
- All USB ports usable for peripherals
- BD drive usable via custom ahci driver
- Internal Bluetooth usable via custom xhci driver
- Ethernet port usable via custom gbe driver
- Hardware video decode, encode and transcode (VCN) via VAAPI

## What's new in this fork

- **Multi-firmware support**: 3.00–13.60 (Phat/Slim) and 13.60 (Pro), auto-selected from the running firmware
- **Built-in WebUI** on port 8080 (no auto-boot) + anonymous FTP on port 21
- **OS-image picker**, boot/install target selector, install-size field, VRAM setting
- **File manager** (list / upload / delete), including `/mnt/ext1` for USB-free setups
- **In-payload "Download here"**: fetch official OS images from the R2 mirror straight onto the console
- **Hardware / driver support table** (GPU, WiFi/Bluetooth, Ethernet, audio, NVMe, USB)
- **M.2 install tools** (`m2_init`, `ps5-osinstall`, `ps5-osexec`) storing the OS as a single `.img`
- **Loop-mounted `.img` boot**: patched initrd re-scans the NVMe GPT (BLKRRPART), searches M.2 → USB, regenerates `root=LABEL=...` automatically

## PS5 firmware

| Model | Supported firmwares | M.2 |
|-------|---------------------|-----|
| PS5 Phat & Slim | 3.00, 3.10, 3.20, 3.21 (no M.2) · 4.00–7.61 · 11.60 · 13.00, 13.20, 13.60 | Yes (except 3.xx) (11.60 untested) |
| PS5 Pro | 13.60 | Yes |

Support for 1.xx / 2.xx may come later. You cannot downgrade firmware.

## USB-less boot

The payload downloads the OS image directly from the R2 mirror (in-payload "Download here"), derives `cmdline.txt` (root label) and `vram.txt` from the selected `.img`, and the patched initrd loop-mounts the `.img` from the M.2. No USB stick is required: the PS5 kernel ships the NVMe driver and shifted-GPT support, and `ps5-osinstall` (baked into every wrapped image) handles installation.

## Hardware

Required: USB drive ≥64 GB (external SSD preferred) + USB keyboard/mouse (dongles supported).
Optional: USB WLAN adapter, M.2 SSD compatible with PS5, Bluetooth dongle for DualSense.

## Configure PS5 settings

- **VERY IMPORTANT**: Rest Mode → System → Power Saving → Features Available in Rest Mode → Supply Power to USB Ports = Always.
- **VERY IMPORTANT**: Settings → HDMI → Enable HDMI Device Link = off.
- *Recommended*: Disable automatic updates / error reporting.

## Installation

1. Get a Linux image (pre-built from [ps5-linux-image](https://github.com/ps5-linux/ps5-linux-image), or build via `build_image.sh --distro ubuntu2604`).
2. Flash to USB (min 64 GB, external SSD recommended):
   ```
   sudo dd if=output/ps5-ubuntu2604.img of=/dev/sdX bs=4M status=progress conv=fsync
   ```
3. Plug USB into front bottom Type-C or rear Type-A (avoid front top Type-A, USB 2.0).
4. Run the jailbreak and send payload:
   ```
   socat -t 99999999 - TCP:$PS5IP:9021 < ps5-linux-loader.elf
   ```
5. Wait for orange LED → press power → boot Linux (white LED = success).

If black screen: remove `video=DP-1:1920x1080@60` from cmdline.txt, toggle HDCP, or add `amdgpu.force_1080p=1`.

### M.2 install (this fork)

Use `m2_init` + `ps5-osinstall` to store the OS as a `.img` on the internal NVMe data partition; initramfs loop-mounts it.

## First boot

Disable screen saver (buggy); hold kernel packages (`apt-mark hold`); install Firefox; update mesa; clone [ps5-linux-tools](https://github.com/ps5-linux/ps5-linux-tools); install WLAN driver for Marvell chips.

## Releases

Each tagged release ships:

- `ps5-linux-loader.elf` + `m2_init` / `ps5-osinstall` / `ps5-osexec` (with checksums).
- Wrapped OS images: `ps5-<distro>-os.img.xz` (ubuntu2604, arch, cachyos, fedora, proxmox, debian, bazzite, bazzite-deck, steamos).
- Boot tarballs: `ps5-<distro>-boot.tar.gz` (bzImage, initrd.img, cmdline.txt, vram.txt; initrd patched to loop-mount the `.img`).
- `images.json` — manifest of latest `.img` download URLs (R2 mirror).

Images >2 GB ship their boot tarballs, checksums (.sha256) and descriptors (.osimage) here; their `.xz` is not attached (too big for a release asset). Download the `.img.xz` from the R2 mirror URL in `images.json`.

## Building from source

```
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk
export LLVM_CONFIG=/opt/homebrew/opt/llvm/bin/llvm-config
make
```

Writes `bin/ps5-linux-loader.elf`.

## Repository layout

```
source/  ... webui, ftp, httpdl, hv_defeat_*, loader, ...
include/ ...
shellcode_*/  ... firmware-specific HV kernel shellcode
tools/      m2_init, ps5-osinstall, ps5-osexec, inject/patch scripts, images.json
initrd/     patched init (loop-mount + BLKRRPART rescan)
.github/    CI build + release
```

## FAQ

- Q: Will higher >=14.00 firmwares be supported? A: No.
- Q: Why can I not use M.2 on 3.xx? A: Because the PS5 fails to boot with it attached.
- Q: Can I dual-boot Linux and PS5 OS? A: No, soft-mod; re-run exploit to boot into Linux.
- Q: Can I put Linux into standby/resume? A: No.
- Q: Can I continue using my PS5 if I install Linux? A: Yes, internal SSD is not modified.
- Q: Can I use the PS5's NIC/WLAN in Linux? A: Yes for Marvell WLAN (Phat/Slim) and MT7925 (Pro); Ethernet on all models.
- Q: Hardware decode/transcode? A: Yes (#38). Decode: H.264, HEVC, VP9, VC-1, MPEG-2. Encode: H.264, HEVC I-frames.
- Q: DualSense? A: Yes, via internal BT or dongle.
- Q: Resolutions? A: 1080p/1440p/2160p @60Hz; 1440p@120Hz confirmed on DELL S3225QC.
- Q: After reboot "Repairing"/"wasn't turned off properly"? A: Yes, harmless.

## Tips and tricks

- `RADV_DEBUG=nohiz` for graphical issues.
- Adjust cmdline in `cmdline.txt`; VRAM in `vram.txt` (default 512 MB = `0x20000000`).
- Black screen if `video=DP-1:` set; confirmed working without it on many monitors.

## Bugs

- Screen saver; HDMI audio on some monitors; HDMI 1440p/2160p on some monitors.
- VCN HEVC encode corrupt chroma with P-frames; use I-frame only.

## Credits

Upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) contributors: theflow, c0w, resulknad, rmuxnet, fail0verflow, flatz, cragson, john-tornblom, echostretch, kirathenotebook, 15432, DeathlySin, gzimbric. Related: [Byepervisor-Reborn](https://github.com/PS5Dev/Byepervisor-Reborn).

## License

See `LICENSE`.
