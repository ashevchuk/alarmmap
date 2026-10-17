// Usage: basemap_test <tests/data directory>
//
// tile_12_2392_1383.pbf is an OpenFreeMap tile; the .json next to it holds
// what an independent Python decoder read from it.

#include "vector_tile.hpp"

#include <nlohmann/json.hpp>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: basemap_test <data directory>\n";
        return 2;
    }
    try {
        testMatchesReference(argv[1]);
        testHandMadeTile();
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
