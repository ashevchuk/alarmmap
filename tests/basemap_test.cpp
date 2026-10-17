// Usage: basemap_test <tests/data directory>
//
// tile_12_2392_1383.pbf is an OpenFreeMap tile; the .json next to it holds
// what an independent Python decoder read from it.

#include "basemap.hpp"
#include "projection.hpp"
#include "tile_store.hpp"
#include "vector_tile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <source_location>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace alarmmap;

namespace {

int failures = 0;

void check(bool condition, const std::string& what,
           std::source_location location = std::source_location::current()) {
    if (condition) return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
}

void checkThrows(const std::function<void()>& action, const std::string& what,
                 std::source_location location = std::source_location::current()) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    check(false, what + " (no exception)", location);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

void testMatchesReference(const std::filesystem::path& dataDirectory) {
    const auto layers = decodeVectorTile(readFile(dataDirectory / "tile_12_2392_1383.pbf"));
    const json reference = json::parse(readFile(dataDirectory / "tile_12_2392_1383.json"));
    check(layers.size() == reference.size(), "layer count");
    for (size_t i = 0; i < layers.size() && i < reference.size(); ++i) {
        const TileLayer& layer = layers[i];
        const json& expected = reference[i];
        const std::string name = expected["name"];
        check(layer.name == name, "layer name " + name);
        check(layer.extent == expected["extent"].get<double>(), name + " extent");
        check(layer.features.size() == expected["features"].get<size_t>(), name + " feature count");

        size_t points = 0, polygons = 0;
        for (const TileFeature& feature : layer.features) {
            for (const TileRing& part : feature.parts) points += part.size();
            if (feature.type == TileGeometryType::Polygon) polygons += groupPolygonRings(feature.parts).size();
        }
        check(points == expected["points"].get<size_t>(), name + " point count");
        check(polygons == expected["polygons"].get<size_t>(), name + " polygon count");

        const json& samples = expected["sample"];
        for (size_t f = 0; f < samples.size() && f < layer.features.size(); ++f) {
            const TileFeature& feature = layer.features[f];
            check(static_cast<int>(feature.type) == samples[f]["type"].get<int>(), name + " sample type");
            check(feature.properties == samples[f]["properties"], name + " sample properties");
            bool same = feature.parts.size() == samples[f]["parts"].size();
            for (size_t p = 0; same && p < feature.parts.size(); ++p) {
                const json& part = samples[f]["parts"][p];
                same = feature.parts[p].size() == part.size();
                for (size_t k = 0; same && k < part.size(); ++k) {
                    same = feature.parts[p][k] ==
                           TilePoint{part[k][0].get<double>(), part[k][1].get<double>()};
                }
            }
            check(same, name + " sample geometry");
        }
    }
}

// Minimal protobuf writer for hand-made tiles.
std::string varint(std::uint64_t value) {
    std::string out;
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
    return out;
}
std::string field(std::uint32_t number, std::uint32_t wireType) { return varint(number << 3 | wireType); }
std::string bytesField(std::uint32_t number, const std::string& payload) {
    return field(number, 2) + varint(payload.size()) + payload;
}
std::string varintField(std::uint32_t number, std::uint64_t value) { return field(number, 0) + varint(value); }
std::string packed(const std::vector<std::uint32_t>& values) {
    std::string out;
    for (const auto value : values) out += varint(value);
    return out;
}
std::uint32_t zigzag(std::int32_t value) {
    return static_cast<std::uint32_t>((value << 1) ^ (value >> 31));
}
std::uint32_t command(std::uint32_t id, std::uint32_t count) { return id | count << 3; }

std::string squareWithHoleTile(const std::string& tags) {
    // Exterior 0,0 -> 10,0 -> 10,10 -> 0,10 (clockwise with y down), hole
    // 3,3 -> 3,7 -> 7,7 -> 7,3 (counter-clockwise).
    const std::vector<std::uint32_t> geometry = {
        command(1, 1), zigzag(0), zigzag(0),
        command(2, 3), zigzag(10), zigzag(0), zigzag(0), zigzag(10), zigzag(-10), zigzag(0),
        command(7, 1),
        command(1, 1), zigzag(3), zigzag(-7),
        command(2, 3), zigzag(0), zigzag(4), zigzag(4), zigzag(0), zigzag(0), zigzag(-4),
        command(7, 1)};
    const std::string feature = bytesField(2, tags) + varintField(3, 3) + bytesField(4, packed(geometry));
    const std::string layer = bytesField(1, "water") + bytesField(2, feature) + bytesField(3, "class") +
                              bytesField(4, bytesField(1, "lake")) + varintField(5, 4096);
    return bytesField(3, layer);
}

void testHandMadeTile() {
    const auto layers = decodeVectorTile(squareWithHoleTile(packed({0, 0})));
    check(layers.size() == 1 && layers[0].name == "water" && layers[0].extent == 4096, "layer header");
    if (layers.empty() || layers[0].features.empty()) return;
    const TileFeature& feature = layers[0].features[0];
    check(feature.type == TileGeometryType::Polygon, "polygon type");
    check(feature.properties == json{{"class", "lake"}}, "string property");
    check(feature.parts.size() == 2 && feature.parts[1][0] == TilePoint{3, 3} &&
              feature.parts[1][3] == TilePoint{7, 3},
          "relative coordinates, negative deltas included");
    const auto polygons = groupPolygonRings(feature.parts);
    check(polygons.size() == 1 && polygons[0].size() == 2, "hole attached to its exterior ring");

    checkThrows([] { decodeVectorTile(squareWithHoleTile(packed({0, 5}))); }, "tag value out of range");
    const std::string tile = squareWithHoleTile(packed({0, 0}));
    checkThrows([&] { decodeVectorTile(tile.substr(0, tile.size() - 5)); }, "truncated tile");
}

double ringArea(const std::vector<MapPoint>& ring) {
    double twiceArea = 0;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        twiceArea += ring[j].x * ring[i].y - ring[i].x * ring[j].y;
    }
    return std::abs(twiceArea) / 2;
}

void testTilesCovering() {
    const MapPoint kyiv = lonLatToMap(30.52, 50.45);
    const auto single = tilesCovering(kyiv, kyiv, 10);
    check(single.size() == 1 && single[0] == TileId{10, 598, 345}, "tile under Kyiv");

    const auto block = tilesCovering({kyiv.x - 10, kyiv.y - 10}, {kyiv.x + 10, kyiv.y + 10}, 10);
    check(block.size() > 4, "area spans several tiles");
    const bool unique = std::adjacent_find(block.begin(), block.end()) == block.end();
    check(unique && std::find(block.begin(), block.end(), TileId{10, 598, 345}) != block.end(),
          "covering includes the centre tile");
    const auto distance = [&](const TileId& id) { return std::hypot(id.x - 598.0, id.y - 345.0); };
    check(distance(block.front()) <= distance(block.back()), "centre tiles first");

    const auto world = tilesCovering({-1e6, -1e6}, {1e6, 1e6}, 2);
    check(world.size() == 16, "covering is clamped to the world");
    check(toString(TileId{12, 2392, 1383}) == "12/2392/1383", "tile id text");
}

void testBuildBasemapTile(const std::filesystem::path& dataDirectory) {
    const TileId id{12, 2392, 1383};
    const auto layers = decodeVectorTile(readFile(dataDirectory / "tile_12_2392_1383.pbf"));
    const BasemapTile tile = buildBasemapTile(id, layers);

    // Water triangles must cover exactly the water polygons minus their holes.
    double expectedWater = 0;
    const double tiles = std::ldexp(1.0, id.z);
    for (const TileLayer& layer : layers) {
        if (layer.name != "water") continue;
        for (const TileFeature& feature : layer.features) {
            for (const auto& polygon : groupPolygonRings(feature.parts)) {
                for (size_t r = 0; r < polygon.size(); ++r) {
                    std::vector<MapPoint> ring;
                    for (const TilePoint& point : polygon[r]) {
                        ring.push_back(worldToMap((id.x + point.x / layer.extent) / tiles,
                                                  (id.y + point.y / layer.extent) / tiles));
                    }
                    expectedWater += (r == 0 ? 1 : -1) * ringArea(ring);
                }
            }
        }
    }
    const FillMesh& water = tile.fills[static_cast<size_t>(BasemapFill::Water)];
    double waterArea = 0;
    for (size_t i = 0; i + 2 < water.indices.size(); i += 3) {
        waterArea += ringArea({water.vertices[water.indices[i]], water.vertices[water.indices[i + 1]],
                               water.vertices[water.indices[i + 2]]});
    }
    check(expectedWater > 0 && std::abs(waterArea - expectedWater) <= expectedWater * 1e-6,
          "water triangles cover the water polygons");

    // Everything lies on this tile, give or take the tile buffer.
    const MapPoint corner = worldToMap(id.x / tiles, id.y / tiles);
    const MapPoint opposite = worldToMap((id.x + 1) / tiles, (id.y + 1) / tiles);
    const double margin = (opposite.x - corner.x) * 0.25;
    bool inside = true;
    for (const auto& lines : tile.lines) {
        for (const auto& line : lines) {
            for (const MapPoint& point : line) {
                inside = inside && point.x >= corner.x - margin && point.x <= opposite.x + margin &&
                         point.y >= corner.y - margin && point.y <= opposite.y + margin;
            }
        }
    }
    check(inside, "lines stay on the tile");
    check(!tile.lines[static_cast<size_t>(BasemapLine::River)].empty() ||
              !tile.lines[static_cast<size_t>(BasemapLine::Stream)].empty(),
          "waterways classified");
    size_t roads = 0;
    for (const BasemapLine line : {BasemapLine::Motorway, BasemapLine::Trunk, BasemapLine::Primary,
                                   BasemapLine::Secondary, BasemapLine::Tertiary, BasemapLine::Minor}) {
        roads += tile.lines[static_cast<size_t>(line)].size();
    }
    check(roads > 0, "roads classified");
}

// Waits until the store has the tile or a few seconds pass.
std::shared_ptr<const BasemapTile> waitForTile(const TileStore& store, const TileId& id) {
    for (int attempt = 0; attempt < 500; ++attempt) {
        if (auto tile = store.get(id)) return tile;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return nullptr;
}

void testTileStore(const std::filesystem::path& dataDirectory) {
    namespace fs = std::filesystem;
    const fs::path cache = fs::temp_directory_path() /
                           ("alarmmap_tile_store_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::string tileData = readFile(dataDirectory / "tile_12_2392_1383.pbf");
    const TileId present{12, 2392, 1383}, missing{12, 0, 0};
    std::atomic<int> tileJsonFetches = 0, tileFetches = 0;
    const auto online = [&](const std::string& url) -> std::string {
        if (url == "test://tilejson") {
            ++tileJsonFetches;
            return R"({"tiles":["test://v1/{z}/{x}/{y}.pbf"]})";
        }
        if (url == "test://v1/12/2392/1383.pbf") {
            ++tileFetches;
            return tileData;
        }
        throw std::runtime_error("not found: " + url);
    };
    const auto offline = [](const std::string& url) -> std::string { throw std::runtime_error("offline: " + url); };

    {
        TileStore store({"test://tilejson", cache, online, 10, 2});
        store.want({present, missing});
        const auto tile = waitForTile(store, present);
        check(tile && !tile->fills[static_cast<size_t>(BasemapFill::Water)].indices.empty(),
              "tile fetched and prepared");
        for (int attempt = 0; attempt < 200 && store.lastError().empty(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(!store.get(missing) && store.lastError().find("12/0/0") != std::string::npos,
              "failed tile reported, not stored");
        check(store.version() >= 1, "version grows");
    }
    check(tileJsonFetches == 1 && tileFetches == 1, "one fetch each");

    {
        TileStore store({"test://tilejson", cache, offline, 10, 2});
        store.want({present});
        check(waitForTile(store, present) != nullptr, "tile served from the disk cache while offline");
    }

    // A damaged cached copy is replaced from the network.
    for (const auto& entry : fs::recursive_directory_iterator(cache)) {
        if (entry.path().extension() == ".pbf") std::ofstream(entry.path(), std::ios::binary) << "garbage";
    }
    {
        TileStore store({"test://tilejson", cache, online, 10, 2});
        store.want({present});
        check(waitForTile(store, present) != nullptr && tileFetches == 2, "damaged cache refetched");
    }
    fs::remove_all(cache);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: basemap_test <data directory>\n";
        return 2;
    }
    try {
        testMatchesReference(argv[1]);
        testHandMadeTile();
        testTilesCovering();
        testBuildBasemapTile(argv[1]);
        testTileStore(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
