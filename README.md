# ps5-linux-loader (fork)

**ps5-linux-loader** boots Linux on a jailbroken PS5 (rest-mode resume) by defeating the hypervisor. Powered by 8 CPU cores (16 threads) at 3.5 GHz and a GPU at 2.23 GHz, it turns your PS5 Phat, Slim or Pro running **13.00, 13.20, 13.40, 13.42, or 13.60** into a capable desktop PC for Steam games and emulators.

This repository is **TheRealRexo's fork** of the upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader). It adds a built-in WebUI + anonymous FTP server, M.2 install tools that store the OS as a single `.img` file, USB-less boot, and an in-payload "Download here" button to fetch official OS images straight from the GitHub release.

## Features

- HDMI 4K60 video and audio output
- M.2 SSD as dedicated Linux partition
- All USB ports usable for peripherals
- BD drive usable via custom ahci driver
- Internal Bluetooth usable via custom xhci driver
- Ethernet port usable via custom gbe driver
- Hardware video decode, encode and transcode (VCN) via VAAPI

## What's new in this fork

- **Multi-firmware HV exploit**: 3.00–7.61, 11.60, 12.xx, and 13.00/13.20/13.40/13.42/13.60 (Phat/Slim); 13.60 (Pro), auto-selected from the running firmware. Only 13.xx firmwares support REXO fork features (M.2 install, USB-less boot, in-payload downloads).
- **Built-in WebUI** on port 8080 (no auto-boot) + anonymous FTP on port 21
- **OS-image picker**, boot/install target selector, install-size field, VRAM setting
- **File manager** (list / upload / delete), including `/mnt/ext1` for USB-free setups
- **In-payload "Download here"**: fetch official OS images from the GitHub release straight onto the console (multi-part aware). Uses direct tagged-release URLs to avoid HTTP 302 redirects — required because the SCE HTTP2 client on the PS5 does not follow redirects automatically.
- **Hardware / driver support table** (GPU, WiFi/Bluetooth, Ethernet, audio, NVMe, USB)
- **M.2 install tools** (`m2_init`, `ps5-osinstall`, `ps5-osexec`) storing the OS as a single `.img`
- **Loop-mounted `.img` boot**: patched initrd re-scans the NVMe GPT (BLKRRPART), searches M.2 → USB, regenerates `root=LABEL=...` automatically

## PS5 firmware

| Model | Supported firmwares | M.2 |
|-------|---------------------|-----|
| PS5 Phat & Slim | 3.00–7.61 · 11.60 · 12.xx · 13.00, 13.20, 13.40, 13.42, 13.60 | Yes (except 3.xx; 11.60 untested) |
| PS5 Pro | 13.60 | Yes |

Support for 1.xx / 2.xx may come later. You cannot downgrade firmware.

**Only 13.xx (particularly 13.60) supports the REXO fork features:** M.2 install tools, USB-less boot, in-payload "Download here", and `/mnt/ext1` searches. Older firmwares boot Linux through upstream paths but do not include these features.

## USB-less boot

The payload downloads the OS image directly from the GitHub release (in-payload "Download here", or the `images.json` URLs), derives `cmdline.txt` (root label) and `vram.txt` from the selected `.img`, and the patched initrd loop-mounts the `.img` from the M.2/internal ssd. No USB stick is required: the PS5 kernel ships the NVMe driver and shifted-GPT support, and `ps5-osinstall` (baked into every wrapped image) handles installation.

**Multi-part download**: large images (>2 GB) ship as ≤2 GB parts. The payload's "Download here" fetches all parts, concatenates them, and decompresses XZ in-payload to produce the raw `.img` bootable image.

**The only file you download is the `.img`.** All the other boot files (`bzImage`, `initrd.img`, `cmdline.txt`, `vram.txt`) and the install-time parameters are generated/managed by the payload from what you put in the WebUI:

- **Boot only** or **install to target** (USB port / internal M.2) — the payload writes the matching `ps5.mode` / `ps5.target` / `ps5.size_gb` boot params;
- **Install size** (GB, `0` = image's natural size);
- **VRAM** (MiB, `0` = auto from the image);
- **Image** — the loader reads the ext4 root label from the selected `.img` and generates `root=LABEL=...` for it.

The patched initrd loop-mounts that `.img` at boot, so the OS is stored as a file and never gets cleared.

## Hardware

Required: USB drive ≥64 GB (external SSD preferred) + USB keyboard/mouse (dongles supported).
Optional: USB WLAN adapter, M.2 SSD compatible with PS5, Bluetooth dongle for DualSense.

## Configure PS5 settings

- **VERY IMPORTANT**: Rest Mode → System → Power Saving → Features Available in Rest Mode → Supply Power to USB Ports = Always.
- **VERY IMPORTANT**: Settings → HDMI → Enable HDMI Device Link = off.
- *Recommended*: Disable automatic updates / error reporting.

## Installation

1. Get a Linux image (pre-built from the latest github release), or build via `build_image.sh --distro ubuntu2604`).
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
- `images.json` — manifest of `.img.xz` download URLs (GitHub release assets).

**Images >2 GB are split into ≤2 GB parts.** GitHub caps per-asset size at 2 GiB − 1 bytes, so any `.img.xz` over that limit is split into consecutive byte chunks:

- `ps5-<distro>-os.img.xz` — part 1 (canonical name)
- `ps5-<distro>-os.img.xz.part2` — part 2
- `ps5-<distro>-os.img.xz.part3` — part 3, etc.

The payload's "Download here" button fetches all parts, concatenates them, and decompresses XZ in-payload to produce the raw `.img` bootable image. You never need to manually concatenate or decompress — the loader does it for you.

**You only ever download the `.img`.** The release's boot tarballs / generated files are convenience artifacts — the payload itself generates the boot configuration from the WebUI settings and the selected image. See [USB-less boot](#usb-less-boot).

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
- Q: DualSense controller support? A: Yes, via internal BT or dongle.
- Q: Supported Resolutions? A: 1080p/1440p/2160p @60Hz; 1440p@120Hz confirmed on DELL S3225QC.
- Q: After reboot a "Repairing"/"wasn't turned off properly" screen appears, is that normal? A: Yes, harmless.

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
