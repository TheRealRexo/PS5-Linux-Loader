# ps5-linux-loader (13.60)

A **firmware-13.60-only** fork of the official [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader), based on the [13.60 Fix (#46)](https://github.com/ps5-linux/ps5-linux-loader/pull/46) merge.

This build targets **PS5 Phat and Slim running firmware 13.60** only. It defeats the hypervisor and prepares the system so that Linux boots on **rest-mode resume**. All code paths for firmwares 3.00-7.61 have been stripped, leaving a smaller, single-purpose payload.

The exploit/kernel shellcode for 13.60 is committed directly (`source/shellcode_1360.c`), so this build requires **no separate `shellcode_hv`/`shellcode_kernel` cross-compilation** — a plain `make` against the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) produces the payload.

> **Credit / provenance.** This is a thin 13.60-only packaging of the upstream project. All exploit, HV, and Linux-boot logic comes from the official authors — see [Credits](#credits). If you want multi-firmware support or the full project, use the [upstream loader](https://github.com/ps5-linux/ps5-linux-loader).

## Firmware

- **13.60 only.** The payload refuses to run on any other firmware.

## Requirements

- **Required**: USB drive, minimum 64 GB (external SSD strongly recommended) to hold the Linux image.
- **Required**: USB keyboard/mouse (dongles supported).
- **Optional**: USB WLAN adapter (Marvell chipsets only), Bluetooth dongle for DualSense, M.2 SSD for a dedicated Linux partition.

## Configure PS5 settings

- **Very important**: enable Rest Mode power — `Settings` → `System` → `Power Saving` → `Features Available in Rest Mode` → `Supply Power to USB Ports` = `Always`.
- **Very important**: disable HDMI Device Link — `Settings` → `HDMI` → `Enable HDMI Device Link` = off.
- **Recommended**: disable automatic updates and automatic error reporting.

If you reset your PS5 settings or reinstall the firmware, reapply these.

## Installation

### 1. Get a Linux image

Download a pre-built image from [ps5-linux-image](https://github.com/ps5-linux/ps5-linux-image/releases/tag/latest) (recommended: `ps5-ubuntu2604.img.xz`) and unpack the `.xz`. Or build your own with the `build_image.sh` script in that repo (Docker required).

### 2. Flash the image to USB

Minimum 64 GB; external SSD strongly recommended.

Linux/macOS:

```bash
# check the drive name with lsblk / diskutil list
sudo dd if=ps5-ubuntu2604.img of=/dev/sdX bs=4M status=progress conv=fsync
```

Windows: use [Balena Etcher](https://etcher.balena.io/).

### 3. Plug the USB drive into your PS5

Supported boot ports: front bottom **Type-C**, and rear **Type-A**. The front top Type-A is USB 2.0 (slower, not recommended).

### 4. Run the 13.60 jailbreak and send the payload

Run your preferred 13.60 kernel exploit (e.g. via your usual 13.60 jailbreak chain), then send the payload:

```bash
socat -t 99999999 - TCP:$PS5IP:9021 < ps5-linux-loader.elf
```

If successful, the console goes into rest mode. Wait for the orange LED to stop blinking and become static, then press the power button to boot into Linux. On a successful boot the **LED turns white**. If it boots back into the PS5 OS you pressed the button too early, or rest-mode features were not enabled. If it freezes instead of entering rest mode, an incompatible HEN/kstuff may be enabled — disable it.

If the LED is white but the screen is black:

- Try removing the `video=DP-1:1920x1080@60` line in `cmdline.txt`.
- Toggle HDCP on/off.
- Try different monitors/capture cards and resolutions.
- Try `amdgpu.force_1080p=1` in `cmdline.txt` on the FAT32 partition.

## Building from source

Install the [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk), then:

```bash
git clone <this-repo>
cd <this-repo>
make
```

The payload is written to `bin/ps5-linux-loader.elf`. A plain `make` is sufficient — there are no sub-builds.

## After first boot

See the upstream [ps5-linux README](https://github.com/ps5-linux/ps5-linux-loader) for the full post-install guide: disabling the screensaver, holding kernel packages against updates, installing Firefox/mesa, `ps5-linux-tools`, WLAN drivers, M.2 setup, and fan/boost control. Those steps are unchanged.

## FAQ

- **Q: Will >=8.00 firmwares be supported?** A: No.
- **Q: Can I dual-boot Linux and PS5 OS?** A: No — this is a soft-mod; re-run the exploit to boot Linux.
- **Q: Can I continue using my PS5 if I install Linux?** A: Yes, the internal SSD is not modified.
- **Q: After reboot I get a "Repairing" / "Your PS5 wasn't turned off properly" screen, is that normal?** A: Yes, and it's harmless.

## Credits

Upstream [ps5-linux/ps5-linux-loader](https://github.com/ps5-linux/ps5-linux-loader) and its contributors, including:

- [theflow](https://github.com/TheOfficialFloW) — ps5-linux-loader, patches, tools
- [c0w](https://github.com/c0w-ar) — ps5-linux-loader
- [Ouroboroshen](https://github.com/Ouroboroshen) — 13.60 Fix (#46)
- [fail0verflow](https://github.com/fail0verflow) — prosperous
- [flatz](https://github.com/flatz) — HV exploit
- [cragson](https://github.com/cragson) — HV exploit implementation
- [john-tornblom](https://github.com/john-tornblom) — PS5 SDK

See the upstream repository for the complete list.

## License

See [LICENSE](LICENSE).
