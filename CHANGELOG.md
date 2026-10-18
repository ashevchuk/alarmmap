# Changelog

All notable changes to this project are documented in this file. The format
is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the
project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0] - 2026-10-18

### Added

- Experimental Android app, released as an APK for 64-bit ARM phones with
  Android 8.0 or newer: live data, one-finger panning, pinch zoom, and tapping
  a target for its name and path.

### Changed

- The map redraws several times faster after panning and zooming: region and
  basemap geometry stays on the GPU instead of being sent again each time.

## [0.1.2] - 2026-10-18

### Changed

- The target summary panel is only as wide as its content.
- Target markers are smaller on the whole-country view and grow to full size
  when zooming in.

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

[Unreleased]: https://github.com/ashevchuk/alarmmap/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/ashevchuk/alarmmap/compare/v0.1.2...v0.2.0
[0.1.2]: https://github.com/ashevchuk/alarmmap/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/ashevchuk/alarmmap/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/ashevchuk/alarmmap/releases/tag/v0.1.0
