#!/usr/bin/env bash
# aliensense-jetson-drivers builder
#
# Applies the lean source/ delta of this repo onto a pristine NVIDIA Jetson
# Linux (L4T) source tree and builds the out-of-tree modules and DTBOs.
# Linux-only; run inside the build container (see docker/Dockerfile).
#
# Stages (no stage flags = prepare + modules + dtbs + collect):
#   -P   pristine: re-extract the OOT and overlay trees before the delta
#        (release builds — a cached tree may carry files a past build dropped)
#   -p   prepare sources (download/extract public_sources, rsync delta in)
#   -m   build OOT modules
#   -d   build DTBs/DTBOs
#   -t   modules_install into a staging dir and produce oot_modules.tar.xz
#   -c   collect artifacts into OUTPUT_DIR
#   -a   archive: the bench package (modules, DTBOs, installer, source) — needs -t
#   -b   deb: nxs-jetson_<l4t>_arm64.deb, the released package — needs -m and -d
#
# Layout it produces:
#   $OUTPUT_DIR/dtbs/*.dtbo + base DTB
#   $OUTPUT_DIR/modules/*.ko
#   $OUTPUT_DIR/oot_modules.tar.xz                     (with -t; bench/reference)
#   $OUTPUT_DIR/aliensense-camera-<l4t>-<describe>.tar.xz  (with -a; the bench package)
#   $OUTPUT_DIR/nxs-jetson_<l4t>_arm64.deb              (with -b; the release)
set -euo pipefail

# ---------------------------------------------------------------------------
# Per-branch configuration — l4t-r39.2.1 (JetPack 7.2.1)
# ---------------------------------------------------------------------------
L4T_VERSION="39.2.1"
L4T_RELEASE_DIR="r39_release_v2.1"
KERNEL_SRC_SUBDIR="kernel/kernel-noble"
KERNEL_NAME="noble"
EXPECTED_UNAME="6.8.12-1021-tegra"              # the vermagic MANIFEST.md records
# r39.x builds a -nv and a -nv-super tree per module; a board boots the one
# declaring its root compatible (DEPLOY.md §2). Both serve the offline gate.
BASE_DTBS="tegra234-p3768-0000+p3767-0005-nv.dtb tegra234-p3768-0000+p3767-0005-nv-super.dtb"
DTBS_OUT_SUBDIR="build/nvidia-public/devicetree/generic-dtbs"  # r36.x: kernel-devicetree/generic-dts/dtbs
EXPECTED_CONTAINER_CODENAME="noble"             # kernel 6.8 needs GCC >= 12; a jammy container fails mid-build
CONTAINER_IMAGE_HINT="jetson-build-r3921"
PUBLIC_SOURCES_URL="https://developer.nvidia.com/downloads/embedded/l4t/${L4T_RELEASE_DIR}/sources/public_sources.tbz2"
# Verified download 2026-09-17 (see MANIFEST.md); empty = warn only.
PUBLIC_SOURCES_SHA256="8346f0371649a92c42293b2d4a7bf3d69e4dd1de23009e1463ea910994a81371"
# Trees the delta overlays; -P restores them from the extracted tarballs.
PRISTINE_TREES="nvidia-oot hardware"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${WORK_DIR:-/build/work-l4t-r${L4T_VERSION}}"
SRC_DIR="${SRC_DIR:-${WORK_DIR}/Linux_for_Tegra/source}"
OUTPUT_DIR="${OUTPUT_DIR:-${REPO_DIR}/output}"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
JOBS="${JOBS:-$(nproc)}"
MOD_STAGE="${WORK_DIR}/mod-stage"
DEB_PACKAGE="nxs-jetson"
# A respin of one release bumps the revision; the file name and the tag stay.
DEB_REVISION="${DEB_REVISION:-1}"
DEB_MAINTAINER="${DEB_MAINTAINER:-Aliensense <nxs@aliensense.com>}"

log() { printf '\n== %s ==\n' "$*"; }

# The source tarball (in SRC_DIR) that carries a top-level tree.
tarball_for() {
    local tree="$1" t
    for t in kernel_oot_modules_src.tbz2 kernel_src.tbz2 \
             nvidia_kernel_display_driver_source.tbz2 \
             kernel_display_driver_source.tbz2; do
        [ -f "${SRC_DIR}/${t}" ] || continue
        if tar -tjf "${SRC_DIR}/${t}" "${tree}" >/dev/null 2>&1; then
            echo "${t}"; return 0
        fi
    done
    return 1
}

restore_pristine() {
    log "pristine: re-extracting ${PRISTINE_TREES}"
    local tree tarball
    for tree in ${PRISTINE_TREES}; do
        tarball="$(tarball_for "${tree}")" || {
            echo "FATAL: no source tarball in ${SRC_DIR} carries ${tree}/" >&2; exit 1; }
        rm -rf "${SRC_DIR:?}/${tree}"
        ( cd "${SRC_DIR}" && tar -xjf "${tarball}" "${tree}" )
        echo "  ${tree}/ <- ${tarball}"
    done
}

prepare_sources() {
    log "prepare_sources (L4T ${L4T_VERSION})"
    if [ ! -d "${SRC_DIR}" ]; then
        mkdir -p "${WORK_DIR}"
        local tarball="${WORK_DIR}/public_sources.tbz2"
        if [ ! -f "${tarball}" ]; then
            log "downloading ${PUBLIC_SOURCES_URL}"
            curl -fL --retry 3 -o "${tarball}" "${PUBLIC_SOURCES_URL}"
        fi
        local sha
        sha="$(sha256sum "${tarball}" | awk '{print $1}')"
        if [ -n "${PUBLIC_SOURCES_SHA256}" ]; then
            [ "${sha}" = "${PUBLIC_SOURCES_SHA256}" ] || {
                echo "FATAL: public_sources.tbz2 sha256 mismatch: ${sha}" >&2; exit 1; }
        else
            echo "WARNING: PUBLIC_SOURCES_SHA256 unpinned. Downloaded sha256: ${sha}" >&2
            echo "         Pin it in build/builder.sh and MANIFEST.md." >&2
        fi
        log "extracting public_sources.tbz2"
        tar -xjf "${tarball}" -C "${WORK_DIR}"      # -> Linux_for_Tegra/source/*.tbz2
        ( cd "${SRC_DIR}"
          tar -xjf kernel_src.tbz2
          tar -xjf kernel_oot_modules_src.tbz2
          if [ -f kernel_display_driver_source.tbz2 ]; then
              tar -xjf kernel_display_driver_source.tbz2
          elif [ -f nvidia_kernel_display_driver_source.tbz2 ]; then
              tar -xjf nvidia_kernel_display_driver_source.tbz2
          fi
          # r38+: the top-level 'modules' target also builds nvidia-gpu-display
          # from a separate tarball; extract when the release ships it.
          if [ -f nvidia_unified_gpu_display_driver_source.tbz2 ]; then
              tar -xjf nvidia_unified_gpu_display_driver_source.tbz2
          fi )
    elif [ "${DO_PRISTINE}" = 1 ]; then
        restore_pristine
    fi
    # One-time kernel configure+build: NVIDIA's OOT conftest/module flow
    # compiles against generated headers and checks Module.symvers, both of
    # which exist only in a built kernel tree. Cached in WORK_DIR afterward.
    if [ ! -f "${SRC_DIR}/${KERNEL_SRC_SUBDIR}/Module.symvers" ]; then
        log "one-time kernel build (generates headers + Module.symvers)"
        export CROSS_COMPILE
        make -C "${SRC_DIR}/kernel" -j"${JOBS}"
    fi
    log "applying repo delta onto ${SRC_DIR}"
    # Purge our files from the tree first: a driver/DTS removed or renamed on
    # this branch must not survive in the cached work tree from an earlier build.
    find "${SRC_DIR}/nvidia-oot/drivers/media/i2c" \
         "${SRC_DIR}/hardware/nvidia/t23x/nv-public/overlay" \
         -name '*aliensense*' -type f -delete 2>/dev/null || true
    rsync -av "${REPO_DIR}/source/" "${SRC_DIR}/"
}

build_modules() {
    log "make modules"
    export CROSS_COMPILE
    export KERNEL_HEADERS="${SRC_DIR}/${KERNEL_SRC_SUBDIR}"
    [ -n "${KERNEL_NAME}" ] && export kernel_name="${KERNEL_NAME}"
    make -C "${SRC_DIR}" -j"${JOBS}" modules
}

build_dtbs() {
    log "make dtbs"
    rm -f "${SRC_DIR}/${DTBS_OUT_SUBDIR}"/*aliensense*.dtbo
    export CROSS_COMPILE
    export KERNEL_HEADERS="${SRC_DIR}/${KERNEL_SRC_SUBDIR}"
    [ -n "${KERNEL_NAME}" ] && export kernel_name="${KERNEL_NAME}"
    make -C "${SRC_DIR}" -j"${JOBS}" dtbs
}

tar_modules() {
    log "modules_install -> oot_modules.tar.xz"
    rm -rf "${MOD_STAGE}"; mkdir -p "${MOD_STAGE}"
    export CROSS_COMPILE
    export KERNEL_HEADERS="${SRC_DIR}/${KERNEL_SRC_SUBDIR}"
    [ -n "${KERNEL_NAME}" ] && export kernel_name="${KERNEL_NAME}"
    make -C "${SRC_DIR}" INSTALL_MOD_PATH="${MOD_STAGE}" modules_install
    mkdir -p "${OUTPUT_DIR}"
    tar -C "${MOD_STAGE}/lib/modules" -cJf "${OUTPUT_DIR}/oot_modules.tar.xz" .
    echo "wrote ${OUTPUT_DIR}/oot_modules.tar.xz (bench/reference: the full OOT set; the deliverable is -a)"
}

# The modules the delta builds: one per .c under source/'s i2c directory.
delta_modules() {
    local src
    for src in "${REPO_DIR}"/source/nvidia-oot/drivers/media/i2c/*.c; do
        echo "$(basename "${src%.c}").ko"
    done
}

collect() {
    log "collect artifacts"
    rm -rf "${OUTPUT_DIR}/dtbs" "${OUTPUT_DIR}/modules"
    mkdir -p "${OUTPUT_DIR}/dtbs" "${OUTPUT_DIR}/modules"
    local dtbs_out="${SRC_DIR}/${DTBS_OUT_SUBDIR}"
    cp -v "${dtbs_out}"/*aliensense*.dtbo "${OUTPUT_DIR}/dtbs/"
    local base
    for base in ${BASE_DTBS}; do
        [ -f "${dtbs_out}/${base}" ] && cp -v "${dtbs_out}/${base}" "${OUTPUT_DIR}/dtbs/"
    done
    local m
    for m in $(delta_modules); do
        cp -v "${SRC_DIR}/nvidia-oot/drivers/media/i2c/${m}" "${OUTPUT_DIR}/modules/"
    done
    for m in "${OUTPUT_DIR}"/modules/*.ko; do
        printf '%s  vermagic: ' "$(basename "$m")"
        modinfo -F vermagic "$m" 2>/dev/null || echo "(modinfo unavailable)"
    done
    echo "expected target kernel: ${EXPECTED_UNAME}"
    [ -f "${OUTPUT_DIR}/oot_modules.tar.xz" ] || \
        echo "note: no oot_modules.tar.xz in output/ — the customer package needs -t then -a (RELEASING.md §3)"
}

archive() {
    log "archive: customer package"
    [ -d "${MOD_STAGE}/lib/modules/${EXPECTED_UNAME}" ] || {
        echo "FATAL: no modules_install staging at ${MOD_STAGE} — run stage -t first" >&2; exit 1; }
    local describe name pkg m ko
    describe="$(git -C "${REPO_DIR}" describe --tags --always --dirty 2>/dev/null || echo untagged)"
    name="aliensense-camera-${L4T_VERSION}-${describe}"
    # Assemble on the container's own filesystem: a bind-mounted OUTPUT_DIR
    # (Docker Desktop) reports files as changing while tar reads them.
    local stage_root="${WORK_DIR}/pkg"
    pkg="${stage_root}/${name}"
    rm -rf "${stage_root}"; mkdir -p "${pkg}/modules" "${pkg}/dtbs" "${pkg}/src"
    for m in $(delta_modules); do
        ko="$(find "${MOD_STAGE}/lib/modules/${EXPECTED_UNAME}/updates" -name "${m}" | head -1)"
        [ -n "${ko}" ] || { echo "FATAL: ${m} is not in the staging tree" >&2; exit 1; }
        cp "${ko}" "${pkg}/modules/"
    done
    cp "${SRC_DIR}/${DTBS_OUT_SUBDIR}"/*aliensense*.dtbo "${pkg}/dtbs/"
    install -m 0755 "${REPO_DIR}/build/install.sh" "${pkg}/install.sh"
    echo "${EXPECTED_UNAME}" > "${pkg}/KERNEL"
    echo "${L4T_VERSION}" > "${pkg}/L4T"
    # Corresponding source (GPL-2.0 §3(a)): what builds modules/, buildable
    # against the pinned public_sources release named in MANIFEST.md.
    rsync -a "${REPO_DIR}/source/" "${pkg}/src/source/"
    cp "${REPO_DIR}/MANIFEST.md" "${REPO_DIR}/LICENSE" "${REPO_DIR}/docs/BUILD.md" \
       "${REPO_DIR}/build/builder.sh" "${pkg}/src/"
    cat > "${pkg}/README.md" <<EOF
# Aliensense camera package — Jetson Linux ${L4T_VERSION} (${describe})

Kernel modules and device-tree overlays for the Aliensense GMSL camera
ports, built for kernel \`${EXPECTED_UNAME}\` over stock JetPack.

    orin\$ ./install.sh --check      verify checksums, the kernel and the L4T release
    orin\$ sudo ./install.sh         install modules + DTBOs (depmod)
    orin\$ sudo ./install.sh --rollback

Then write the boot entry, with \`nxs host overlay <port> --install --select\`
or by pasting the block from the Deployment Reference (DEPLOY.md §5), and
reboot. Camera control lives in userspace (the \`nxs\` tool and its
descriptor packs); these modules only make the ports appear.

\`src/\` is the corresponding source of \`modules/\` (GPL-2.0, see
src/LICENSE), rebuildable with src/builder.sh against the public
NVIDIA release named in src/MANIFEST.md.
EOF
    ( cd "${pkg}" && find modules dtbs src install.sh KERNEL L4T README.md -type f \
        | LC_ALL=C sort | xargs sha256sum > SHA256SUMS )
    mkdir -p "${OUTPUT_DIR}"
    tar -C "${stage_root}" -cJf "${stage_root}/${name}.tar.xz" "${name}"
    rm -rf "${OUTPUT_DIR:?}/${name}" "${OUTPUT_DIR}/${name}.tar.xz"
    cp -r "${pkg}" "${OUTPUT_DIR}/"
    cp "${stage_root}/${name}.tar.xz" "${OUTPUT_DIR}/"
    echo "wrote ${OUTPUT_DIR}/${name}.tar.xz"
    ( cd "${OUTPUT_DIR}" && sha256sum "${name}.tar.xz" )
    echo "modules:"; ( cd "${pkg}" && sha256sum modules/*.ko )
}

# The released package: the modules under the kernel's updates directory
# (the build tree's files, unsigned, so a build reproduces byte for byte),
# the fixed overlays under /boot/camera-dtbos (the universal ones are the
# host tool's to write), the corresponding source under /usr/src
# (GPL-2.0), depmod on install and removal. It refuses another Jetson
# Linux release at install. The boot entry is the host tool's
# (`nxs switch`), never the package's.
deb() {
    log "deb: ${DEB_PACKAGE}_${L4T_VERSION}_arm64.deb"
    command -v dpkg-deb >/dev/null || { echo "FATAL: dpkg-deb is not in the container" >&2; exit 1; }
    local root="${WORK_DIR}/deb/${DEB_PACKAGE}_${L4T_VERSION}_arm64" m ko dtbo
    local mods="lib/modules/${EXPECTED_UNAME}/updates/drivers/media/i2c"
    local src="usr/src/${DEB_PACKAGE}-${L4T_VERSION}" doc="usr/share/doc/${DEB_PACKAGE}"
    rm -rf "${WORK_DIR}/deb"
    mkdir -p "${root}/DEBIAN" "${root}/${mods}" "${root}/boot/camera-dtbos" "${root}/${src}/build" "${root}/${doc}"
    for m in $(delta_modules); do
        ko="${SRC_DIR}/nvidia-oot/drivers/media/i2c/${m}"
        [ -f "${ko}" ] || { echo "FATAL: ${m} is not in the build tree — run stage -m first" >&2; exit 1; }
        install -m 0644 "${ko}" "${root}/${mods}/"
    done
    for dtbo in "${SRC_DIR}/${DTBS_OUT_SUBDIR}"/*aliensense*.dtbo; do
        case "$(basename "${dtbo}")" in
            *universal*) ;;
            *) install -m 0644 "${dtbo}" "${root}/boot/camera-dtbos/" ;;
        esac
    done
    rsync -a "${REPO_DIR}/source/" "${root}/${src}/source/"
    rsync -a --exclude attic "${REPO_DIR}/patches/" "${root}/${src}/patches/"
    rsync -a "${REPO_DIR}/docker/" "${root}/${src}/docker/"
    install -m 0755 "${REPO_DIR}/build/builder.sh" "${root}/${src}/build/builder.sh"
    install -m 0644 "${REPO_DIR}/MANIFEST.md" "${REPO_DIR}/LICENSE" "${root}/${src}/"
    install -m 0644 "${REPO_DIR}/LICENSE" "${root}/${doc}/copyright"
    install -m 0644 "${REPO_DIR}/MANIFEST.md" "${root}/${doc}/MANIFEST.md"
    echo "${EXPECTED_UNAME}" > "${root}/${doc}/KERNEL"
    echo "${L4T_VERSION}" > "${root}/${doc}/L4T"
    cat > "${root}/DEBIAN/control" <<DEBCTL
Package: ${DEB_PACKAGE}
Version: ${L4T_VERSION}-${DEB_REVISION}
Architecture: arm64
Section: kernel
Priority: optional
Maintainer: ${DEB_MAINTAINER}
Depends: nvidia-l4t-kernel
Description: NXS Hub camera modules and overlays for Jetson Linux ${L4T_VERSION}
 Two out-of-tree modules for kernel ${EXPECTED_UNAME} and the device tree
 overlays of the NXS Hub's camera ports. The nxs host tool writes the boot
 entry that loads them.
DEBCTL
    # The modules load on one kernel and one Jetson Linux release (point
    # releases share a kernel string while symbol CRCs move): another
    # release is refused before anything is unpacked.
    cat > "${root}/DEBIAN/preinst" <<DEBPRE
#!/bin/sh
set -e
running="\$(sed -nE '1s/^# R([0-9]+) \\(release\\), REVISION: ([0-9][0-9.]*).*/\\1.\\2/p' /etc/nv_tegra_release 2>/dev/null || true)"
if [ -n "\${running}" ] && [ "\${running}" != "${L4T_VERSION}" ]; then
    echo "${DEB_PACKAGE}: built for Jetson Linux ${L4T_VERSION}; this host runs \${running}" >&2
    exit 1
fi
if [ ! -d "/lib/modules/${EXPECTED_UNAME}/kernel" ]; then
    echo "${DEB_PACKAGE}: built for kernel ${EXPECTED_UNAME}; this host has no such kernel" >&2
    exit 1
fi
DEBPRE
    cat > "${root}/DEBIAN/postinst" <<DEBPOST
#!/bin/sh
set -e
depmod -a "${EXPECTED_UNAME}"
DEBPOST
    cat > "${root}/DEBIAN/postrm" <<DEBPOSTRM
#!/bin/sh
depmod -a "${EXPECTED_UNAME}" 2>/dev/null || true
DEBPOSTRM
    chmod 0755 "${root}/DEBIAN/preinst" "${root}/DEBIAN/postinst" "${root}/DEBIAN/postrm"
    mkdir -p "${OUTPUT_DIR}"
    dpkg-deb --build --root-owner-group "${root}" "${OUTPUT_DIR}/${DEB_PACKAGE}_${L4T_VERSION}_arm64.deb"
    ( cd "${OUTPUT_DIR}" && sha256sum "${DEB_PACKAGE}_${L4T_VERSION}_arm64.deb" )
}

DO_PRISTINE=0; DO_PREPARE=0; DO_MODULES=0; DO_DTBS=0; DO_TAR=0; DO_COLLECT=0; DO_ARCHIVE=0; DO_DEB=0; ANY=0
while getopts "Ppmdtcabh" opt; do
    ANY=1
    case "$opt" in
        P) DO_PRISTINE=1; DO_PREPARE=1 ;;
        p) DO_PREPARE=1 ;;
        m) DO_MODULES=1 ;;
        d) DO_DTBS=1 ;;
        t) DO_TAR=1 ;;
        c) DO_COLLECT=1 ;;
        a) DO_ARCHIVE=1 ;;
        b) DO_DEB=1 ;;
        h) grep '^#' "$0" | head -25; exit 0 ;;
        *) exit 2 ;;
    esac
done
if [ "${ANY}" = 0 ]; then DO_PREPARE=1; DO_MODULES=1; DO_DTBS=1; DO_COLLECT=1; fi

codename="$( { [ -r /etc/os-release ] && . /etc/os-release && echo "${VERSION_CODENAME:-}"; } || true )"
if [ "${codename}" != "${EXPECTED_CONTAINER_CODENAME}" ]; then
    echo "FATAL: wrong build container for this branch: '${codename:-unknown}', need '${EXPECTED_CONTAINER_CODENAME}'." >&2
    echo "       Use the image built from this branch's docker/ directory:" >&2
    echo "           docker build -t ${CONTAINER_IMAGE_HINT} docker/" >&2
    echo "       then rerun with ${CONTAINER_IMAGE_HINT} (BUILD.md §3)." >&2
    exit 1
fi

[ "${DO_PREPARE}" = 1 ] && prepare_sources
[ "${DO_MODULES}" = 1 ] && build_modules
[ "${DO_DTBS}"    = 1 ] && build_dtbs
[ "${DO_TAR}"     = 1 ] && tar_modules
[ "${DO_COLLECT}" = 1 ] && collect
[ "${DO_ARCHIVE}" = 1 ] && archive
[ "${DO_DEB}"     = 1 ] && deb
log "done"
