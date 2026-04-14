# Gramofon FON2515A02 — Logboek & Technische Documentatie

*Hacken & Hergebruiken | April 2026*

---

## 1. Doel

De Gramofon FON2515A02 is een cloud-muziekstreamer van Fon Wireless Limited. De cloudinfrastructuur van Fon is al jaren buiten gebruik, waardoor het apparaat onbruikbaar is geworden. Het doel van dit project is om de hardware te hergebruiken door aangepaste firmware (OpenWrt) te installeren, zodat het apparaat als zelfstandige audiospeler kan functioneren — bijvoorbeeld via AirPlay, Spotify Connect of een lokale muziekspeler (MPD/squeezelite).

---

## 2. Hardware Identificatie

Chips geïdentificeerd door het apparaat open te maken (geclipste behuizing, geen schroeven) en de PCB-markingen te lezen.

| Component    | Chip / Specificatie                                      |
|--------------|----------------------------------------------------------|
| CPU          | Atheros AR9341 DL3A — 535MHz MIPS 74Kc                  |
| RAM          | ESMT M14D5121632A — 64MB DDR2                            |
| Flash        | MXIC MX25L128 (MX25L1283SF M2I-10G) — 16MB              |
| WiFi         | Ingebouwd in AR9341 — 2.4GHz 802.11b/g/n                |
| Audio DAC    | AKM AK4430ET — 3.5mm jack uitgang                       |
| Ethernet     | 2 poorten via Bi-TEK FM-1024LLF switch                  |
| Board naam   | Fonera HUB FON2415 board (intern, uit kernellog)         |
| PCB opdruk   | 21514FON24151A1 / Rev:A1                                 |

### 2.1 Flash-partitie indeling (/proc/mtd)

```
mtd0: 00020000 00010000 "u-boot"       (128KB)
mtd1: 00fc0000 00010000 "firmware"     (15.75MB — hele firmware)
mtd2: 00ec6684 00010000 "rootfs"
mtd3: 00680000 00010000 "rootfs_data"  (beschrijfbare overlay, jffs2)
mtd4: 00010000 00010000 "fon_data"
mtd5: 00010000 00010000 "art"          (WiFi kalibratie — NOOIT overschrijven)
```

---

## 3. Toegang Verkregen

### 3.1 Failsafe webinterface

Activeren: knop ingedrukt houden bij opstarten, loslaten zodra LED rood knippert.

- Ethernetkabel in de **Computer-poort** van de Gramofon
- Statisch IP op desktop: `192.168.10.111 / 255.255.255.0`
- Bereikbaar op: `http://192.168.10.1`
- Toont een firmware-uploadknop (Choose File + Submit)
- Geen andere poorten open (nmap bevestigd: alleen poort 80)

### 3.2 UART seriële verbinding

UART-testpunten gevonden op header **J2** (4 pads verticaal op de PCB, achterkant).

| Pin (van onder naar boven) | Functie                              |
|----------------------------|--------------------------------------|
| Onderste                   | VCC — 3.3V stabiel                  |
| Pin 2                      | Onbekend / niet gebruikt             |
| Pin 3                      | GND — bevestigd met multimeter       |
| Bovenste                   | TX — fluctueert 2.1–2.45V           |

**Instellingen:** 115200 baud, 8N1, 3.3V TTL  
**Adapter:** CP2102 USB-naar-serieel  
**Aansluiting:**

```
Adapter zwart (GND)  →  J2 pin 3 (GND)
Adapter wit   (RX)   →  J2 bovenste pad (TX van Gramofon)
Adapter groen (TX)   →  J2 RX (pin 2, nog te bevestigen)
Adapter rood  (VCC)  →  NIET aansluiten
```

**Verbinding op desktop:**

```bash
screen /dev/ttyUSB0 115200
```

### 3.3 U-Boot bootloader

Bereikbaar door binnen 1 seconde na stroomtoevoer een toets in te drukken.  
Prompt: `fonera>`

**Relevante U-Boot commando's:**

```
bootm          — boot kernel image vanuit geheugen
tftpboot       — download bestand via TFTP naar RAM
erase          — wis flash geheugen
cp.b           — kopieer geheugen naar flash
run lf         — flash firmware via TFTP (verwacht 'fon2415.image')
run lu         — flash U-Boot via TFTP (verwacht 'fon2415.uboot')
ping           — netwerk testen
setenv         — omgevingsvariabele instellen
printenv       — omgevingsvariabelen tonen
```

**U-Boot omgevingsvariabelen (printenv):**

```
bootcmd=bootm 0x9f020000
bootdelay=1
baudrate=115200
ipaddr=192.168.10.1
serverip=192.168.10.100
lf=tftp 0x80060000 fon2415.image; erase 0x9f020000 +$filesize; cp.b $fileaddr 0x9f020000 $filesize
lu=tftp 0x80060000 fon2415.uboot; erase 0x9f000000 +$filesize; cp.b $fileaddr 0x9f000000 $filesize
```

**Netwerk instellen in U-Boot (werkend):**

```
setenv ipaddr 192.168.178.200
setenv serverip 192.168.178.131
ping 192.168.178.131
```

Netwerk werkt via **eth1** (niet eth0). Ping naar desktop succesvol bevestigd.

### 3.4 Root shell via init=/bin/sh

Login-systeem omzeilen via U-Boot:

```
setenv bootargs init=/bin/sh
boot
```

Geeft direct een BusyBox root shell zonder wachtwoord:

```
BusyBox v1.19.4 (2015-11-13 18:31:49 CET) built-in shell (ash)
/ #
```

---

## 4. Overzicht — Wat Werkte

| Stap                                          | Status                                    |
|-----------------------------------------------|-------------------------------------------|
| Apparaat openmaken (geclipst)                 | ✅ Gelukt                                 |
| Chips identificeren (CPU, RAM, Flash)         | ✅ Gelukt                                 |
| Failsafe webinterface bereiken                | ✅ Gelukt                                 |
| UART-pinnen vinden (J2)                       | ✅ Gelukt                                 |
| CP2102 aansluiten en verbinding maken         | ✅ Gelukt                                 |
| Bootlog lezen via seriële poort               | ✅ Gelukt                                 |
| U-Boot prompt bereiken                        | ✅ Gelukt                                 |
| Netwerk in U-Boot (ping desktop)              | ✅ Gelukt                                 |
| Root shell via init=/bin/sh                   | ✅ Gelukt                                 |
| Root wachtwoord resetten                      | ❌ Mislukt — squashfs read-only           |
| Firmware backup via netcat                    | ❌ Mislukt — /dev/mtd niet beschikbaar    |
| OpenWrt flashen                               | ⏳ Nog niet gedaan — image ontbreekt      |

---

## 5. Huidige Blokkade

Er bestaat geen officieel OpenWrt-profiel voor de FON2415/FON2515A02. Het flashen van een willekeurig AR9341-image is riskant omdat de flash-partitie-indeling apparaatspecifiek is. Een verkeerd image kan het apparaat onbruikbaar maken.

Een bericht is geplaatst op het OpenWrt-forum (forum.openwrt.org) met de volledige hardware-informatie, inclusief de partitie-indeling en U-Boot omgeving, om community-hulp te vragen.

---

## 6. Volgende Stappen

1. **Wachten op reactie OpenWrt-forum** — iemand met ervaring met FON2415/AR9341 kan een DTS-bestand aanleveren of een werkend image hebben.
2. **ART-partitie (mtd5) backuppen** — WiFi-kalibratie bewaren voordat er iets geflasht wordt.
3. **OpenWrt image bouwen** — met de juiste DTS kan een custom image gebouwd worden voor de exacte partitie-indeling.
4. **Flashen via `run lf` in U-Boot** — zodra een geschikt image beschikbaar is, direct flashen via TFTP.
5. **Audio-software installeren** — na succesvolle OpenWrt installatie: `shairport-sync` (AirPlay), `mpd` of `squeezelite`.

---

## 7. Notities

- De **AKM AK4430ET** audio DAC is aanwezig op de PCB en volledig hardware-ondersteund. Na installatie van OpenWrt is de 3.5mm audio-uitgang bruikbaar voor hifi-kwaliteit streaming.
- De **ART-partitie (mtd5)** bevat WiFi-kalibratie data die specifiek is voor dit apparaat. Deze mag nooit overschreven worden, ook niet per ongeluk.
- U-Boot gebruikt intern de naam **FON2415**, niet FON2515. Dit is relevant bij het zoeken naar community-support en firmware.
- Het huidige firmware-systeem is gebouwd op OpenWrt met kernel ~3.x en BusyBox 1.19.4 (2015).
