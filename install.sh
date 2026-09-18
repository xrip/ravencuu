#!/usr/bin/env bash
# raven-cu-unlock - one-command installer for the Raven Ridge 11-CU unlock.
#
# Patches the amdgpu module of the RUNNING Ubuntu kernel so it clears the
# harvested CU bits in CC_GC_SHADER_ARRAY_CONFIG during driver init
# (module param raven_cu_count, default 8 = stock no-op, strictly gated
# to PCI 1002:15dd subsystem 1458:d000).
#
#   install:   curl -fsSL https://github.com/xrip/ravencuu/releases/latest/download/install.sh | sudo bash
#   count 10:  ... | sudo bash -s -- --count 10
#   uninstall: ... | sudo bash -s -- --uninstall
#   local:     sudo bash linux/install.sh [--count N | --uninstall | --patch FILE]
#
# A kernel update replaces the module - re-run the installer afterwards.
set -euo pipefail

COUNT="${RAVEN_CU_COUNT:-11}"
MODE="install"
PATCH_FILE=""

KREL="$(uname -r)"
MODDIR="/lib/modules/$KREL"
MODDEST="$MODDIR/updates/extra/amdgpu.ko"
MODPROBE_CONF="/etc/modprobe.d/raven-cu-unlock.conf"

die() { echo "ERROR: $*" >&2; exit 1; }
log() { echo "[*] $*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --count)    COUNT="$2"; shift 2 ;;
        --uninstall) MODE="uninstall"; shift ;;
        --patch)    PATCH_FILE="$2"; shift 2 ;;
        --help|-h)
            grep -E '^#   ' "$0" | sed 's/^#//'
            exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

[ "$(id -u)" -eq 0 ] || die "run as root: sudo bash install.sh"
command -v apt-get >/dev/null || die "Debian/Ubuntu only (apt not found)"
case "$COUNT" in 8|9|10|11) ;; *) die "--count must be 8 (stock), 9, 10 or 11" ;; esac

if ! lspci -nn 2>/dev/null | grep -qi '1002:15dd'; then
    log "WARNING: no PCI 1002:15dd (Raven iGPU) visible - this unlock does nothing here"
fi
case "$KREL" in
    6.8.*) ;;
    *) log "WARNING: patch was validated on 6.8.x; running $KREL may not apply" ;;
esac

do_uninstall() {
    log "uninstalling (kernel $KREL)"
    rm -f "$MODDEST" "$MODPROBE_CONF"
    depmod -a "$KREL"
    update-initramfs -u
    log "done - next boot is stock"
}

do_install() {
    local work buildsrc
    work="$(mktemp -d /tmp/raven-cu-unlock.XXXXXX)"
    trap 'rm -rf "$work"' EXIT

    log "installing build dependencies (this can take a minute)"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq "linux-headers-$KREL" build-essential bc bison flex \
        libelf-dev libssl-dev dpkg-dev >/dev/null

    log "enabling deb-src (needed for apt-get source)"
    local srcs=0 f
    for f in /etc/apt/sources.list.d/*.sources; do
        [ -f "$f" ] || continue
        if grep -q '^Types: deb deb-src' "$f"; then srcs=1; break; fi
        if [ "$srcs" -eq 0 ] && grep -q '^Types: deb$' "$f"; then
            cp -a "$f" "$f.pre-cu-unlock"
            sed -i 's/^Types: deb$/Types: deb deb-src/' "$f"
            srcs=1
        fi
    done
    if [ "$srcs" -eq 0 ] && [ -f /etc/apt/sources.list ] && \
       grep -q '^deb ' /etc/apt/sources.list && ! grep -q '^deb-src ' /etc/apt/sources.list; then
        cp -a /etc/apt/sources.list /etc/apt/sources.list.pre-cu-unlock
        sed -n 's/^deb /deb-src /p' /etc/apt/sources.list.pre-cu-unlock >> /etc/apt/sources.list
        srcs=1
    fi
    [ "$srcs" -eq 1 ] || die "could not enable deb-src - enable it manually and re-run"
    apt-get update -qq

    log "fetching kernel source for $KREL (several minutes on first run)"
    ( cd "$work" && apt-get source "linux-image-unsigned-$KREL" ) \
        || die "apt-get source failed - check deb-src and network"
    buildsrc="$(find "$work" -maxdepth 1 -type d -name 'linux-*' | head -n1)"
    [ -n "$buildsrc" ] || die "kernel source not found after apt-get source"
    cd "$buildsrc"

    log "applying the CU-unlock patch"
    if [ -n "$PATCH_FILE" ]; then
        cp "$PATCH_FILE" "$work/raven-gfx9-cu-unlock.patch"
    else
        cat > "$work/raven-gfx9-cu-unlock.patch" <<'RAVEN_PATCH_EOF'
diff --git a/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c b/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c
--- a/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c
+++ b/drivers/gpu/drm/amd/amdgpu/gfx_v9_0.c
@@ -64,0 +65,8 @@
+#define RAVEN_CU_UNLOCK_DEVICE_ID             0x15dd
+#define RAVEN_CU_UNLOCK_SUBSYSTEM_VENDOR     0x1458
+#define RAVEN_CU_UNLOCK_SUBSYSTEM_DEVICE     0xd000
+
+static int raven_cu_count = 8;
+module_param_named(raven_cu_count, raven_cu_count, int, 0444);
+MODULE_PARM_DESC(raven_cu_count,
+	"Total Raven CU count to test (8-11)");
@@ -7251 +7259 @@
-	u32 mask, bitmap, ao_bitmap, ao_cu_mask = 0;
+	u32 mask, bitmap, ao_bitmap, ao_cu_mask = 0, cc_config, unlock_mask;
@@ -7276,2 +7284,14 @@
 				adev, disable_masks[i * adev->gfx.config.max_sh_per_se + j]);
+			if (raven_cu_count > 8 && raven_cu_count <= 11 &&
+				adev->pdev->device == RAVEN_CU_UNLOCK_DEVICE_ID &&
+				adev->pdev->subsystem_vendor == RAVEN_CU_UNLOCK_SUBSYSTEM_VENDOR &&
+				adev->pdev->subsystem_device == RAVEN_CU_UNLOCK_SUBSYSTEM_DEVICE) {
+				unlock_mask = GENMASK(raven_cu_count - 1, 8);
+				cc_config = RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
+				cc_config &= ~(unlock_mask <<
+					CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT);
+				WREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG, cc_config);
+				dev_info(adev->dev, "Raven CU test: count %d, mask 0x%x\n",
+					 raven_cu_count, unlock_mask);
+			}
 			bitmap = gfx_v9_0_get_cu_active_bitmap(adev);
RAVEN_PATCH_EOF
    fi
    if patch -p1 --dry-run --force -i "$work/raven-gfx9-cu-unlock.patch" >/dev/null 2>&1; then
        patch -p1 --force -i "$work/raven-gfx9-cu-unlock.patch" >/dev/null
    elif patch -p1 -R --dry-run -i "$work/raven-gfx9-cu-unlock.patch" >/dev/null 2>&1; then
        log "patch already applied - rebuilding"
    else
        die "patch does not apply to this source tree (see $work)"
    fi

    log "preparing the module build (a few minutes)"
    cp "/boot/config-$KREL" .config
    scripts/config -d SYSTEM_TRUSTED_KEYS -d SYSTEM_REVOCATION_KEYS
    make olddefconfig >/dev/null
    make -j"$(nproc)" modules_prepare >/dev/null
    if [ -f "/usr/lib/modules/$KREL/build/Module.symvers" ]; then
        cp "/usr/lib/modules/$KREL/build/Module.symvers" Module.symvers
    fi

    log "building amdgpu"
    make -j"$(nproc)" M=drivers/gpu/drm/amd >/dev/null
    local ko
    ko="$(find drivers/gpu/drm/amd -name 'amdgpu.ko' | head -n1)"
    [ -n "$ko" ] || die "amdgpu.ko not produced - build failed (see $work)"

    log "installing module + modprobe option (count $COUNT)"
    install -D -m 0644 "$ko" "$MODDEST"
    depmod -a "$KREL"
    printf 'options amdgpu raven_cu_count=%d\n' "$COUNT" > "$MODPROBE_CONF"
    update-initramfs -u

    if ! modinfo "$MODDEST" 2>/dev/null | grep -q "vermagic.*$KREL"; then
        log "WARNING: vermagic mismatch - the module may not load; install the"
        log "gcc version the kernel was built with and re-run"
    fi
    log "done - reboot, then verify:"
    log "  sudo dmesg | grep 'Raven CU test'   -> 'count 11, mask 0x700'"
    log "  clinfo | grep -i 'compute units'   -> 11"
}

if [ "$MODE" = "uninstall" ]; then do_uninstall; else do_install; fi
