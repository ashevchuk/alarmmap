#pragma once

#include "projection.hpp"
#include "region_geometry.hpp"
#include "target_snapshot.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace alarmmap {

// Axis-aligned rectangle on the map plane; y grows southwards.
struct MapRect {
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;

    bool operator==(const MapRect&) const = default;
};

// Footprint of a grid cell; empty sizes fall back to the site's default cell.
MapRect cellRect(const Cell& cell);

// Answers whether a rectangle touches any region under an air alert.
// Keeps pointers into `shapes`, which must outlive the filter.
class AirAlertFilter {
public:
    explicit AirAlertFilter(const std::vector<RegionShape>& shapes);
    void setAirRegions(const std::set<std::string>& regionIds);
    bool intersects(const MapRect& rect) const;

private:
    struct Polygon {
        const std::vector<std::vector<MapPoint>>* rings;
        MapRect bounds;
    };
    std::unordered_map<std::string, std::vector<Polygon>> polygonsByRegion_;
    std::vector<const Polygon*> active_;
};

struct HeatCandidate {
    HeatCell cell;
    bool extrapolation = false;
    // Target cells an extrapolated cell is allowed to hang from.
    std::vector<MapRect> holdAnchors;
};

struct HeatQuad {
    MapRect rect;
    double strength = 0;

    bool operator==(const HeatQuad&) const = default;
};

// Trail and extrapolation cells that touch an air alert region; a null filter
// (alerts not known yet) keeps every cell. Depends only on the snapshot and
// the alerts, so it is recomputed when either changes.
std::vector<HeatCandidate> selectHeatCandidates(const TargetSnapshot& snapshot,
                                                const AirAlertFilter* airAlerts);

// Cells to draw `elapsedSeconds` after the snapshot. Extrapolated cells are
// kept only while connected, edge or corner, to live trail cells or their
// hold anchors, directly or through other extrapolated cells.
std::vector<HeatQuad> heatQuadsAt(const std::vector<HeatCandidate>& candidates,
                                  double elapsedSeconds, double fogSeconds);

}  // namespace alarmmap
