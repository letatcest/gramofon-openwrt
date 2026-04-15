#!/usr/bin/env bash
# build_openwrt.sh — Build an OpenWrt image for the Fon FON2415 (Gramofon)
#
# Requirements:
#   git, gcc, g++, make, libncurses-dev, zlib1g-dev, gawk, flex, bison,
#   unzip, python3, rsync, wget
#
# Usage:
#   chmod +x build_openwrt.sh
#   ./build_openwrt.sh
#
# Output:
#   fon2415.image         — factory image for first-time flashing via U-Boot
#   fon2415-sysupgrade.bin — sysupgrade image for subsequent updates

set -euo pipefail
trap 'echo ""; echo "Build failed — check build.log for details."' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OPENWRT_DIR="${SCRIPT_DIR}/openwrt"
OPENWRT_TAG="v23.05.4"

echo "=== Gramofon FON2415 — OpenWrt build script ==="
echo "    OpenWrt version : ${OPENWRT_TAG}"
echo "    Working dir     : ${OPENWRT_DIR}"
echo ""

# ── 1. Clone source ──────────────────────────────────────────────────────────
if [ ! -d "${OPENWRT_DIR}" ]; then
    echo "[1/7] Cloning OpenWrt (${OPENWRT_TAG})..."
    git clone --depth 1 --branch "${OPENWRT_TAG}" \
        https://git.openwrt.org/openwrt/openwrt.git "${OPENWRT_DIR}"
else
    echo "[1/7] OpenWrt directory already exists, skipping clone."
fi

cd "${OPENWRT_DIR}"

# ── 2. Copy DTS file ─────────────────────────────────────────────────────────
echo "[2/7] Copying DTS file..."
cp "${SCRIPT_DIR}/fon2415.dts" \
   "${OPENWRT_DIR}/target/linux/ath79/dts/qca9341_fon_fon2415.dts"
echo "      → target/linux/ath79/dts/qca9341_fon_fon2415.dts"

# ── 3. Add device profile to generic.mk ─────────────────────────────────────
echo "[3/7] Adding device profile to generic.mk..."
GENERIC_MK="${OPENWRT_DIR}/target/linux/ath79/image/generic.mk"

if grep -q "fon_fon2415" "${GENERIC_MK}"; then
    echo "      → Already present, skipping."
else
    cat "${SCRIPT_DIR}/fon2415.mk" >> "${GENERIC_MK}"
    echo "      → Added to generic.mk"
fi

# ── 4. Update feeds ──────────────────────────────────────────────────────────
echo "[4/7] Updating and installing feeds..."
./scripts/feeds update -a
./scripts/feeds install -a

# ── 5. Create build config ───────────────────────────────────────────────────
echo "[5/7] Creating build configuration..."

# Step 1: base target
printf 'CONFIG_TARGET_ath79=y\nCONFIG_TARGET_ath79_generic=y\n' > .config
make defconfig

# Step 2: add device and packages after defconfig
printf '%s\n' \
    'CONFIG_TARGET_ath79_generic_DEVICE_fon_fon2415=y' \
    'CONFIG_PACKAGE_kmod-ath9k=y' \
    'CONFIG_PACKAGE_luci=y' \
    'CONFIG_PACKAGE_luci-ssl=y' \
    'CONFIG_PACKAGE_openssh-sftp-server=y' \
    'CONFIG_TARGET_ROOTFS_SQUASHFS=y' \
    >> .config

# Step 3: expand new symbols
make defconfig

# Verify device is selected
if grep -q 'CONFIG_TARGET_ath79_generic_DEVICE_fon_fon2415=y' .config; then
    echo "      OK: fon_fon2415 selected"
else
    echo "      ERROR: device not selected in .config — aborting"
    exit 1
fi

# ── 6. Build ─────────────────────────────────────────────────────────────────
JOBS=$(nproc)
echo "[6/7] Building with ${JOBS} cores..."
echo "      This takes 30-90 minutes on first run."
echo ""

# Open FD 8: OpenWrt writes timing output via >&8
exec 8>/dev/null
make -j"${JOBS}" V=s 2>&1 | tee "${SCRIPT_DIR}/build.log"

# ── 7. Copy output ───────────────────────────────────────────────────────────
echo ""
echo "[7/7] Copying output to ${SCRIPT_DIR}..."
OUTDIR="${OPENWRT_DIR}/bin/targets/ath79/generic"

FACTORY="${OUTDIR}/openwrt-ath79-generic-fon_fon2415-squashfs-factory.bin"
SYSUPGRADE="${OUTDIR}/openwrt-ath79-generic-fon_fon2415-squashfs-sysupgrade.bin"

if [ -f "${FACTORY}" ]; then
    cp "${FACTORY}" "${SCRIPT_DIR}/fon2415.image"
    echo "      ✓ fon2415.image created (factory image for U-Boot 'run lf')"
    ls -lh "${SCRIPT_DIR}/fon2415.image"
else
    echo "      ✗ factory.bin not found — check build.log"
    exit 1
fi

if [ -f "${SYSUPGRADE}" ]; then
    cp "${SYSUPGRADE}" "${SCRIPT_DIR}/fon2415-sysupgrade.bin"
    echo "      ✓ fon2415-sysupgrade.bin created"
fi

echo ""
echo "=== Build complete! ==="
echo ""
echo "Flash via U-Boot (UART, 115200 baud):"
echo ""
echo "  1. Copy fon2415.image to your TFTP server root:"
echo "     cp fon2415.image /var/lib/tftpboot/"
echo ""
echo "  2. Start TFTP server if not running:"
echo "     sudo systemctl start tftpd-hpa"
echo ""
echo "  3. In U-Boot prompt:"
echo "     setenv ipaddr 192.168.178.200"
echo "     setenv serverip 192.168.178.131"
echo "     ping 192.168.178.131         # should print 'alive'"
echo "     run lf                       # download and flash"
echo "     reset                        # reboot into OpenWrt"
echo ""
echo "  4. After reboot: http://192.168.1.1 (LuCI), password empty."
echo ""
