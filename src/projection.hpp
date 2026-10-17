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

}  // namespace alarmmap
