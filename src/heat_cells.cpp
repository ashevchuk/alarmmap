#include "heat_cells.hpp"

#include "heat_timing.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace alarmmap {

namespace {

constexpr double kFallbackCellWidth = 0.00595;
constexpr double kFallbackCellHeight = 0.00945;
constexpr double kGeometryEpsilon = 1e-10;
constexpr double kMinimumVisibleStrength = 0.001;
// Touch tolerance relative to the largest cell, as on the site.
constexpr double kConnectionEpsilonFactor = 1e-7;

MapRect boundsOf(const std::vector<MapPoint>& ring) {
    MapRect bounds{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity(),
                   -std::numeric_limits<double>::infinity()};
    for (const MapPoint& point : ring) {
        bounds.left = std::min(bounds.left, point.x);
        bounds.top = std::min(bounds.top, point.y);
        bounds.right = std::max(bounds.right, point.x);
        bounds.bottom = std::max(bounds.bottom, point.y);
    }
    return bounds;
}

enum class Side { Outside, Boundary, Inside };

Side ringSide(const std::vector<MapPoint>& ring, double x, double y) {
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        const MapPoint& a = ring[j];
        const MapPoint& b = ring[i];
        const double cross = (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
        if (std::abs(cross) <= kGeometryEpsilon && x >= std::min(a.x, b.x) - kGeometryEpsilon &&
            x <= std::max(a.x, b.x) + kGeometryEpsilon && y >= std::min(a.y, b.y) - kGeometryEpsilon &&
            y <= std::max(a.y, b.y) + kGeometryEpsilon) {
            return Side::Boundary;
        }
        if ((a.y > y) != (b.y > y) && x < (b.x - a.x) * (y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside ? Side::Inside : Side::Outside;
}

bool polygonContains(const std::vector<std::vector<MapPoint>>& rings, double x, double y) {
    if (ringSide(rings[0], x, y) == Side::Outside) return false;
    return std::none_of(rings.begin() + 1, rings.end(), [&](const std::vector<MapPoint>& hole) {
        return ringSide(hole, x, y) == Side::Inside;
    });
}

// Liang-Barsky clip of segment a-b against the rectangle, touching included.
bool segmentIntersectsRect(const MapPoint& a, const MapPoint& b, const MapRect& rect) {
    const double e = kGeometryEpsilon;
    if (std::max(a.x, b.x) < rect.left - e || std::min(a.x, b.x) > rect.right + e ||
        std::max(a.y, b.y) < rect.top - e || std::min(a.y, b.y) > rect.bottom + e) {
        return false;
    }
    double near = 0;
    double far = 1;
    const double axes[2][4] = {{a.x, b.x, rect.left, rect.right}, {a.y, b.y, rect.top, rect.bottom}};
    for (const auto& [start, end, low, high] : axes) {
        const double delta = end - start;
        if (std::abs(delta) <= e) {
            if (start < low - e || start > high + e) return false;
            continue;
        }
        const double first = (low - e - start) / delta;
        const double last = (high + e - start) / delta;
        near = std::max(near, std::min(first, last));
        far = std::min(far, std::max(first, last));
        if (near > far) return false;
    }
    return true;
}

bool polygonIntersectsRect(const std::vector<std::vector<MapPoint>>& rings, const MapRect& bounds,
                           const MapRect& rect) {
    const double e = kGeometryEpsilon;
    if (bounds.right < rect.left - e || bounds.left > rect.right + e ||
        bounds.bottom < rect.top - e || bounds.top > rect.bottom + e) {
        return false;
    }
    const MapPoint corners[] = {{rect.left, rect.top},
                                {rect.left, rect.bottom},
                                {rect.right, rect.top},
                                {rect.right, rect.bottom}};
    for (const MapPoint& corner : corners) {
        if (polygonContains(rings, corner.x, corner.y)) return true;
    }
    // Corners are not enough: a narrow region can cross the middle of the
    // cell, and a small town can lie entirely inside it.
    for (const auto& ring : rings) {
        for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
            if (segmentIntersectsRect(ring[j], ring[i], rect)) return true;
        }
    }
    return false;
}

bool touches(const MapRect& a, const MapRect& b, double epsilon) {
    return !(a.left > b.right + epsilon || a.right < b.left - epsilon ||
             a.top > b.bottom + epsilon || a.bottom < b.top - epsilon);
}

}  // namespace

MapRect cellRect(const Cell& cell) {
    const double width = cell.width > 0 ? cell.width : kFallbackCellWidth;
    const double height = cell.height > 0 ? cell.height : kFallbackCellHeight;
    const MapPoint topLeft = normalizedToMap(cell.x - width / 2, cell.y - height / 2);
    const MapPoint bottomRight = normalizedToMap(cell.x + width / 2, cell.y + height / 2);
    return {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
}

AirAlertFilter::AirAlertFilter(const std::vector<RegionShape>& shapes) {
    for (const RegionShape& shape : shapes) {
        auto& polygons = polygonsByRegion_[shape.id];
        for (const auto& rings : shape.polygons) {
            if (rings.empty() || rings[0].size() < 3) continue;
            polygons.push_back({&rings, boundsOf(rings[0])});
        }
    }
}

void AirAlertFilter::setAirRegions(const std::set<std::string>& regionIds) {
    active_.clear();
    for (const std::string& id : regionIds) {
        const auto found = polygonsByRegion_.find(id);
        if (found == polygonsByRegion_.end()) continue;
        for (const Polygon& polygon : found->second) active_.push_back(&polygon);
    }
}

bool AirAlertFilter::intersects(const MapRect& rect) const {
    return std::any_of(active_.begin(), active_.end(), [&](const Polygon* polygon) {
        return polygonIntersectsRect(*polygon->rings, polygon->bounds, rect);
    });
}

std::vector<HeatCandidate> selectHeatCandidates(const TargetSnapshot& snapshot,
                                                const AirAlertFilter* airAlerts) {
    auto allowed = [airAlerts](const MapRect& rect) {
        return airAlerts == nullptr || airAlerts->intersects(rect);
    };
    std::vector<HeatCandidate> candidates;
    auto add = [&](const HeatCell& cell, bool extrapolation) {
        if (!allowed(cellRect(cell.cell))) return;
        HeatCandidate candidate{cell, extrapolation, {}};
        if (cell.extrapolationHoldAnchors) {
            for (const Cell& anchor : *cell.extrapolationHoldAnchors) {
                const MapRect rect = cellRect(anchor);
                if (allowed(rect)) candidate.holdAnchors.push_back(rect);
            }
        }
        candidates.push_back(std::move(candidate));
    };
    for (const HeatCell& cell : snapshot.trail) add(cell, false);
    for (const HeatCell& cell : snapshot.extrapolationTrail) add(cell, true);
    return candidates;
}

std::vector<HeatQuad> heatQuadsAt(const std::vector<HeatCandidate>& candidates,
                                  double elapsedSeconds, double fogSeconds) {
    struct Live {
        const HeatCandidate* candidate;
        HeatQuad quad;
    };
    std::vector<Live> live;
    for (const HeatCandidate& candidate : candidates) {
        const HeatCell aged = agedHeatCell(candidate.cell, elapsedSeconds);
        if (!isHeatCellAlive(aged, fogSeconds)) continue;
        const double strength = std::clamp(heatStrength(aged, fogSeconds), 0.0, 1.0);
        if (strength <= kMinimumVisibleStrength) continue;
        live.push_back({&candidate, {cellRect(candidate.cell.cell), strength}});
    }

    std::vector<MapRect> anchors;
    std::vector<size_t> pending;
    double largestSide = 0;
    for (size_t i = 0; i < live.size(); ++i) {
        const MapRect& rect = live[i].quad.rect;
        largestSide = std::max({largestSide, rect.right - rect.left, rect.bottom - rect.top});
        if (!live[i].candidate->extrapolation) {
            anchors.push_back(rect);
            continue;
        }
        pending.push_back(i);
        for (const MapRect& anchor : live[i].candidate->holdAnchors) {
            anchors.push_back(anchor);
            largestSide = std::max({largestSide, anchor.right - anchor.left, anchor.bottom - anchor.top});
        }
    }

    // Breadth-first growth from the anchors through touching extrapolated cells.
    const double epsilon = largestSide * kConnectionEpsilonFactor;
    std::vector<bool> connected(live.size(), false);
    std::deque<MapRect> frontier(anchors.begin(), anchors.end());
    while (!frontier.empty() && !pending.empty()) {
        const MapRect anchor = frontier.front();
        frontier.pop_front();
        std::erase_if(pending, [&](size_t index) {
            if (!touches(live[index].quad.rect, anchor, epsilon)) return false;
            connected[index] = true;
            frontier.push_back(live[index].quad.rect);
            return true;
        });
    }

    std::vector<HeatQuad> quads;
    for (size_t i = 0; i < live.size(); ++i) {
        if (!live[i].candidate->extrapolation || connected[i]) quads.push_back(live[i].quad);
    }
    return quads;
}

}  // namespace alarmmap
