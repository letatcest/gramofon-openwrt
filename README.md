# OpenWrt voor Fon Fonera Hub FON2415 (Gramofon)

![Build Status](https://github.com/letatcest/gramofon-openwrt/actions/workflows/build.yml/badge.svg)

OpenWrt-port voor de **Fon Fonera Hub FON2415**, ook bekend als de **Gramofon** — een cloud-muziekstreamer waarvan de originele cloud-infrastructuur al jaren buiten gebruik is. Dit project geeft het apparaat een nieuw leven als zelfstandige audiospeler.

**Status juli 2026: de audio werkt.** Er is een ALSA/ASoC-driver voor de AR9341-I2S-controller en de AK4430ET-DAC geschreven; muziek klinkt schoon via de 3.5mm-jack (`aplay -D hw:0,0`).

## Hardware

| Component | Specificatie |
|-----------|-------------|
| CPU | Atheros AR9341 @ 535MHz (MIPS 74Kc) |
| RAM | 64MB DDR2 |
| Flash | 16MB SPI NOR (MXIC MX25L128) |
| WiFi | 2.4GHz 802.11b/g/n (geïntegreerd) |
| Ethernet | 2 poorten via Bi-TEK FM-1024LLF switch |
| Audio DAC | AKM AK4430ET (3.5mm jack) |
| UART | J2 header, 115200 8N1, 3.3V TTL |
| Board naam | Fonera HUB FON2415 (intern, U-Boot) |

## Flash-indeling

| Partitie | Offset | Grootte | Inhoud |
|----------|--------|---------|--------|
| u-boot | 0x000000 | 128KB | Bootloader — niet overschrijven |
| firmware | 0x020000 | 15.75MB | Kernel + rootfs (doelgebied) |
| fon_data | 0xfe0000 | 64KB | Fon-specifieke data |
| art | 0xff0000 | 64KB | **WiFi-kalibratie — NOOIT overschrijven** |

## Image downloaden

Ga naar [Actions](https://github.com/letatcest/gramofon-openwrt/actions), klik op de laatste succesvolle build, en download het artifact `fon2415-openwrt-image`.

## Flashen via U-Boot (UART)

Verbind via UART (J2 header, 115200 8N1), onderbreek de boot binnen 1 seconde.

```
# Netwerk instellen
setenv ipaddr 192.168.178.200
setenv serverip 192.168.178.131   # IP van je TFTP-server

# Testen
ping 192.168.178.131

# Flash (verwacht fon2415.image op de TFTP-server)
run lf

# Herstart
reset
```

Zet `fon2415.image` (de factory-build) in je TFTP-map:
```bash
cp fon2415.image /var/lib/tftpboot/
sudo systemctl start tftpd-hpa
```

Na herstart: http://192.168.1.1 — LuCI webinterface, wachtwoord leeg.

## Audio (AK4430ET via I2S)

De audiodriver zit in [`package/ath79-audio/`](package/ath79-audio/) en bestaat uit twee kernelmodules:

- **`ath79-i2s.ko`** — platformdriver voor de AR9341 stereo/I2S-controller: MBOX-DMA, audio-PLL (fractional-N) en klokconfiguratie.
- **`ak4430et.ko`** — codec-driver voor de AKM AK4430ET DAC (klok-slave, geen registerinterface).

| Eigenschap | Waarde |
|------------|--------|
| ALSA-apparaat | `hw:0,0` (afspelen, stereo) |
| Formaten | S16_LE, S16_BE, S32_BE |
| Samplerates | 22,05 – 96 kHz (32 / 44,1 / 48 kHz elektrisch geverifieerd) |
| Klokken | MCLK = 512fs (GPIO14), BICK = 64fs (GPIO13), LRCK = fs (GPIO12), SDTO = GPIO15 |

Afspelen:

```bash
aplay -D hw:0,0 muziek.wav    # 48 kHz, S16_LE, stereo
```

### Belangrijkste bevinding

De AK4430 eist een bitclock van **minimaal 48fs**. De AR9341 levert bij 16-bit data standaard 32fs; het `I2S_WORD_SIZE`-bit in `STEREO_CONFIG` moet daarom óók bij S8/S16 gezet worden, zodat BICK op 64fs staat. Zonder die instelling produceert de DAC alleen een harde 6 kHz-brom. Zie [`gramofon_logboek.md`](gramofon_logboek.md) voor het volledige onderzoeksverslag.

### Beperkingen

- **RAM**: 64 MB (≈ 56 MB bruikbaar). `/tmp` is tmpfs; houd audiobestanden daar ≤ 12 MB, anders volgt een OOM-crash.
- **Streamen over ssh** (`... | ssh aplay -`) hapert: dropbear-encryptie is te zwaar voor de 535 MHz-MIPS. Kopieer bestanden vooraf naar `/tmp`, of gebruik een onversleuteld transport.
- **S24- en S32_LE-formaten** zijn bewust niet geadverteerd: het 24-bit FIFO-pad kan het apparaat laten hangen en `PCM_SWAP` byteswapt per 16-bit halfwoord (verhaspelt 32-bit LE).

## Lokaal bouwen

```bash
git clone https://github.com/letatcest/gramofon-openwrt
cd gramofon-openwrt
chmod +x build_openwrt.sh
./build_openwrt.sh
```

Builds against OpenWrt **v24.10.6** (latest stable). Duurt 30–90 minuten bij de eerste keer.

## Status

- [x] Hardware geïdentificeerd
- [x] UART/U-Boot toegang
- [x] DTS-bestand aangemaakt
- [x] GitHub Actions build pipeline
- [x] Build geverifieerd succesvol (OpenWrt 24.10.6, kernel 6.6.127)
- [x] Geflasht en getest — OpenWrt boot, Ethernet werkt, WiFi werkend (AP-modus)
- [x] **Audio (AK4430ET) werkend** — schone muziekweergave, lineariteit en PLL-omschakeling (32/44,1/48 kHz) geverifieerd (2026-07-15)
- [ ] GPIO-nummers voor LED's en resetknop verifiëren (resetknop stond fout op GPIO12 = LRCK; uit de DTS verwijderd)
- [ ] Ethernet PHY-configuratie verifiëren (AR9341 + Atheros S27)
- [ ] S24-hangbug in het FIFO-pad afvangen
- [ ] Bijgewerkt DTS-image flashen en diagnostische module-parameters opruimen

## Bijdragen

Heb je een Gramofon en wil je helpen testen? Issues en pull requests zijn welkom. Met name GPIO-nummers voor LED's en de exacte Ethernet PHY-configuratie zijn nog niet geverifieerd op hardware.
