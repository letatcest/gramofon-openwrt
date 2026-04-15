# OpenWrt voor Fon Fonera Hub FON2415 (Gramofon)

![Build Status](https://github.com/letatcest/gramofon-openwrt/actions/workflows/build.yml/badge.svg)

OpenWrt-port voor de **Fon Fonera Hub FON2415**, ook bekend als de **Gramofon** — een cloud-muziekstreamer waarvan de originele cloud-infrastructuur al jaren buiten gebruik is. Dit project geeft het apparaat een nieuw leven als zelfstandige audiospeler.

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
- [x] Build geverifieerd succesvol (OpenWrt 23.05.3, kernel 5.15.150)
- [x] Geflasht en getest — OpenWrt boot, Ethernet werkt, WiFi-interface aanwezig
- [ ] GPIO-nummers voor LED's en resetknop verifiëren
- [ ] Ethernet PHY-configuratie verifiëren (AR9341 + Atheros S27)
- [ ] Audio (AK4430ET) werkend

## Bijdragen

Heb je een Gramofon en wil je helpen testen? Issues en pull requests zijn welkom. Met name GPIO-nummers voor LED's en de exacte Ethernet PHY-configuratie zijn nog niet geverifieerd op hardware.
