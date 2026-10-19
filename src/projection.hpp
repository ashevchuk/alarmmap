#pragma once

namespace alarmmap {

// The map plane shared with map.ukrainealarm.com: Web Mercator fitted into a
// kMapWidth x kMapHeight box around Ukraine. Normalized coordinates in target
// snapshots are positions on this plane divided by its size.
inline constexpr double kMapWidth = 260;
inline constexpr double kMapHeight = 175;

struct MapPoint {
    double x = 0;
    double y = 0;

    bool operator==(const MapPoint&) const = default;
};

MapPoint lonLatToMap(double longitude, double latitude);
MapPoint normalizedToMap(double x, double y);

// Web Mercator world square, as used by map tiles: x and y run from 0 to 1
// from the west edge and the north edge. The map plane is a scaled and
// shifted copy of it, so the conversion is linear.
MapPoint worldToMap(double x, double y);
MapPoint mapToWorld(MapPoint point);

// Zoom level of 256-pixel web map tiles that matches the given scale.
double tileZoomForScale(double pixelsPerMapUnit);
// Inverse of tileZoomForScale.
double scaleForTileZoom(double tileZoom);

}  // namespace alarmmap
