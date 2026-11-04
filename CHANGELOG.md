# Changelog

All notable changes to the NXS kernel package for NVIDIA Jetson: the modules, the fixed overlays and the package itself. The format follows Keep a Changelog; a version is the Jetson Linux release the package is built for and the package revision, `<l4t>-<revision>`.

## [Unreleased]

## [39.2.1+nxs1.3.0] - 2026-11-04

### Changed
- The package carries the camera map header of NXS 1.3.0, and the NXS device behind a camera door is the pod: `nxs-csi2-core` reads the pod's identity and serial through the door and names the pod in its log lines.
- `nxs-csi2-core` waits a commit's painted bound and reads STATE once, instead of polling the door through the bound.
- `nxs-csi2-tegra` logs a pooled mode the camera map does not serve and keeps the session, instead of refusing it.
- A camera door that does not answer a commit's check is left alone for 500 ms before the next commit.
- The documentation names `nxs compose` for the boot entry and the per-port overlays the host tool writes.
### Changed
- `nxs-csi2-core` commits only while a capture session is open on the link. A control set before that, tegracam's defaults at probe among them, waits in the held rows and lands at the session's start, so the host tool's bring-up of a port meets no commit of the driver's.

### Fixed
- `install.sh` keeps a per-port overlay the host tool wrote under the package's default name instead of replacing it with the package's own.

## [39.2.1+nxs1.2.0] - 2026-10-19

### Added
- `nxs-csi2-core`, the NXS camera map's client, shared byte for byte with the public mainline driver: the map's transfers, the commit windows of the host interface, the rows and the mode table, and one V4L2 control for every row the capture stack does not drive itself. Its constants are generated from the map's one source, `nxs-csi2-map.h`.
- The camera map's controls on a pod's or a smart camera's video node: `Trigger Mode` (free run, fast trigger, sequential trigger, as the device offers them), `Test Pattern`, `Black Level`, `Temperature`, and one control per custom row of the device, named as the device names it.
- A passive camera node, `aliensense,passive`: the stream of a port whose hub another host owns reaches this host with no transfer on its control bus.
- `nxs-csi2-tegra` builds its frame-format table from the overlay's mode nodes and checks the camera map's mode table against them at the first session the map answers. A mode the map does not serve refuses the session with a line naming it.

### Changed
- `nxs_csi2_tegra` is `nxs-csi2-tegra`, the tegracam adapter over the shared core. It commits with the host interface's windows: the painted apply time first, then a `STATE` read every millisecond for 20 ms, and the painted run time before a mode switch or a trigger conversion. A control set while the map is INERT waits and commits at the next session that finds the map configured.
- The package's `Description` and its modules: `universal_aliensense.ko`, `nxs-csi2-core.ko`, `nxs-csi2-tegra.ko` and `aliensense_generic_des.ko`.

### Removed
- The four FRAMOS-named private controls `shutter_mode`, `test_pattern`, `operation_mode` and `black_level`. `Trigger Mode` replaces `shutter_mode`, the core's `Test Pattern` and `Black Level` replace the other two by the map's rows, and `operation_mode`, which committed nothing, has no successor.

## [39.2.1-1] - 2026-09-28

### Added
- The first package for Jetson Linux 39.2.1 (JetPack 7.2.1, kernel 6.8): the `universal_aliensense` sensor driver, steered by the control table the device tree carries, and the `aliensense_generic_des` deserializer driver.
- The three fixed overlays: the camera mux, `cam0-gmsl` and `cam1-gmsl`; the `nxs` host tool generates the per-port overlays.
- The package refuses a host running another Jetson Linux release or lacking its kernel, and ships its source under `/usr/src/nxs-jetson-<l4t>/`.

## [36.4.4-1] - 2026-09-28

### Added
- The first package for Jetson Linux 36.4.4 (JetPack 6.2.1, kernel 5.15): the `universal_aliensense` sensor driver, steered by the control table the device tree carries, and the `aliensense_generic_des` deserializer driver.
- The three fixed overlays: the camera mux, `cam0-gmsl` and `cam1-gmsl`; the `nxs` host tool generates the per-port overlays.
- The package refuses a host running another Jetson Linux release or lacking its kernel, and ships its source under `/usr/src/nxs-jetson-<l4t>/`.
