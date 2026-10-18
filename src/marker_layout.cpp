#include "marker_layout.hpp"

#include "heat_cells.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace alarmmap {

namespace {

constexpr double kTypeSpacingPx = 48;
constexpr double kIconPx = 34;
constexpr double kCompactScale = 0.7;
// Spread of type columns as a share of the cell width; on a far zoom a
// partial overlap is better than a marker outside its cell.
constexpr double kSpreadShareOfCell = 0.7;
// Positions closer than this share of the cell size count as the same spot.
constexpr double kSamePositionShare = 0.02;

using CellKey = std::tuple<double, double, double, double>;

CellKey cellKey(const Target& target) {
    return {target.cell.x, target.cell.y, target.cell.width, target.cell.height};
}

// Drawn position without the per-target jitter that keeps markers in one
// cell apart; settled markers of one cell share it.
MapPoint basePosition(const TargetMotion::Marker& marker) {
    const MapPoint jitter = normalizedToMap(marker.target.markerOffsetX, marker.target.markerOffsetY);
    return {marker.position.x - jitter.x, marker.position.y - jitter.y};
}

}  // namespace

std::vector<MarkerPlacement> layoutMarkers(const std::vector<TargetMotion::Marker>& markers,
                                           double pixelsPerMapUnit) {
    // Cell -> type -> marker indices.
    std::map<CellKey, std::map<int, std::vector<size_t>>> cells;
    for (size_t i = 0; i < markers.size(); ++i) {
        cells[cellKey(markers[i].target)][markers[i].target.type].push_back(i);
    }

    std::vector<MarkerPlacement> placements;
    for (const auto& [key, types] : cells) {
        const MapRect rect = cellRect(markers[types.begin()->second.front()].target.cell);
        const double cellWidthPx = (rect.right - rect.left) * pixelsPerMapUnit;
        const auto columns = static_cast<double>(types.size());
        const bool compact = columns > 1 && cellWidthPx < kTypeSpacingPx * (columns - 1) + kIconPx;
        const double scale = compact ? kCompactScale : 1;
        const double spacing = std::min(kTypeSpacingPx * scale,
                                        cellWidthPx * kSpreadShareOfCell / std::max(1.0, columns - 1));
        const double toleranceX = std::max(1e-9, (rect.right - rect.left) * kSamePositionShare);
        const double toleranceY = std::max(1e-9, (rect.bottom - rect.top) * kSamePositionShare);

        int column = 0;
        for (const auto& [type, members] : types) {
            const double offsetPx = (column++ - (columns - 1) / 2) * spacing;
            // Steady, settled markers lead their group.
            std::vector<size_t> ordered = members;
            std::stable_sort(ordered.begin(), ordered.end(), [&](size_t left, size_t right) {
                const auto rank = [&](size_t index) {
                    return std::tuple(markers[index].fadingOut, markers[index].gliding,
                                      markers[index].target.trackId);
                };
                return rank(left) < rank(right);
            });

            std::vector<std::vector<size_t>> groups;
            for (const size_t index : ordered) {
                const MapPoint position = basePosition(markers[index]);
                const auto group = std::find_if(groups.begin(), groups.end(), [&](const auto& items) {
                    const MapPoint leader = basePosition(markers[items.front()]);
                    return std::abs(leader.x - position.x) <= toleranceX &&
                           std::abs(leader.y - position.y) <= toleranceY;
                });
                if (group == groups.end()) {
                    groups.push_back({index});
                } else {
                    group->push_back(index);
                }
            }
            for (const auto& group : groups) {
                const auto active = std::count_if(group.begin(), group.end(),
                                                  [&](size_t index) { return !markers[index].fadingOut; });
                const int count = static_cast<int>(active > 0 ? active : group.size());
                placements.push_back({group.front(), count, offsetPx, scale, group});
            }
        }
    }
    std::sort(placements.begin(), placements.end(),
              [](const MarkerPlacement& left, const MarkerPlacement& right) { return left.index < right.index; });
    return placements;
}

}  // namespace alarmmap
