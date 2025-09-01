#!/usr/bin/env bash
# End-to-end automation for RV32IMA Linux on QEMU using this repo
# - Builds (or reuses) a riscv32 musl cross toolchain
# - Builds a size-optimized Linux Image from ./linux
# - Clones and builds BusyBox statically, creates an initramfs
# - Prepares and builds the mgpu kernel module
# - Boots QEMU with serial console on your terminal

set -euo pipefail

# ----- Paths -----
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LINUX_DIR="${ROOT_DIR}/linux"
TOOLCHAIN_DIR="${HOME}/opt/riscv32ima-linux-musl"
CROSS_PREFIX="${TOOLCHAIN_DIR}/bin/riscv32-unknown-linux-musl-"
BUSYBOX_DIR="${ROOT_DIR}/third_party/busybox"
OUT_DIR="${ROOT_DIR}/out"
BUSYBOX_ROOT="${OUT_DIR}/busybox_root"
INITRAMFS_DIR="${OUT_DIR}/initramfs"
INITRAMFS_IMG="${OUT_DIR}/rootfs.cpio.gz"
JOBS="${JOBS:-$(nproc)}"

# ----- Helpers -----
need_cmd() { command -v "$1" >/dev/null 2>&1 || { echo "Missing command: $1"; exit 1; }; }
log() { echo -e "\033[1;34m[rv32]\033[0m $*"; }

check_host_prereqs() {
  for c in git make gcc g++ python3 cpio gzip sed awk patch; do need_cmd "$c"; done
  # QEMU may be qemu-system-riscv32 or bundled in qemu-system-misc
  if command -v qemu-system-riscv32 >/dev/null 2>&1; then :; 
  elif command -v qemu-system-misc >/dev/null 2>&1; then :; 
  else echo "Missing qemu-system-riscv32 (or qemu-system-misc)"; exit 1; fi
  # Optional but recommended for building device nodes in initramfs without sudo
  if ! command -v fakeroot >/dev/null 2>&1; then
    log "fakeroot not found; will attempt mknod without fakeroot (may require sudo)."
  fi
}

ensure_toolchain() {
  if [[ -x "${CROSS_PREFIX}gcc" ]]; then
    log "Using existing toolchain: ${CROSS_PREFIX}gcc ($("${CROSS_PREFIX}gcc" --version | head -n1))"
    return
  fi
  log "Building riscv32 musl toolchain into ${TOOLCHAIN_DIR}"
  pushd "${ROOT_DIR}/riscv-gnu-toolchain" >/dev/null
  # Build just enough for musl GCC stage2 (skips gdb dependency)
  PYTHON=python3 make -j"${JOBS}" \
    INSTALL_DIR="${TOOLCHAIN_DIR}" \
    stamps/build-gcc-musl-stage2
  popd >/dev/null
  if [[ ! -x "${CROSS_PREFIX}gcc" ]]; then
    echo "Failed to produce ${CROSS_PREFIX}gcc"; exit 1;
  fi
}

configure_kernel_min_size() {
  # Use scripts/config if present for non-interactive .config tweaks
  local sc="${LINUX_DIR}/scripts/config"
  if [[ -x "${sc}" ]]; then
    pushd "${LINUX_DIR}" >/dev/null
    "${sc}" --file .config \
      --enable CC_OPTIMIZE_FOR_SIZE \
      --enable EMBEDDED \
      --enable DEVTMPFS \
      --enable DEVTMPFS_MOUNT \
      --enable SERIAL_8250 \
      --enable SERIAL_8250_CONSOLE \
      --disable DEBUG_KERNEL \
      --disable DEBUG_INFO || true
    make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" olddefconfig
    popd >/dev/null
  fi
}

build_kernel() {
  log "Building Linux kernel Image (rv32, size-optimized)"
  pushd "${LINUX_DIR}" >/dev/null
  if [[ ! -f .config ]]; then
    make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" defconfig
  fi
  configure_kernel_min_size
  make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" -j"${JOBS}" Image
  popd >/dev/null
  [[ -f "${LINUX_DIR}/arch/riscv/boot/Image" ]] || { echo "Kernel Image missing"; exit 1; }
}

build_busybox() {
  log "Building BusyBox (static, minimal)"
  mkdir -p "${OUT_DIR}"
  if [[ ! -d "${BUSYBOX_DIR}/.git" ]]; then
    mkdir -p "${BUSYBOX_DIR%/*}"
    git clone --depth=1 https://github.com/mirror/busybox "${BUSYBOX_DIR}"
  fi
  pushd "${BUSYBOX_DIR}" >/dev/null
  # Start from a clean tree
  make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" distclean || true
  # Provide a minimal feature fragment
  local BB_FRAG="${OUT_DIR}/busybox_minimal.config"
  local KCONF="${OUT_DIR}/busybox.config"
  cat > "${BB_FRAG}" <<'CFG'
CONFIG_BUSYBOX=y
CONFIG_STATIC=y
# Large File Support (needed with musl on 32-bit where off_t is 64-bit)
CONFIG_LFS=y
# Shell
CONFIG_SH_IS_ASH=y
CONFIG_ASH=y
CONFIG_ASH_OPTIMIZE_FOR_SIZE=y
CONFIG_ASH_ECHO=y
CONFIG_FEATURE_SH_STANDALONE=y
CONFIG_CTTYHACK=y
# Core userland
CONFIG_LS=y
CONFIG_ECHO=y
CONFIG_CAT=y
CONFIG_DMESG=y
CONFIG_MOUNT=y
CONFIG_UMOUNT=y
CONFIG_MKDIR=y
CONFIG_MKNOD=y
CONFIG_UNAME=y
CONFIG_SLEEP=y
CONFIG_TRUE=y
CONFIG_FALSE=y
# Drop problematic applets
CONFIG_HWCLOCK=n
CONFIG_TC=n
# Disable syslog/logger to avoid prompts
CONFIG_SYSLOGD=n
CONFIG_KLOGD=n
CONFIG_LOGGER=n
CONFIG_LOGREAD=n
# No networking/tools by default to keep size small
CONFIG_NETWORKING=n
CFG
  # Ensure off_t/uoff_t agree even if config toggles get ignored
  local BB_EXTRA_CFLAGS="-D_FILE_OFFSET_BITS=64"
  # Use a dedicated BusyBox config and make Kconfig fully non-interactive
  export KCONFIG_CONFIG="${KCONF}"
  export KCONFIG_ALLCONFIG="${BB_FRAG}"
  # Generate .config from fragment (unspecified = 'n')
  make -s ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" allnoconfig
  # Fill in any new symbols with defaults without prompting
  yes "" | make -s ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" oldconfig
  # Sanity check required options
  for sym in CONFIG_LFS CONFIG_ASH CONFIG_SH_IS_ASH CONFIG_CTTYHACK CONFIG_FEATURE_SH_STANDALONE; do
    if ! grep -q "^${sym}=y" "${KCONF}"; then
      echo "BusyBox config missing ${sym}. Aborting." >&2
      echo "Active config is at ${KCONF}" >&2
      exit 1
    fi
  done
  # Build and install using the fixed config
  make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" EXTRA_CFLAGS="${BB_EXTRA_CFLAGS}" -j"${JOBS}"
  rm -rf "${BUSYBOX_ROOT}"
  make ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" EXTRA_CFLAGS="${BB_EXTRA_CFLAGS}" CONFIG_PREFIX="${BUSYBOX_ROOT}" install
  # Verify sh and cttyhack exist in rootfs
  if [[ ! -x "${BUSYBOX_ROOT}/bin/sh" ]]; then
    echo "BusyBox /bin/sh not found after install. Check ${KCONF}" >&2
    exit 1
  fi
  if [[ ! -x "${BUSYBOX_ROOT}/bin/cttyhack" ]]; then
    echo "BusyBox /bin/cttyhack not found after install. Check ${KCONF}" >&2
    exit 1
  fi
  popd >/dev/null
}

make_initramfs() {
  log "Creating initramfs"
  rm -rf "${INITRAMFS_DIR}"
  mkdir -p "${INITRAMFS_DIR}"
  # Copy BusyBox rootfs
  rsync -a "${BUSYBOX_ROOT}/" "${INITRAMFS_DIR}/"
  # Create minimal dirs and /init script
  mkdir -p "${INITRAMFS_DIR}/"{proc,sys,dev,root,tmp,etc}
  # Minimal passwd/group so whoami shows root
  cat > "${INITRAMFS_DIR}/etc/passwd" <<'P'
root:x:0:0:root:/root:/bin/sh
P
  cat > "${INITRAMFS_DIR}/etc/group" <<'G'
root:x:0:
tty:x:5:
G
  cat > "${INITRAMFS_DIR}/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev || true
export HOME=/root
export TERM=linux
export PS1='[root@rv32]# '
# Ensure controlling tty and drop into root shell
if [ -x /bin/cttyhack ]; then
  exec /bin/cttyhack /bin/sh -l
else
  exec /bin/sh -l
fi
EOF
  chmod +x "${INITRAMFS_DIR}/init"
  # Device nodes: create under fakeroot if available
  create_nodes() {
    mkdir -p "${INITRAMFS_DIR}/dev"
    mknod -m 600 "${INITRAMFS_DIR}/dev/console" c 5 1 || true
    mknod -m 666 "${INITRAMFS_DIR}/dev/null" c 1 3 || true
  }
  if command -v fakeroot >/dev/null 2>&1; then
    fakeroot bash -eu -c "$(declare -f create_nodes); create_nodes"
  else
    create_nodes || true
  fi
  # Pack cpio.gz
  pushd "${INITRAMFS_DIR}" >/dev/null
  if command -v fakeroot >/dev/null 2>&1; then
    fakeroot bash -eu -c 'find . | cpio -H newc -o | gzip -9 > "${INITRAMFS_IMG}"'
  else
    find . | cpio -H newc -o | gzip -9 > "${INITRAMFS_IMG}"
  fi
  popd >/dev/null
  [[ -s "${INITRAMFS_IMG}" ]] || { echo "Failed to create initramfs"; exit 1; }
}

build_module() {
  log "Preparing kernel headers and building mgpu module"
  make -C "${LINUX_DIR}" ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" -j"${JOBS}" modules_prepare
  make -C "${LINUX_DIR}" ARCH=riscv CROSS_COMPILE="${CROSS_PREFIX}" \
    M="${ROOT_DIR}/kernel/drivers/gpu/mgpu" -j"${JOBS}" V=1 modules || true
  if [[ -f "${ROOT_DIR}/kernel/drivers/gpu/mgpu/mgpu.ko" ]]; then
    log "Built: ${ROOT_DIR}/kernel/drivers/gpu/mgpu/mgpu.ko"
  else
    log "mgpu.ko not produced (ok on QEMU virt without hardware)."
  fi
}

run_qemu() {
  log "Booting QEMU (serial console)"
  local QEMU_BIN="qemu-system-riscv32"
  if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
    QEMU_BIN="qemu-system-misc"
  fi
  command -v "$QEMU_BIN" >/dev/null 2>&1 || { echo "QEMU binary not found"; exit 1; }
  [[ -s "${LINUX_DIR}/arch/riscv/boot/Image" ]] || { echo "Kernel Image missing or empty"; exit 1; }
  [[ -s "${INITRAMFS_IMG}" ]] || { echo "Initramfs missing or empty"; exit 1; }
  local QEMU_CMD=(
    "$QEMU_BIN"
    -machine virt -m 512M -bios default
    -kernel "${LINUX_DIR}/arch/riscv/boot/Image"
    -initrd "${INITRAMFS_IMG}"
    -append "console=ttyS0,115200 earlycon init=/init"
    -nographic
  )
  log "Launching: ${QEMU_CMD[*]}"
  "${QEMU_CMD[@]}"
}

usage() {
  cat <<USAGE
Usage: $(basename "$0") [all|toolchain|kernel|busybox|initramfs|module|run|clean]
Defaults to 'all'. Artifacts in ${OUT_DIR}.
USAGE
}

clean() {
  rm -rf "${OUT_DIR}"
}

main() {
  check_host_prereqs
  mkdir -p "${OUT_DIR}"
  local target="${1:-all}"
  case "$target" in
    toolchain) ensure_toolchain ;;
    kernel)    ensure_toolchain; build_kernel ;;
    busybox)   ensure_toolchain; build_busybox ;;
    initramfs) ensure_toolchain; build_busybox; make_initramfs ;;
    module)    ensure_toolchain; build_kernel; build_module ;;
    run)       ensure_toolchain; [[ -f "${LINUX_DIR}/arch/riscv/boot/Image" ]] || build_kernel; [[ -f "${INITRAMFS_IMG}" ]] || { build_busybox; make_initramfs; }; run_qemu ;;
    clean)     clean ;;
    all)       ensure_toolchain; build_kernel; build_busybox; make_initramfs; build_module; run_qemu ;;
    *) usage; exit 1 ;;
  esac
}

main "$@"
