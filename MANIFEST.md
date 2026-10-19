# Manifest — branch l4t-r39.2.1

| Field | Value |
|---|---|
| JetPack | 7.2.1 |
| Jetson Linux (L4T) | 39.2.1 |
| Kernel | 6.8.12 (`kernel/kernel-noble`), target `uname -r` `6.8.12-1021-tegra` |
| Board | Orin Nano devkit — module p3767-0005 on carrier p3768-0000 |
| Base DTB | `tegra234-p3768-0000+p3767-0005-nv.dtb`, root compatible `nvidia,p3768-0000+p3767-0005`. r39.2.1 also builds `-nv-super.dtb`, root compatible `nvidia,p3768-0000+p3767-0005-super`. A board boots the one declaring the first string of `/proc/device-tree/compatible`; `model` does not separate the module SKUs |
| public_sources.tbz2 | `https://developer.nvidia.com/downloads/embedded/l4t/r39_release_v2.1/sources/public_sources.tbz2` |
| public_sources sha256 | `8346f0371649a92c42293b2d4a7bf3d69e4dd1de23009e1463ea910994a81371` (verified 2026-09-17) |

## Build record

| Field | Value |
|---|---|
| Built | 2026-10-15, container `jetson-build-r3921` (ubuntu:24.04) |
| vermagic (all four modules) | `6.8.12-1021-tegra SMP preempt mod_unload modversions aarch64` |
| `nxs-csi2-core.ko` sha256 | `21a5f34c3c3d964da31407e2aa598223dcba0cb21d69eb850339665379047945` |
| `nxs-csi2-tegra.ko` sha256 | `396ce77074e96b9d4e5cfd1d03c01315911f968c2ef0a3e26bb5f7a40c56e16b` |
| `aliensense_generic_des.ko` sha256 | `1f5b285f04ed806360477b614212b2e0d42510a08ccd706dcb35d7a153c52d99` |
| `universal_aliensense.ko` sha256 | `54260c57407a7b0055ee8517767906abb9e6e76b5c2d13dacd605ba4015fa4a5` |
| DTBOs | 3, all built; the offline `fdtoverlay` gate onto both base DTBs (PORTING.md gate 3) is pending for this build |

## Provenance

`aliensense_generic_des.c`, the three overlays and the two Makefiles descend
from the RidgeRun delivery, commit `9a68f9154` ("feat: add extra dtbos"),
extracted on 2026-07-11 (via `git show`, no worktree copy); the RidgeRun/GPL
copyright headers are preserved inside the files. `nxs-csi2-core.c`,
`nxs-csi2-core.h` and `nxs-csi2-map.h` are Aliensense's, vendored byte for
byte from the public `nxs-drivers` repository (the header generated from the
map's one source in the `nxs` tool's tree). `nxs-csi2-tegra.c` is
Aliensense's.

Deliberately **not** carried over from that delivery (see
`patches/attic/framos-framework-residue.patch` and its README):

- `nvidia-oot/include/media/camera_common.h` additions (+53 lines)
- `nvidia-oot/include/media/tegra-v4l2-camera.h` FRAMOS CIDs (+6 lines)
- `nvidia-oot/drivers/media/platform/tegra/camera/sensor_common.c` pixel_t
  fallback (+26/−12)

All three were statically verified dead for this stack; with them dropped, the
delta modifies **zero NVIDIA-owned files** (the two Makefiles under `source/`
are full replacements of stock files, adding only `obj-m`/`dtbo-y` lines).

A fourth candidate joined the attic on 2026-09-02:
`patches/attic/capture-vi-pixel-line-reclass.patch` (19 lines in NVIDIA's
`fusa-capture/capture-vi.c`, reclassifying `PIXEL_LONG/SHORT_LINE` as
correctable). It sat untracked in the working tree from 2026-08-23 and was
never part of the rig's proven state — the rig runs a stock-source
`tegra-camera.ko` and delivers on both links. See the patch's README for the
symptoms that would justify upstreaming it. The customer package therefore
carries four modules and the DTBOs over stock JetPack modules, and the
source bundle inside it covers exactly those (RELEASING.md).

## Delta inventory (16 files under `source/`)

- `nvidia-oot/drivers/media/i2c/nxs-csi2-core.c`, `nxs-csi2-core.h` — the NXS camera map's client, shared with the mainline `nxs-csi2` driver: the transfers, the commit windows, the rows, the mode table, the custom controls
- `nvidia-oot/drivers/media/i2c/nxs-csi2-map.h` — the map's constants, generated from `constants/nxs_map.yaml` of the `nxs` tool's tree
- `nvidia-oot/drivers/media/i2c/nxs-csi2-tegra.c` — the tegracam driver over the core: a head behind an NXS pod, or a smart camera; the frmfmt table from the mode nodes, gain, exposure and frame rate as commits in physical units (binding: `docs/dt-bindings/aliensense,nxs-csi2.md`)
- `nvidia-oot/drivers/media/i2c/universal_aliensense.c` — tegracam sensor with table-driven controls from the `aliensense-ctrl` node, a head without a unit (binding: `docs/dt-bindings/aliensense,universal.md`); a node without the table behaves as the v1.0.0 stub
- `nvidia-oot/drivers/media/i2c/universal_aliensense_mode_tbls.h` — the universal driver's 8-mode frmfmt table (56 L)
- `nvidia-oot/drivers/media/i2c/aliensense_generic_des.c` — probe-only dummy GMSL deserializer (178 L)
- `nvidia-oot/drivers/media/i2c/Makefile` — r39.2.1 stock plus the four `obj-m` lines
- `hardware/nvidia/t23x/nv-public/overlay/Makefile` — r39.2.1 stock plus 7 `dtbo-y` lines
- `hardware/nvidia/t23x/nv-public/overlay/tegra234-p3767-camera-p3768-aliensense_*.dts` — 7 overlays
  (general-mux foundation; cam0/cam1 × 2/4-lane universal overlays, each with both virtual channels; 2 GMSL patch overlays). The per-port overlays of a port with a unit are the host tool's
