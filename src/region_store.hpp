#pragma once

#include "region_geometry.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace alarmmap {

// Simplification tolerances of the two detail levels, in map-plane units.
// The coarse one is about 240 m on the ground: under one pixel even with the
// country on a 4K screen. Zoomed in, the detailed shapes take over; the source
// data itself has a resolution of roughly 0.01 map units, so finer would
// change nothing.
inline constexpr double kCoarseTolerance = 0.05;
inline constexpr double kDetailedTolerance = 0.002;

// Region shapes at both detail levels, simplified and triangulated ahead of
// time, so the app starts without downloading or processing the site's
// GeoJSON.
struct PreparedRegions {
    std::vector<RegionShape> coarse;
    // Their exact polygons are left out: only the coarse level serves hit tests.
    std::vector<RegionShape> detailed;
};

// Compact binary form. A point is stored as whole steps of 1/65535 of its
// shape's extent, as the difference from the point before; a point read back
// is off by at most half a step, a couple of metres on the largest oblast.
std::string writePreparedRegions(const PreparedRegions& regions);
// Throws std::runtime_error on data that is not in that form.
PreparedRegions readPreparedRegions(std::string_view bytes);

}  // namespace alarmmap
