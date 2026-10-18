# Changelog

All notable changes to this project are documented in this file. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.1] - 2026-10-18

### Changed

- Zooming out to the minimum zoom brings the map back to the middle of the
  window.
- The heat layer is drawn at the site's opacity of 0.5.

### Removed

- The yellow halo under target markers.

### Fixed

- Resizing the window with the mouse no longer freezes the desktop on the
  NVIDIA driver. The window is created without multisampling, which made the
  driver stall the GPU.

## [0.1.0] - 2026-10-18

### Added

- Live map of air raid alerts and aerial targets from map.ukrainealarm.com.
- Targets classified by kind as on the site, with unidentified ones counted
  separately, and a summary panel of current targets.
- Heat trails of target movement.
- Pan and zoom, with detailed region outlines when zoomed in.
- OpenFreeMap basemap with GeoNames place names.
- Hovered targets show their name and the path flown since start; targets
  animate in and out.
- Window icon.
- Installable, relocatable build and an x86-64 AppImage.

[Unreleased]: https://github.com/ashevchuk/alarmmap/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/ashevchuk/alarmmap/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ashevchuk/alarmmap/releases/tag/v0.1.0
