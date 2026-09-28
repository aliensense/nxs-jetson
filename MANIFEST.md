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
| Built | 2026-09-17, container `jetson-build-r3921` (ubuntu:24.04) |
| vermagic (both modules) | `6.8.12-1021-tegra SMP preempt mod_unload modversions aarch64` |
| `universal_aliensense.ko` sha256 | `31254ff9f041fb133df16328990b3cd9a2d80f62e9195db9751e3d30884f3999` |
| `aliensense_generic_des.ko` sha256 | `2a8e84d27802951684cac637296475a92a6a13fe415ef68c6d715834f2fbe91e` |
| DTBOs | 7, all built; the offline `fdtoverlay` gate passes onto both base DTBs (PORTING.md gate 3) |

## Provenance

The files under `source/` were extracted (via `git show`, no worktree copy)
from the RidgeRun delivery, commit `9a68f9154` ("feat: add extra dtbos"),
on 2026-07-11. RidgeRun/GPL copyright headers are preserved inside the
source files; the sensor driver descends from the FRAMOS Jetson sensor
drivers (GPL-2.0), which its header records.

**One deliberate deviation from the delivery:** the two dead
`board_priv_pdata->gmsl` writes (`universal_aliensense.c`, gmsl-link probe
block) are removed. The `gmsl` pdata field exists only in the attic'd
`camera_common.h` patch — stock L4T headers reject the writes, and nothing
in the tree ever read the field. All other files are byte-identical to the
delivery commit.

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
carries two modules and the DTBOs over stock JetPack modules, and the
source bundle inside it covers exactly those (RELEASING.md).

## Delta inventory (12 files under `source/`)

- `nvidia-oot/drivers/media/i2c/universal_aliensense.c` — tegracam sensor with table-driven controls from the `aliensense-ctrl` node (binding: `docs/dt-bindings/aliensense,universal.md`); a node without the table behaves as the v1.0.0 stub
- `nvidia-oot/drivers/media/i2c/universal_aliensense_mode_tbls.h` — 8-mode frmfmt table (56 L)
- `nvidia-oot/drivers/media/i2c/aliensense_generic_des.c` — probe-only dummy GMSL deserializer (174 L)
- `nvidia-oot/drivers/media/i2c/Makefile` — r39.2.1 stock plus the two `obj-m` lines (20, 37)
- `hardware/nvidia/t23x/nv-public/overlay/Makefile` — r39.2.1 stock plus 7 `dtbo-y` lines (79–85)
- `hardware/nvidia/t23x/nv-public/overlay/tegra234-p3767-camera-p3768-aliensense_*.dts` — 7 overlays
  (general-mux foundation; cam0/cam1 × 2/4-lane universal overlays, each with both virtual channels; 2 GMSL patch overlays)
