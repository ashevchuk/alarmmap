#pragma once

#include "projection.hpp"
#include "vector_tile.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alarmmap {

// Basemap content of OpenFreeMap tiles (OpenMapTiles schema) reduced to
// what this map draws, ready for rendering on the map plane.

struct TileId {
    int z = 0;
    int x = 0;
    int y = 0;

    auto operator<=>(const TileId&) const = default;
};

std::string toString(const TileId& id);

// Tiles of zoom `z` that intersect the map-plane rectangle, nearest to its
// centre first so the middle of the screen fills in first.
std::vector<TileId> tilesCovering(MapPoint min, MapPoint max, int z);

enum class BasemapFill { Water, Wood, Wetland, Residential, Industrial };
enum class BasemapLine { River, Stream, Motorway, Trunk, Primary, Secondary, Tertiary, Minor, Rail };

inline constexpr BasemapFill kAllBasemapFills[] = {BasemapFill::Water, BasemapFill::Wood,
                                                   BasemapFill::Wetland, BasemapFill::Residential,
                                                   BasemapFill::Industrial};
inline constexpr BasemapLine kAllBasemapLines[] = {
    BasemapLine::River,     BasemapLine::Stream,   BasemapLine::Motorway,
    BasemapLine::Trunk,     BasemapLine::Primary,  BasemapLine::Secondary,
    BasemapLine::Tertiary,  BasemapLine::Minor,    BasemapLine::Rail};

struct FillMesh {
    std::vector<MapPoint> vertices;
    // Triangle list into vertices.
    std::vector<std::uint32_t> indices;
};

struct BasemapTile {
    TileId id;
    // Indexed by BasemapFill and BasemapLine.
    std::vector<FillMesh> fills = std::vector<FillMesh>(std::size(kAllBasemapFills));
    std::vector<std::vector<std::vector<MapPoint>>> lines =
        std::vector<std::vector<std::vector<MapPoint>>>(std::size(kAllBasemapLines));
};

// Classifies the tile's features and converts them to the map plane;
// features this map does not draw are dropped.
BasemapTile buildBasemapTile(const TileId& id, const std::vector<TileLayer>& layers);

}  // namespace alarmmap
