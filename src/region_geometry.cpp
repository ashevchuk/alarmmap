#include "region_geometry.hpp"

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace alarmmap {

using json = nlohmann::json;

namespace {

// Foreign GeoJSON member keeping the site's geometry of an overridden region.
constexpr const char* kSiteGeometryKey = "siteGeometry";
constexpr double kLabelGridSteps = 24;
// A label needs no better than about a kilometre, so the search runs on
// outlines simplified this much, which makes it many times faster.
constexpr double kLabelSearchTolerance = 0.2;
constexpr int kLabelRefinementLevels = 4;

double distanceToSegment(const MapPoint& point, const MapPoint& start, const MapPoint& end) {
    const double dx = end.x - start.x;
    const double dy = end.y - start.y;
    const double lengthSquared = dx * dx + dy * dy;
    double t = lengthSquared == 0
                   ? 0
                   : ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(point.x - (start.x + t * dx), point.y - (start.y + t * dy));
}

// Marks points of points[first..last] that must survive, endpoints excluded.
// Iterative so that long rings cannot exhaust the stack.
void markDouglasPeucker(const std::vector<MapPoint>& points, size_t first, size_t last,
                        double tolerance, std::vector<bool>& keep) {
    std::vector<std::pair<size_t, size_t>> pending = {{first, last}};
    while (!pending.empty()) {
        const auto [start, end] = pending.back();
        pending.pop_back();
        double farthestDistance = 0;
        size_t farthest = start;
        for (size_t i = start + 1; i < end; ++i) {
            const double distance = distanceToSegment(points[i], points[start], points[end]);
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthest = i;
            }
        }
        if (farthestDistance <= tolerance) continue;
        keep[farthest] = true;
        pending.emplace_back(start, farthest);
        pending.emplace_back(farthest, end);
    }
}

std::vector<MapPoint> projectRing(const json& coordinates) {
    std::vector<MapPoint> ring;
    ring.reserve(coordinates.size());
    for (const json& position : coordinates) {
        if (!position.is_array() || position.size() < 2) {
            throw std::runtime_error("invalid GeoJSON position");
        }
        ring.push_back(lonLatToMap(position[0].get<double>(), position[1].get<double>()));
    }
    return ring;
}

json polygonsOf(const json& geometry, const std::string& regionId) {
    const std::string type = geometry.at("type").get<std::string>();
    if (type == "Polygon") return json::array({geometry.at("coordinates")});
    if (type == "MultiPolygon") return geometry.at("coordinates");
    throw std::runtime_error("region " + regionId + " has unsupported geometry " + type);
}

}  // namespace

std::vector<MapPoint> simplifyRing(const std::vector<MapPoint>& ring, double tolerance) {
    std::vector<MapPoint> points = ring;
    if (points.size() > 1 && points.front() == points.back()) points.pop_back();
    if (points.size() < 3) return {};

    // A closed ring has no natural endpoints; anchor it at the first point and
    // the point farthest from it, then simplify both halves.
    size_t opposite = 1;
    double farthestDistance = 0;
    for (size_t i = 1; i < points.size(); ++i) {
        const double distance =
            std::hypot(points[i].x - points[0].x, points[i].y - points[0].y);
        if (distance > farthestDistance) {
            farthestDistance = distance;
            opposite = i;
        }
    }

    points.push_back(points.front());
    std::vector<bool> keep(points.size(), false);
    keep[0] = keep[opposite] = true;
    markDouglasPeucker(points, 0, opposite, tolerance, keep);
    markDouglasPeucker(points, opposite, points.size() - 1, tolerance, keep);

    std::vector<MapPoint> simplified;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        if (keep[i]) simplified.push_back(points[i]);
    }
    if (simplified.size() < 3) return {};
    return simplified;
}

MapPoint labelPoint(const RegionShape& shape) {
    if (shape.outlines.empty()) throw std::runtime_error("region " + shape.id + " has no outline");
    std::vector<std::vector<MapPoint>> outlines;
    for (const auto& ring : shape.outlines) {
        std::vector<MapPoint> coarse = simplifyRing(ring, kLabelSearchTolerance);
        // Rings too small to survive keep their full shape.
        outlines.push_back(coarse.empty() ? ring : std::move(coarse));
    }
    double left = std::numeric_limits<double>::infinity(), top = left;
    double right = -left, bottom = -left;
    for (const auto& ring : outlines) {
        for (const MapPoint& point : ring) {
            left = std::min(left, point.x);
            right = std::max(right, point.x);
            top = std::min(top, point.y);
            bottom = std::max(bottom, point.y);
        }
    }

    // Distance to the nearest border, negative outside the region.
    const auto signedDistance = [&](const MapPoint& point) {
        bool inside = false;
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& ring : outlines) {
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                const MapPoint& a = ring[j];
                const MapPoint& b = ring[i];
                if ((a.y > point.y) != (b.y > point.y) &&
                    point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x) {
                    inside = !inside;
                }
                nearest = std::min(nearest, distanceToSegment(point, a, b));
            }
        }
        return inside ? nearest : -nearest;
    };

    // Coarse grid over the bounds, then finer grids around the best point.
    MapPoint best{(left + right) / 2, (top + bottom) / 2};
    double bestDistance = signedDistance(best);
    double step = std::max(right - left, bottom - top) / kLabelGridSteps;
    MapPoint center = best;
    double halfSpan = std::max(right - left, bottom - top) / 2;
    for (int level = 0; level < kLabelRefinementLevels; ++level) {
        for (double y = center.y - halfSpan; y <= center.y + halfSpan; y += step) {
            for (double x = center.x - halfSpan; x <= center.x + halfSpan; x += step) {
                const double distance = signedDistance({x, y});
                if (distance > bestDistance) {
                    bestDistance = distance;
                    best = {x, y};
                }
            }
        }
        center = best;
        halfSpan = step;
        step /= kLabelGridSteps / 2;
    }
    return best;
}

void overrideRegionGeometry(json& regions, const json& overrides) {
    for (const json& override : overrides.at("features")) {
        const std::string id = override.at("properties").at("regionId").get<std::string>();
        auto& features = regions.at("features");
        const auto target = std::find_if(features.begin(), features.end(), [&](const json& feature) {
            return feature.at("properties").value("regionId", "") == id;
        });
        if (target == features.end()) {
            throw std::runtime_error("geometry override for unknown region " + id);
        }
        if (!target->contains(kSiteGeometryKey)) (*target)[kSiteGeometryKey] = (*target)["geometry"];
        (*target)["geometry"] = override.at("geometry");
    }
}

std::vector<RegionShape> buildRegionShapes(const json& geojson, double tolerance) {
    std::vector<RegionShape> shapes;
    for (const json& feature : geojson.at("features")) {
        const json& properties = feature.at("properties");
        const json& geometry = feature.at("geometry");
        RegionShape shape;
        shape.id = properties.at("regionId").get<std::string>();
        shape.name = properties.value("name", "");
        shape.rank = properties.at("rank").get<int>();

        const json& hitGeometry = feature.contains(kSiteGeometryKey) ? feature[kSiteGeometryKey] : geometry;
        for (const json& polygon : polygonsOf(hitGeometry, shape.id)) {
            auto& exactRings = shape.polygons.emplace_back();
            for (const json& ring : polygon) exactRings.push_back(projectRing(ring));
        }

        for (const json& polygon : polygonsOf(geometry, shape.id)) {
            std::vector<std::vector<std::array<double, 2>>> rings;
            for (size_t ringIndex = 0; ringIndex < polygon.size(); ++ringIndex) {
                std::vector<MapPoint> ring = simplifyRing(projectRing(polygon[ringIndex]), tolerance);
                if (ring.empty()) {
                    // A collapsed outer ring takes the whole polygon with it.
                    if (ringIndex == 0) break;
                    continue;
                }
                std::vector<std::array<double, 2>>& earcutRing = rings.emplace_back();
                for (const MapPoint& point : ring) earcutRing.push_back({point.x, point.y});
                shape.outlines.push_back(std::move(ring));
            }
            if (rings.empty()) continue;

            const auto base = static_cast<std::uint32_t>(shape.vertices.size());
            for (const auto& ring : rings) {
                for (const auto& point : ring) shape.vertices.push_back({point[0], point[1]});
            }
            for (const std::uint32_t index : mapbox::earcut<std::uint32_t>(rings)) {
                shape.indices.push_back(base + index);
            }
        }
        if (!shape.outlines.empty()) {
            shape.boundsMin = shape.boundsMax = shape.outlines.front().front();
            for (const auto& ring : shape.outlines) {
                for (const MapPoint& point : ring) {
                    shape.boundsMin = {std::min(shape.boundsMin.x, point.x), std::min(shape.boundsMin.y, point.y)};
                    shape.boundsMax = {std::max(shape.boundsMax.x, point.x), std::max(shape.boundsMax.y, point.y)};
                }
            }
        }
        shapes.push_back(std::move(shape));
    }
    return shapes;
}

}  // namespace alarmmap
