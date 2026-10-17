#include "basemap.hpp"

#include <mapbox/earcut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>

namespace alarmmap {


namespace {

std::string_view stringProperty(const TileFeature& feature, const char* key) {
    const auto found = feature.properties.find(key);
    if (found == feature.properties.end() || !found->is_string()) return {};
    return found->get_ref<const std::string&>();
}

bool inTunnel(const TileFeature& feature) { return stringProperty(feature, "brunnel") == "tunnel"; }

std::optional<BasemapFill> fillFor(std::string_view layer, const TileFeature& feature) {
    const std::string_view kind = stringProperty(feature, "class");
    if (layer == "water") return BasemapFill::Water;
    if (layer == "landcover") {
        if (kind == "wood") return BasemapFill::Wood;
        if (kind == "wetland") return BasemapFill::Wetland;
    }
    if (layer == "landuse") {
        if (kind == "residential" || kind == "suburb" || kind == "neighbourhood") return BasemapFill::Residential;
        if (kind == "industrial" || kind == "commercial" || kind == "retail" || kind == "railway") {
            return BasemapFill::Industrial;
        }
    }
    return std::nullopt;
}

std::optional<BasemapLine> lineFor(std::string_view layer, const TileFeature& feature) {
    if (inTunnel(feature)) return std::nullopt;
    const std::string_view kind = stringProperty(feature, "class");
    if (layer == "waterway") {
        if (kind == "river" || kind == "canal") return BasemapLine::River;
        if (kind == "stream") return BasemapLine::Stream;
        return std::nullopt;
    }
    if (layer != "transportation") return std::nullopt;
    if (kind == "motorway") return BasemapLine::Motorway;
    if (kind == "trunk") return BasemapLine::Trunk;
    if (kind == "primary") return BasemapLine::Primary;
    if (kind == "secondary") return BasemapLine::Secondary;
    if (kind == "tertiary") return BasemapLine::Tertiary;
    if (kind == "minor") return BasemapLine::Minor;
    if (kind == "rail" && stringProperty(feature, "subclass") == "rail") return BasemapLine::Rail;
    return std::nullopt;
}

}  // namespace

std::string toString(const TileId& id) {
    return std::to_string(id.z) + '/' + std::to_string(id.x) + '/' + std::to_string(id.y);
}

std::vector<TileId> tilesCovering(MapPoint min, MapPoint max, int z) {
    const double tiles = std::ldexp(1.0, z);
    const MapPoint worldMin = mapToWorld(min), worldMax = mapToWorld(max);
    const auto clampTile = [tiles](double value) {
        return static_cast<int>(std::clamp(std::floor(value * tiles), 0.0, tiles - 1));
    };
    const int left = clampTile(std::min(worldMin.x, worldMax.x));
    const int right = clampTile(std::max(worldMin.x, worldMax.x));
    const int top = clampTile(std::min(worldMin.y, worldMax.y));
    const int bottom = clampTile(std::max(worldMin.y, worldMax.y));

    std::vector<TileId> ids;
    for (int y = top; y <= bottom; ++y) {
        for (int x = left; x <= right; ++x) ids.push_back({z, x, y});
    }
    const double centerX = (left + right) / 2.0, centerY = (top + bottom) / 2.0;
    std::stable_sort(ids.begin(), ids.end(), [&](const TileId& a, const TileId& b) {
        return std::hypot(a.x - centerX, a.y - centerY) < std::hypot(b.x - centerX, b.y - centerY);
    });
    return ids;
}

BasemapTile buildBasemapTile(const TileId& id, const std::vector<TileLayer>& layers) {
    BasemapTile tile;
    tile.id = id;
    const double tiles = std::ldexp(1.0, id.z);
    for (const TileLayer& layer : layers) {
        const auto toMap = [&](const TilePoint& point) {
            return worldToMap((id.x + point.x / layer.extent) / tiles, (id.y + point.y / layer.extent) / tiles);
        };
        for (const TileFeature& feature : layer.features) {
            if (feature.type == TileGeometryType::Polygon) {
                const auto fill = fillFor(layer.name, feature);
                if (!fill) continue;
                FillMesh& mesh = tile.fills[static_cast<size_t>(*fill)];
                for (const auto& polygon : groupPolygonRings(feature.parts)) {
                    std::vector<std::vector<std::array<double, 2>>> rings;
                    const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
                    for (const TileRing& ring : polygon) {
                        auto& converted = rings.emplace_back();
                        for (const TilePoint& point : ring) {
                            const MapPoint mapped = toMap(point);
                            converted.push_back({mapped.x, mapped.y});
                            mesh.vertices.push_back(mapped);
                        }
                    }
                    for (const std::uint32_t index : mapbox::earcut<std::uint32_t>(rings)) {
                        mesh.indices.push_back(base + index);
                    }
                }
            } else if (feature.type == TileGeometryType::LineString) {
                const auto line = lineFor(layer.name, feature);
                if (!line) continue;
                auto& lines = tile.lines[static_cast<size_t>(*line)];
                for (const TileRing& part : feature.parts) {
                    if (part.size() < 2) continue;
                    auto& converted = lines.emplace_back();
                    for (const TilePoint& point : part) converted.push_back(toMap(point));
                }
            }
        }
    }
    return tile;
}

}  // namespace alarmmap
