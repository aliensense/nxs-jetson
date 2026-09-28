# Changelog

All notable changes to the NXS kernel package for NVIDIA Jetson: the two modules, the fixed overlays and the package itself. The format follows Keep a Changelog; a version is the Jetson Linux release the package is built for and the package revision, `<l4t>-<revision>`.

## [Unreleased]

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
