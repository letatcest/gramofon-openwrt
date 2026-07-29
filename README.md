# OpenWrt for Fon Fonera Hub FON2415 (Gramofon)

![Build Status](https://github.com/letatcest/gramofon-openwrt/actions/workflows/build.yml/badge.svg)

OpenWrt port for the **Fon Fonera Hub FON2415**, also known as the **Gramofon** — a cloud music streamer whose original cloud infrastructure has been defunct for years. This project gives the device a new life as a standalone audio player.

**Status July 2026: audio works.** An ALSA/ASoC driver has been written for the AR9341 I2S controller and the AK4430ET DAC; music plays back cleanly via the 3.5mm jack (`aplay -D hw:0,0`).

## Hardware

| Component | Specification |
|-----------|-------------|
| CPU | Atheros AR9341 @ 535MHz (MIPS 74Kc) |
| RAM | 64MB DDR2 |
| Flash | 16MB SPI NOR (MXIC MX25L128) |
| WiFi | 2.4GHz 802.11b/g/n (integrated) |
| Ethernet | 2 ports via Bi-TEK FM-1024LLF switch |
| Audio DAC | AKM AK4430ET (3.5mm jack) |
| UART | J2 header, 115200 8N1, 3.3V TTL |
| Board name | Fonera HUB FON2415 (internal, U-Boot) |

## Flash layout

| Partition | Offset | Size | Contents |
|----------|--------|---------|--------|
| u-boot | 0x000000 | 128KB | Bootloader — do not overwrite |
| firmware | 0x020000 | 15.75MB | Kernel + rootfs (target area) |
| fon_data | 0xfe0000 | 64KB | Fon-specific data |
| art | 0xff0000 | 64KB | **WiFi calibration — NEVER overwrite** |

## Downloading the image

Go to [Actions](https://github.com/letatcest/gramofon-openwrt/actions), click the latest successful build, and download the `fon2415-openwrt-image` artifact.

## Flashing via U-Boot (UART)

Connect via UART (J2 header, 115200 8N1), interrupt the boot within 1 second.

```
# Set up networking
setenv ipaddr 192.168.178.200
setenv serverip 192.168.178.131   # IP of your TFTP server

# Test
ping 192.168.178.131

# Flash (expects fon2415.image on the TFTP server)
run lf

# Reboot
reset
```

Place `fon2415.image` (the factory build) in your TFTP directory:
```bash
cp fon2415.image /var/lib/tftpboot/
sudo systemctl start tftpd-hpa
```

After reboot: http://192.168.1.1 — LuCI web interface, empty password.

## Audio (AK4430ET via I2S)

The audio driver lives in [`package/ath79-audio/`](package/ath79-audio/) and consists of two kernel modules:

- **`ath79-i2s.ko`** — platform driver for the AR9341 stereo/I2S controller: MBOX DMA, audio PLL (fractional-N), and clock configuration.
- **`ak4430et.ko`** — codec driver for the AKM AK4430ET DAC (clock slave, no register interface).

| Property | Value |
|------------|--------|
| ALSA device | `hw:0,0` (playback, stereo) |
| Formats | S16_LE, S16_BE, S32_BE |
| Sample rates | 22.05 – 96 kHz (32 / 44.1 / 48 kHz electrically verified) |
| Clocks | MCLK = 512fs (GPIO14), BICK = 64fs (GPIO13), LRCK = fs (GPIO12), SDTO = GPIO15 |

Playback:

```bash
aplay -D hw:0,0 music.wav    # 48 kHz, S16_LE, stereo
```

### Key finding

The AK4430 requires a bit clock of **at least 48fs**. The AR9341 defaults to 32fs for 16-bit data; the `I2S_WORD_SIZE` bit in `STEREO_CONFIG` must therefore also be set for S8/S16, so that BICK runs at 64fs. Without that setting, the DAC only produces a harsh 6 kHz drone. See [`gramofon_logboek.md`](gramofon_logboek.md) for the full investigation log.

### Limitations

- **RAM**: 64 MB (≈ 56 MB usable). `/tmp` is tmpfs; keep audio files there ≤ 12 MB, otherwise an OOM crash follows.
- **Streaming over ssh** (`... | ssh aplay -`) stutters: dropbear encryption is too heavy for the 535 MHz MIPS core. Copy files to `/tmp` beforehand, or use an unencrypted transport.
- **S24 and S32_LE formats** are deliberately not advertised: the 24-bit FIFO path can hang the device, and `PCM_SWAP` byte-swaps per 16-bit half-word (garbles 32-bit LE).

## Building locally

```bash
git clone https://github.com/letatcest/gramofon-openwrt
cd gramofon-openwrt
chmod +x build_openwrt.sh
./build_openwrt.sh
```

Builds against OpenWrt **v24.10.6** (latest stable). Takes 30–90 minutes on the first run.

## Status

- [x] Hardware identified
- [x] UART/U-Boot access
- [x] DTS file created
- [x] GitHub Actions build pipeline
- [x] Build verified successful (OpenWrt 24.10.6, kernel 6.6.127)
- [x] Flashed and tested — OpenWrt boots, Ethernet works, WiFi working (AP mode)
- [x] **Audio (AK4430ET) working** — clean music playback, linearity and PLL switching (32/44.1/48 kHz) verified (2026-07-15)
- [ ] Verify GPIO numbers for LEDs and reset button (reset button was incorrectly on GPIO12 = LRCK; removed from the DTS)
- [ ] Verify Ethernet PHY configuration (AR9341 + Atheros S27)
- [ ] Trap the S24 hang bug in the FIFO path
- [ ] Flash the updated DTS image and clean up diagnostic module parameters
- [ ] Streaming from standard phone apps (DLNA/UPnP renderer or AirPlay)

## Contributing

Do you have a Gramofon and want to help test? Issues and pull requests are welcome. In particular, the GPIO numbers for the LEDs and the exact Ethernet PHY configuration have not yet been verified on hardware.
