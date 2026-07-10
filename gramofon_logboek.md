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

---

## 8. Audio-driver debugstatus — 2026-05-18

### Context

- Werkdirectory: `/home/krijn/openai/Gramofon`.
- OpenWrt buildtree: `/home/krijn/openai/Gramofon/openwrt`.
- Device draait OpenWrt `24.10.6`, kernel `6.6.127`.
- SSH: `root@192.168.178.115`.
- Seriele console: detached screen-sessie `screen -r gramofon` op `/dev/ttyUSB0 115200`.
- Volgende sessie opnieuw verbinden via zowel `screen -r gramofon` als SSH.
- Belangrijk: voor elke `aplay`-test eerst de gebruiker waarschuwen zodat de koptelefoon opgezet kan worden.

### Doel

Audio via 3.5mm jack werkend krijgen met de custom `ath79-i2s`/`ak4430et` kernelmodule voor AR9341 + AK4430ET.

### Huidige bronstatus

De daadwerkelijke package die gebouwd wordt staat hier:

```bash
/home/krijn/openai/Gramofon/openwrt/package/kernel/ath79-audio
```

Niet verwarren met de top-level referentiekopie:

```bash
/home/krijn/openai/Gramofon/package/ath79-audio
```

Op 2026-05-18 zijn in de OpenWrt-package de volgende wijzigingen aangebracht:

- `ath79-i2s.h`: MBOX interrupt registers gecorrigeerd naar `INT_STATUS=0x44`, `INT_ENABLE=0x4c`.
- `ath79-i2s.h`: interruptbits gecorrigeerd naar RX complete `BIT(10)` en TX complete `BIT(6)`.
- `ath79-i2s.h`: FIFO-resetbits gecorrigeerd naar RX `BIT(2)` en TX `BIT(0)`.
- `ath79-mbox.c`: playback teruggezet naar MBOX RX-kanaal (`DDR -> MBOX FIFO -> I2S out`).
- `ath79-mbox.c`: capture blijft MBOX TX-kanaal (`I2S in -> FIFO -> DDR`).
- `ath79-mbox.c`: descriptor `length` voor playback op `0` gezet.
- `ath79-i2s-drv.c`: playback-interruptpad gebruikt RX complete en schrijft RX `RESUME`.
- `ath79-i2s-drv.c`: diagnostische logging nog aanwezig rond trigger/start en MBOX registerdump.

### Build/install laatste run

Package-build gelukt:

```bash
cd /home/krijn/openai/Gramofon/openwrt
make package/ath79-audio/compile V=s
```

Geinstalleerd op device en gereboot:

```bash
scp bin/targets/ath79/generic/packages/kmod-ath79-audio_6.6.127-r1_mips_24kc.ipk root@192.168.178.115:/tmp/
ssh root@192.168.178.115 "opkg install --force-reinstall /tmp/kmod-ath79-audio_6.6.127-r1_mips_24kc.ipk && reboot"
```

Na reboot kwam SSH terug. Modulelog:

```text
ath79-i2s 180b0000.i2s: probe: INT_ENABLE at boot=0x00000000 INT_STATUS=0x00000001
ath79-i2s 180b0000.i2s: RESET_MODULE before audio deassert: 0x200040c8
ath79-i2s 180b0000.i2s: RESET_MODULE after audio deassert:  0x200040c8
ath79-i2s 180b0000.i2s: AR9341 I2S ready (MCLK=GPIO22 BICK=GPIO21 LRCK=GPIO20 SDTO=GPIO18)
```

### Laatste audio-test

Na reboot moest de test-WAV opnieuw naar `/tmp` gekopieerd worden:

```bash
scp /tmp/gramofon-stereo-test.wav root@192.168.178.115:/tmp/gramofon-stereo-test.wav
```

Laatste testcommando:

```bash
ssh root@192.168.178.115 "dmesg -c >/tmp/dmesg-before-audio4.log; aplay -v -D hw:0,0 -f S16_LE -c 2 -r 44100 /tmp/gramofon-stereo-test.wav; rc=\$?; echo APLAY_RC=\$rc; dmesg | grep -E 'ath79-i2s|INT_ENABLE|trigger START|post-start|mbox dump|writeprobe|audio PLL|DPLL|RESET_MODULE'; exit \$rc"
```

Resultaat: nog geen audio; `aplay` faalt met I/O error.

```text
aplay: pcm_write:2178: write error: I/O error
APLAY_RC=1
```

Belangrijke vooruitgang: `INT_ENABLE` schrijft nu wel correct terug:

```text
INT_ENABLE mask=0x00000400 before=0x00000000 wrote=0x00000400 readback=0x00000400
```

Laatste relevante dump:

```text
trigger START: STEREO=0x00a21302 INT_ENABLE=0x00000400 INT_STATUS=0x00000005 RX_CTRL=0x00000000 irq_count=0
post-start: INT_STATUS=0x00000005 FIFO_STATUS=0x00000000 RX_CTRL=0x00000000 desc0.OWN=1 irq_count=0
mbox dump: 00=000000ff 04=00010032 08=00000000 0c=00000002 10=00000062 14=00000040 18=02d68000 1c=00000000 20=00000000 24=00000000 44=00000005 4c=00000400 58=00000000 60=02f26348 64=02d8c0a8 68=0ffc8010
writeprobe ff: 1c=00000000 24=00000000 44=00000005 4c=00000575
writeprobe restore: 1c=00000000 24=00000000 44=00000015 4c=00000400
```

### Huidige conclusie

De registermap voor interrupts was fout en is nu verbeterd. Het resterende hoofdprobleem is dat de MBOX DMA-control registerwrite nog steeds genegeerd wordt:

```text
RX_CTRL blijft 0x00000000 na START
desc0.OWN blijft 1
irq_count blijft 0
```

Waarschijnlijk is er nog een reset/enable/clock-gate voor MBOX DMA actief. Verdachte punten:

- MBOX offset `0x0c` staat op `0x00000002` in de dump.
- Resetmodule `0x1806001c` blijft `0x200040c8` ondanks clearpoging.
- De top-level referentiekopie heeft code rond `AR9341_MISC_BASE 0x18060010` en clearing van `BIT(10)` bij `+0x0c`; dit komt effectief ook uit op `0x1806001c`, maar moet opnieuw gecontroleerd worden tegen OpenWrt reset-driver semantics.
- Mogelijk is er naast reset de HOST_DMA/MBOX DMA engine zelf nog disabled of gated.

### Volgende stappen

1. Verbind opnieuw met seriele console: `screen -r gramofon`.
2. Controleer SSH: `ssh root@192.168.178.115`.
3. Inspecteer reset/clock/MBOX gating voordat opnieuw audio gestart wordt.
4. Onderzoek specifiek MBOX offset `0x0c`, resetregister `0x1806001c`, en eventuele HOST_DMA enable/reset bits.
5. Patch minimaal zodat START op RX control blijft hangen of verklaar waarom register `0x1c` write-only/auto-clear is.
6. Bouw opnieuw alleen package `ath79-audio`.
7. Installeer kmod, reboot, kopieer test-WAV terug naar `/tmp`.
8. Waarschuw gebruiker voor `aplay`.
9. Test opnieuw met `aplay -v -D hw:0,0 -f S16_LE -c 2 -r 44100 /tmp/gramofon-stereo-test.wav`.

## 2026-07-07 — Regressie "0 ISRs" opgelost; kernprobleem geïsoleerd

### Oorzaak regressie gevonden
De edit van build-`ath79-mbox.c` op 2026-06-07 12:31 voegde precies terug wat
commit `8fd3200` eerder empirisch had verwijderd:

- `RX_QUANTUM` in DMA_POLICY (quantum-mode vereist RESUME per descriptor en
  blokkeerde de engine volledig)
- `START|RESUME` i.p.v. alleen `START` in `ath79_mbox_dma_start`
- `6 << RX_FIFO_THRESH_SHIFT` (bits 15:12 bestaan niet; threshold zit op 7:4)

### Fix toegepast (alleen in buildkopie)
- `dma_start`: alleen `START` schrijven
- `dma_prepare`: QUANTUM-bits expliciet wissen, threshold op `TX_FIFO_THRESH_SHIFT`
- Herbouwd out-of-tree tegen `openwrt/build_dir/.../linux-6.6.127`, `.ko` naar
  `/lib/modules/6.6.127/` gekopieerd, reboot.

### Testresultaat (schone boot)
- Run 1: na ~1,27 s ISR #1 (status=0x405, played=2048) — maar dit tijdstip
  valt samen met het moment dat `aplay` opgeeft; vermoedelijk flush-artefact
  van de STOP-write, geen echte streaming.
- Run 2 (zelfde boot): NUL descriptors verwerkt; spurious ISR (status=0x5)
  al tijdens prepare.
- FIFO_STATUS blijft altijd 0; `aplay` faalt consistent met I/O error.

### Conclusie
DMA-programmering is terug conform de werkende referentie, maar de engine
streamt feitelijk nooit. Hoofdhypothese: het I2S/stereo-blok consumeert de
MBOX-FIFO niet (klok/enable/koppeling ontbreekt), waardoor de DMA na het
vullen van de FIFO stalt.

### Volgende stappen
1. Originele QCA/QSDK ar934x i2s+mbox-driver ophalen (zit niet in de repo)
   en registermap + init-volgorde vergelijken (o.a. RX_DESC 0x14 vs 0x18,
   MBOX_FRAME/FIFO_TIMEOUT-registers die wij nooit zetten).
2. Frame/slot-koppeling MBOX→stereo-blok onderzoeken (offsets 0x28–0x40).
3. BICK/LRCK fysiek meten op GPIO 20/21/22 (scope/logic analyzer).
4. Uitzoeken waarom status-bits 0/2 een ISR veroorzaken terwijl alleen
   bit 10 enabled is.

## 2026-07-07 (avond) — DMA streamt! Registerlayout-bugs gevonden via QSDK-vergelijking

### QSDK-referentie opgehaald
De originele QCA-driver (copyright Qualcomm Atheros 2012/2013) gevonden in
`GBert/openwrt-misc` (map `ath-i2s-qca`), gedownload naar de scratchpad.
Vergelijking van `ar71xx_regs.h` met onze `ath79-i2s.h` legde drie
fundamentele fouten bloot:

1. **START/STOP omgewisseld**: QCA definieert STOP=BIT(0), START=BIT(1).
   Wij hadden START=BIT(0), STOP=BIT(1). Elke "start" schreef dus een
   STOP-commando en omgekeerd. Alle eerdere empirische bevindingen
   (QUANTUM/RESUME-gedrag) waren hierdoor ongeldig.
2. **PLL MOD-register layout fout**: QCA: DIV_INT op bits 6:1,
   DIV_FRAC op bits 28:11. Wij: INT op 23:18, FRAC op 17:0. De audio-VCO
   werd dus compleet verkeerd geprogrammeerd.
3. **DPLL-bitposities fout**: DO_MEAS=BIT(30) (wij BIT(0)),
   MEAS_DONE=BIT(3) (wij BIT(0)), KI/KD/RANGE/PHASESH allemaal anders.
   Daarom kwam de DPLL-meting nooit klaar.
   Ook: BYPASS=BIT(4) (wij BIT(6)), REFDIV_MASK=0xf (wij 0x1f).

### Overige verschillen conform QCA hersteld
- `dma_reset`: volledige MBOX-modulereset via RESET_MODULE (0x18060000+0x1c,
  bit 1) + FIFO-reset — was alleen FIFO-reset.
- POLICY: RX_QUANTUM terug aan voor playback, threshold 6 op bits 7:4.
- `hw_params`: `ath79_stereo_reset()` na elke format-wijziging (QCA doet dit).
- startup: I2S_DELAY weg (QCA zet die niet); I2S_WORD_SIZE bij S24/S32.
- stereo_reset: RESET-bit is self-clearing, niet meer handmatig wissen.
- **ISR: RESUME-writes verwijderd** — QCA schrijft nérgens RESUME. Onze
  RESUME na een late interrupt (na TRIGGER_STOP) herstartte de engine, die
  dan eeuwig door de (vrijgegeven!) descriptor-ring bleef lopen → wilde DMA
  → AHB-hang → watchdog-reboot. Dit verklaarde de "crashes" bij run 2.

### Debug-valkuil: log-storm
Per-ISR `dev_info` (2 regels per ~11 ms) over de 115200-baud console
verstikte de CPU volledig: SSH viel weg en het apparaat leek gecrasht.
Nu alleen de eerste 5 interrupts loggen; poll-worker uitgeschakeld.

### Resultaat
- DPLL-meting slaagt direct (MEAS_DONE meteen, sqsum≈0x2xx–0x4xx).
- DMA verwerkt descriptors in exact realtime-tempo (2048 B ≈ 11,6 ms bij
  44,1 kHz) — de klok klopt.
- `aplay -d 3` × 2 op dezelfde boot: beide exit=0, geen I/O error, geen
  crash. Het "works once"-probleem is weg.

### Openstaand
- Luistertest: komt er daadwerkelijk geluid uit de 3,5mm-jack?
- Diagnostische logging verder opruimen zodra geluid bevestigd is.

## 2026-07-07 (laat) — Stilte-onderzoek: digitaal pad 100% bewezen, analoog pad verdacht

### Uitgesloten oorzaken (deze sessie)
- ~~Volumeregister~~: STEREO_VOLUME (0x04) resette op 0x0, schrijfbaar
  (0x0808 gezet in startup) — geen verschil hoorbaar.
- ~~MCLK-ratio~~: AK4430 accepteert in Normal Speed géén 256fs; PLL-tabel
  aangepast naar 512fs (extdiv 6→3, posedge 2→4 voor 44.1k; alle rates
  herrekend, zie ath79-i2s-pll.c). Bevestigd actief (posedge=4 in log),
  nog steeds stilte.
- ~~GPIO-mux overschreven~~: FUNC4/FUNC5 + OE tijdens playback correct.
- ~~Signalen verlaten SoC niet~~: GPIO_IN toggle-mask tijdens playback =
  0x00740000 = exact bits 18/20/21/22 → alle vier I2S-pads toggelen fysiek.

### Belangrijke feiten
- AK4430ET datasheet (scratchpad ak4430et.pdf): GEEN PDN-pin; pin 3 =
  SMUTE (pull-down, H=mute); pin 8 = DIF (pull-up, H=I²S → dan hoort
  I2S_DELAY aan — nu uit; geeft hooguit vervorming, geen stilte);
  pinout: 4=MCLK 5=BICK 6=SDTI 7=LRCK, 9/10=AOUTR/L.
- U-Boot identificeert als "AP123" (QCA AR9341-referentiebord).
- CUS227-referentie (gbert-mach-cus227.c) gebruikt exact onze pinnen:
  MCLK=22 SD=18 WS=20 CLK=21 → DTS-schatting is conform QCA-referentie.
- GPIO 0/1/2 en 4-7 staan sinds boot als output (U-Boot) — kandidaten
  voor een SMUTE/amp-enable-lijn van de fabrieksdesign.

### Hoofdhypothese
SMUTE van de AK4430 (of een amp/mute-circuit) hangt aan een GPIO die
hoog staat. Fabriekssoftware zette die pas laag bij afspelen.

### Klaarstaand (gebouwd, NOG NIET gedeployed)
Module-parameter `force_gpio_level` (schrijfbaar via
/sys/module/ath79_i2s/parameters/force_gpio_level):
- -1 = uit (default), 0 = kandidaat-GPIO's laag, 1 = hoog
- Kandidaten: 0-8, 13, 15, 16, 17, 19, 23 (mux→GPIO, output, niveau)
- Toegepast bij TRIGGER_START.
Testplan: deploy → force=0 → luisteren → force=1 → luisteren →
bij geluid: kandidatenlijst halveren (bisectie) tot de mute-pin gevonden is.

### Als GPIO-test niets oplevert → fysiek meten (multimeter)
Tijdens lange playback-loop op AK4430 (16-TSSOP, pin 1 = stip):
- Pin 4/5/7 (MCLK/BICK/LRCK) t.o.v. GND: toggelend ≈ 1,1-1,9 V DC;
  0 V of 3,3 V statisch = signaal komt niet aan → bedrading anders
- Pin 3 (SMUTE): moet 0 V zijn; 3,3 V = mute actief!
- Pin 11 (VDD): 3,3 V aanwezig?
- Pin 9/10 (AOUT): AC-stand, muziek → ~0,5-2 V AC

## 2026-07-09/10 — measure_freq-metingen: PLL blijft op 48k bij 44,1 kHz

### Meetuitslagen (measure_freq 12 = LRCK, software-flankenteller)
Zelfde boot, A/B/C-vergelijking (methode ondertelt ~0,5–4,5% door
interrupt-pauzes, maar is consistent binnen een meetreeks):
- 48 kHz-playback:  45.887 / 45.897 / 45.902 Hz  → klok = 48k ✓
- 44,1 kHz-playback: 45.882 / 45.901 Hz          → klok = **48k!** ✗
- 32 kHz-playback:  31.857 / 31.871 Hz           → klok = 32k ✓

### Conclusies
1. **De PLL schakelt wél om** (32k bewijst dat), maar de
   **44,1 kHz-familie-instelling wordt fysiek niet doorgevoerd** — LRCK
   blijft op 48k. De 6 kHz-fluit + ~9% te hoge toonhoogte bij 44,1 kHz
   zijn hiermee verklaard. De fout zit in de 44,1-rij van de PLL-tabel
   of in hoe die wordt toegepast (fractioneel deel narekenen!).
2. **Tweede probleem (gehoord door Krijn):** ook de correct
   omgeschakelde 32k-toon is niet stabiel. Alleen 48k (boot-default,
   PLL hoeft nooit om te schakelen) klinkt schoon. Elke omgeschakelde
   PLL-configuratie lijkt dus jitter/instabiliteit te hebben.
3. Meting van 2026-07-08 avond ("44,1 meet 43,8k") was misleidend:
   grote spreiding (42,0–43,8k), andere boot, geen A/B-referentie.

### Overlay-wipe-saga opgelost met sysupgrade.tgz-sluiproute
Symptoom: elke reboot na een deploy wiste de overlay ("not fully
initialized" → overlay_delete in fstools). Weerlegd als oorzaak:
flash-lock per boot, regio-protectie, CRC-fouten (dd/strings/dumps
bewezen writes en erases werken). Zelfs een verse jffs2 na `mtd -r
erase rootfs_data` wipete weer zodra er substantieel geschreven was —
de READY-symlink (.fs_state) overleeft de mount-scan niet (waarom is
nog open; verdenking: SUMMARY-nodes of read-instabiliteit bij boot).
**Werkende oplossing:** /sysupgrade.tgz — wordt door overlay_delete
expliciet bewaard en door /lib/preinit/80_mount_root uitgepakt VÓÓR
kmodloader. Inhoud: beide .ko's, /etc/config/firewall,
/etc/dropbear, /etc/rc.local. rc.local hermaakt het archief elke boot
(zelfvernieuwend, ook naar /overlay/sysupgrade.tgz) en de verse module
laadt nu elke boot met de juiste routing, wipe of geen wipe.
Offline-instrumentatie: scratchpad jffs2_fsstate.py (eigen big-endian
jffs2-parser voor mtd4-dumps).

### Volgende stappen
1. 44,1-PLL-rij narekenen tegen AR9344-datasheet (DIV_INT/DIV_FRAC,
   refdiv, extdiv/posedge) — waarom levert die fysiek 48k op?
2. Instabiliteit van omgeschakelde configuraties: DPLL-lock/KI/KD
   onderzoeken; registerdump 48k (boot-default) vs. 48k (na re-set)
   vergelijken — is een híngeschakelde 48k ook vuil?
3. PLL-registers dumpen tijdens 44,1-playback en diffen met 48k.

## Sessie 2026-07-10 avond — TWEE GROTE DOORBRAKEN

### Doorbraak 1: het wipe/crash-mysterie volledig opgelost
De DTS (qca9341_fon_fon2415.dts) definieerde de **resetknop op GPIO 12 —
onze LRCK-pin** (gegokte GPIO's, aldus het DTS-commentaar zelf). Gevolgen:
- Module-probe muxt GPIO12 → gpio-keys ziet "knop losgelaten na ≥5 s
  ingedrukt" → procd draait `jffs2reset -y` (**FACTORY RESET**, wipet
  ook de sysupgrade.tgz) + reboot. Serieel bewijs: "FACTORY RESET" op
  de console 0,5 s na de module-probe, reboot 13 s later.
- Elke playback-stop liet LRCK stilvallen → korte "knopdruk" →
  procd-REBOOT. **Alle "crashes" van de afgelopen dagen waren dus
  procd-reboots, geen wilde DMA.** measure_freq was onschuldig
  (correlatie kwam door playback-stops eromheen).
- **Fix runtime**: /etc/rc.button/reset vervangen door no-op, opgenomen
  in de sysupgrade.tgz-lijst. **Fix structureel**: DTS gecorrigeerd
  (reset-knop verwijderd, LED-GPIO's opgeschoond) — vergt image-flash.
- Bewijs dat het werkt: harde watchdog-reset overleefd zónder wipe,
  meerdere nette reboots zonder firewall-verlies.

### fstools-mechanisme begrepen (verklaring wipe-lus)
Elke boot-mount zet fs_state op PENDING; /etc/init.d/done zet READY aan
het boot-einde. Reboot vóór `done` (of factory reset) → volgende boot
wipet. `mount_root done` handmatig vóór reboot voorkomt de wipe — mits
/overlay écht jffs2 is (na verse format draait de eerste boot soms op
tmpfs en komt de tgz-restore pas ná kmodloader → ROM-module geladen;
één nette reboot corrigeert dat). Werkend deployprotocol staat in
~/.claude/jobs/76995c3e/tmp/deploy_stabiel.sh.

### Doorbraak 2: 44,1 kHz-klok schakelt nu bewezen fysiek om
- AR9344-datasheet: audio-PLL-MODULATION heeft START-bit (bit 0): uit =
  PLL volgt TGT_DIV direct; aan = trage ramp via MOD_STEP. CURRENT-
  register (0x3C, RO: FRAC 27:10, INT 6:1) toont de fysieke divider.
  Driver wist START nu altijd en logt TGT vs CUR (pll_log_current).
- Resultaat: CUR volgt TGT exact bij álle omschakelingen (boot-default
  20,32 → 44,1k-div 28,90 → 48k-div 23,58, alle richtingen).
- Opname-FFT-bewijs: 440,0 Hz exact bij 44,1k-playback én bij 48k na
  terugschakeling. Het dinsdag-mysterie ("blijft op 48k") is niet
  gereproduceerd; meetmethode voortaan: opname-FFT i.p.v. measure_freq.
- pll_44k_variant module-param (runtime): 0 = VCO 722,53/ext4,
  1 = VCO 541,90/postpll÷4/ext6, 2 = QSDK 541,90/÷8/ext3.

### NIEUW hoofdprobleem: ~6 kHz-"fluit"/bromtoon domineert de toon
- 44,1k: fluit 6020 Hz (+5820/5920/6300/6700/6800), 250× de toon.
- 48k: fluit 6196 Hz (+5716/6596/6676), ook bij boot-verse eerste
  playback — dus GEEN omschakel-effect en GEEN VCO-effect (variant 0
  en 1 klinken identiek).
- De 440 Hz-toon staat er telkens exact maar ~48 dB te zacht onder —
  ≈ 8 bits → **hypothese: byte-verschuiving in het MBOX→I2S-datapad**
  (DAC krijgt sample 8 bits verschoven; rommel in de hoge bits = fluit).
- Dinsdag was 48k aantoonbaar schoon ("fors, zuiver") — bisect loopt:
  dinsdag-driver (git HEAD 4a1671a, build 34579aba) herbouwd en
  gedeployed; luistertest = eerste actie volgende sessie (of vanavond).
- Let op: test-WAV's verschilden in niveau (ffmpeg sine = −18 dBFS!);
  nieuwe referentie-WAV: toon_440_48k_vol.wav (−1 dBFS, python).

### Volgende stappen
1. Bisect afronden: is dinsdag-driver (34579aba) boot-vers schoon op
   48k met toon_440_48k_vol.wav? Zo ja → delta zit in vanavond's
   pll.c-wijzigingen (functioneel no-ops — dan grondig diffen!);
   zo nee → omgeving/DAC-toestand onderzoeken.
2. Byte-shift-hypothese: MBOX FIFO-reset-volgorde t.o.v. START diffen
   met QSDK (ath79-pcm.c dma_start doet fifo_reset per start).
3. Image bouwen + flashen met gecorrigeerde DTS (resetknop weg) zodat
   de rc.button-workaround overbodig wordt.

### Bisect-uitslag (zelfde avond, 21:25): drivercode onschuldig
Dinsdag-driver (4a1671a, build 34579aba) boot-vers op 48k met −1 dBFS-
WAV: zelfde brom (5956 Hz, 44× de toon). De brom schaalt mee met het
signaalniveau → signaal-gecorreleerde vervorming (bit-misuitlijning-
spoor), geen jitter/stoorbron. Verschil met dinsdag's schone 48k zit
dus NIET in de drivercode — wat er wél veranderde is de openstaande
vraag voor de volgende sessie.
