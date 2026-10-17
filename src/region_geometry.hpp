#pragma once

#include "projection.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace alarmmap {

inline constexpr int kOblastRank = 1;
inline constexpr int kRaionRank = 2;
inline constexpr int kHromadaRank = 3;

// One administrative region projected onto the map plane.
struct RegionShape {
    std::string id;
    std::string name;
    int rank = 0;
    // Triangle list: every three indices form one filled triangle.
    std::vector<MapPoint> vertices;
    std::vector<std::uint32_t> indices;
    // Closed rings (outer borders and holes); the first point is not repeated.
    std::vector<std::vector<MapPoint>> outlines;
    // Bounding box of the outlines.
    MapPoint boundsMin;
    MapPoint boundsMax;
    // Unsimplified polygons, each an outer ring followed by its holes, for
    // exact hit tests; always the site's geometry, even when the drawn one
    // is overridden.
    std::vector<std::vector<std::vector<MapPoint>>> polygons;
};

// Douglas-Peucker simplification of a closed ring. A repeated closing point
// is accepted. Returns an empty ring when fewer than three points survive.
std::vector<MapPoint> simplifyRing(const std::vector<MapPoint>& ring, double tolerance);

// Point inside the drawn region farthest from its borders, where a label
// fits best; unlike the centroid it never falls into a hole or a bay.
// Requires a shape with outlines.
MapPoint labelPoint(const RegionShape& shape);

// Replaces the drawn geometry of features in the `regions` FeatureCollection
// with that of the `overrides` features carrying the same properties.regionId.
// The site's own geometry is kept for hit tests, so alert filtering still
// matches the site. Throws std::runtime_error when an override names a
// missing region.
void overrideRegionGeometry(nlohmann::json& regions, const nlohmann::json& overrides);

// Builds shapes from the site's regions GeoJSON (properties regionId, name,
// rank). Tolerance is in map-plane units. Throws std::runtime_error on
// features it cannot interpret.
std::vector<RegionShape> buildRegionShapes(const nlohmann::json& geojson, double tolerance);

}  // namespace alarmmap
