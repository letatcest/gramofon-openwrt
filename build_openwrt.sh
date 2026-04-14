#!/usr/bin/env bash
# build_openwrt.sh — Bouw een OpenWrt image voor de Fon FON2415 (Gramofon)
#
# Vereisten:
#   git, gcc, g++, make, libncurses-dev, zlib1g-dev, gawk, flex, bison,
#   unzip, python3, rsync, wget
#
# Gebruik:
#   chmod +x build_openwrt.sh
#   ./build_openwrt.sh
#
# Output:
#   openwrt/bin/targets/ath79/generic/
#     openwrt-ath79-generic-fon_fon2415-squashfs-factory.bin     ← voor eerste keer flashen
#     openwrt-ath79-generic-fon_fon2415-squashfs-sysupgrade.bin  ← voor updates

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENWRT_DIR="${SCRIPT_DIR}/openwrt"
OPENWRT_TAG="v23.05.3"   # Bewezen stabiel voor ath79; verhoog naar 24.x zodra beschikbaar

echo "=== Gramofon FON2415 — OpenWrt build script ==="
echo "    OpenWrt versie : ${OPENWRT_TAG}"
echo "    Werkmap        : ${OPENWRT_DIR}"
echo ""

# ── 1. Source ophalen ────────────────────────────────────────────────────────
if [ ! -d "${OPENWRT_DIR}" ]; then
    echo "[1/7] OpenWrt klonen (${OPENWRT_TAG})..."
    git clone --depth 1 --branch "${OPENWRT_TAG}" \
        https://git.openwrt.org/openwrt/openwrt.git "${OPENWRT_DIR}"
else
    echo "[1/7] OpenWrt map bestaat al, overslaan."
fi

cd "${OPENWRT_DIR}"

# ── 2. DTS-bestand kopiëren ──────────────────────────────────────────────────
echo "[2/7] DTS-bestand kopiëren..."
cp "${SCRIPT_DIR}/fon2415.dts" \
   "${OPENWRT_DIR}/target/linux/ath79/dts/qca9341_fon_fon2415.dts"
echo "      → target/linux/ath79/dts/qca9341_fon_fon2415.dts"

# ── 3. Image-definitie toevoegen aan generic.mk ──────────────────────────────
echo "[3/7] Device-profiel toevoegen aan generic.mk..."
GENERIC_MK="${OPENWRT_DIR}/target/linux/ath79/image/generic.mk"

# Controleer of de definitie al bestaat
if grep -q "fon_fon2415" "${GENERIC_MK}"; then
    echo "      → Al aanwezig, overslaan."
else
    # Voeg in na de laatste TARGET_DEVICES-regel (veilige plek)
    cat >> "${GENERIC_MK}" << 'EOF'

# Fon Fonera Hub FON2415 (Gramofon)
define Device/fon_fon2415
  SOC := ar9341
  DEVICE_VENDOR := Fon
  DEVICE_MODEL := Fonera Hub FON2415
  DEVICE_VARIANT := (Gramofon)
  DEVICE_DTS := qca9341_fon_fon2415
  DEVICE_DTS_DIR := ../dts
  IMAGE_SIZE := 15744k
  KERNEL_SIZE := 2048k
  IMAGES := sysupgrade.bin factory.bin
  IMAGE/sysupgrade.bin := append-kernel | append-rootfs | pad-rootfs | check-size | append-metadata
  IMAGE/factory.bin    := append-kernel | append-rootfs | pad-rootfs | check-size
  DEVICE_PACKAGES := kmod-ath9k
endef
TARGET_DEVICES += fon_fon2415
EOF
    echo "      → Toegevoegd aan generic.mk"
fi

# ── 4. Feeds bijwerken ───────────────────────────────────────────────────────
echo "[4/7] Feeds bijwerken en installeren..."
./scripts/feeds update -a
./scripts/feeds install -a

# ── 5. Config aanmaken ───────────────────────────────────────────────────────
echo "[5/7] Build-configuratie aanmaken..."
cat > "${OPENWRT_DIR}/.config" << 'EOF'
CONFIG_TARGET_ath79=y
CONFIG_TARGET_ath79_generic=y
CONFIG_TARGET_ath79_generic_DEVICE_fon_fon2415=y

# Kernel modules
CONFIG_PACKAGE_kmod-ath9k=y
CONFIG_PACKAGE_kmod-ath9k-htc=n

# Basis-packages
CONFIG_PACKAGE_luci=y
CONFIG_PACKAGE_luci-ssl=y
CONFIG_PACKAGE_openssh-sftp-server=y

# Optioneel: audio (uncomment als je de AK4430 wilt testen)
# CONFIG_PACKAGE_kmod-sound-soc-core=y
# CONFIG_PACKAGE_mpd-full=y
# CONFIG_PACKAGE_shairport-sync=y

# Grootte-optimalisaties (past comfortabel in 15.75MB)
CONFIG_TARGET_ROOTFS_SQUASHFS=y
CONFIG_TARGET_SQUASHFS_BLOCK_SIZE=256
EOF

# Expand config
make defconfig

# ── 6. Bouwen ────────────────────────────────────────────────────────────────
JOBS=$(nproc)
echo "[6/7] Bouwen met ${JOBS} cores..."
echo "      Dit duurt 30-90 minuten bij eerste keer."
echo ""
make -j"${JOBS}" V=s 2>&1 | tee "${SCRIPT_DIR}/build.log"

# ── 7. Output kopiëren ───────────────────────────────────────────────────────
echo ""
echo "[7/7] Output kopiëren naar ${SCRIPT_DIR}..."
OUTDIR="${OPENWRT_DIR}/bin/targets/ath79/generic"

FACTORY="${OUTDIR}/openwrt-ath79-generic-fon_fon2415-squashfs-factory.bin"
SYSUPGRADE="${OUTDIR}/openwrt-ath79-generic-fon_fon2415-squashfs-sysupgrade.bin"

if [ -f "${FACTORY}" ]; then
    cp "${FACTORY}" "${SCRIPT_DIR}/fon2415.image"
    echo "      ✓ fon2415.image aangemaakt (factory, voor U-Boot 'run lf')"
    ls -lh "${SCRIPT_DIR}/fon2415.image"
else
    echo "      ✗ factory.bin niet gevonden — controleer build.log"
    exit 1
fi

if [ -f "${SYSUPGRADE}" ]; then
    cp "${SYSUPGRADE}" "${SCRIPT_DIR}/fon2415-sysupgrade.bin"
    echo "      ✓ fon2415-sysupgrade.bin aangemaakt"
fi

echo ""
echo "=== Build geslaagd! ==="
echo ""
echo "Stappen om te flashen via U-Boot:"
echo ""
echo "  1. Kopieer fon2415.image naar je TFTP-server:"
echo "     cp fon2415.image /var/lib/tftpboot/"
echo ""
echo "  2. TFTP-server starten (als dat nog niet draait):"
echo "     sudo systemctl start tftpd-hpa"
echo "     # of: sudo python3 -m tftpy.TftpServer /var/lib/tftpboot"
echo ""
echo "  3. In U-Boot (via UART, 115200 baud):"
echo "     setenv ipaddr 192.168.178.200"
echo "     setenv serverip 192.168.178.131"
echo "     ping 192.168.178.131         # moet 'alive' geven"
echo "     run lf                       # download + flash"
echo "     reset                        # herstart naar OpenWrt"
echo ""
echo "  4. Na herstart: http://192.168.1.1 (LuCI), wachtwoord leeg."
echo ""
