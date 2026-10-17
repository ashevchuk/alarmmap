#pragma once

#include "target_motion.hpp"

#include <cstddef>
#include <vector>

namespace alarmmap {

// How one drawn marker is placed after grouping.
struct MarkerPlacement {
    // Index into the markers given to layoutMarkers().
    size_t index = 0;
    // Targets this marker stands for; above 1 the marker shows a counter.
    int count = 1;
    // Horizontal shift in screen pixels that keeps different target types
    // in one cell side by side.
    double offsetPx = 0;
    double scale = 1;
};

// Grouping of map.ukrainealarm.com (detailTargetGroups.js): markers of one
// type that have settled in the same cell merge into one marker with a
// counter; a marker still gliding in stays separate. Different types in a
// cell are spread horizontally within the cell. `pixelsPerMapUnit` converts
// cell sizes to screen pixels. Returns placements of the markers to draw.
std::vector<MarkerPlacement> layoutMarkers(const std::vector<TargetMotion::Marker>& markers,
                                           double pixelsPerMapUnit);

}  // namespace alarmmap
