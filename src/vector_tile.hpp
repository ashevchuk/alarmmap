#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace alarmmap {

// Mapbox Vector Tile 2.1 (protobuf), as served by OpenFreeMap.

enum class TileGeometryType { Unknown = 0, Point = 1, LineString = 2, Polygon = 3 };

// Position in tile units, 0..extent, y pointing down.
struct TilePoint {
    double x = 0;
    double y = 0;

    bool operator==(const TilePoint&) const = default;
};

using TileRing = std::vector<TilePoint>;

struct TileFeature {
    TileGeometryType type = TileGeometryType::Unknown;
    // Attribute values as strings, numbers or booleans.
    nlohmann::json properties = nlohmann::json::object();
    // One entry per point, line or polygon ring; rings are not closed by a
    // repeated first point.
    std::vector<TileRing> parts;
};

struct TileLayer {
    std::string name;
    double extent = 4096;
    std::vector<TileFeature> features;
};

// Throws std::runtime_error on malformed data.
std::vector<TileLayer> decodeVectorTile(std::string_view data);

// Groups polygon rings into polygons, each the exterior ring followed by its
// holes. Per the specification, exterior rings wind clockwise in tile
// coordinates and holes counter-clockwise; rings without area are dropped.
std::vector<std::vector<TileRing>> groupPolygonRings(const std::vector<TileRing>& rings);

}  // namespace alarmmap
