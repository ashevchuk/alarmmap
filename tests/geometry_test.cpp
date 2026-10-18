// Usage: geometry_test <tests/data directory>

#include "cities.hpp"
#include "map_update.hpp"
#include "map_view.hpp"
#include "projection.hpp"
#include "region_geometry.hpp"
#include "region_store.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <numbers>
#include <iostream>
#include <source_location>
#include <stdexcept>
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

bool near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

double ringArea(const std::vector<MapPoint>& ring) {
    double twiceArea = 0;
    for (size_t i = 0; i < ring.size(); ++i) {
        const MapPoint& a = ring[i];
        const MapPoint& b = ring[(i + 1) % ring.size()];
        twiceArea += a.x * b.y - b.x * a.y;
    }
    return std::abs(twiceArea) / 2;
}

double triangleArea(const RegionShape& shape) {
    double area = 0;
    for (size_t i = 0; i + 2 < shape.indices.size(); i += 3) {
        area += ringArea({shape.vertices[shape.indices[i]], shape.vertices[shape.indices[i + 1]],
                          shape.vertices[shape.indices[i + 2]]});
    }
    return area;
}

json squareRing(double west, double south, double size) {
    return json::array({json::array({west, south}), json::array({west + size, south}),
                        json::array({west + size, south + size}),
                        json::array({west, south + size}), json::array({west, south})});
}

json feature(const std::string& id, int rank, const json& geometry) {
    return {{"type", "Feature"},
            {"properties", {{"regionId", id}, {"name", "region " + id}, {"rank", rank}}},
            {"geometry", geometry}};
}

void testProjectionMatchesSite() {
    // Expected values come from the site's lngLatToNormalized (detailKyivMap.js).
    struct Sample {
        double longitude, latitude, x, y;
    };
    const Sample samples[] = {
        {30.5234, 50.4501, 0.46409199938638285, 0.25047252420082194},
        {22.137059, 52.3791473, 0.007076059012474353, 0},
        {40.2275801, 44.184598, 0.9929239409875257, 1},
        {36.2304, 49.9935, 0.77509652110973, 0.30825265224341847},
        {24.0316, 49.8429, 0.11031958758970788, 0.3271897851158779},
    };
    for (const Sample& sample : samples) {
        const MapPoint point = lonLatToMap(sample.longitude, sample.latitude);
        check(near(point.x / kMapWidth, sample.x, 1e-12) &&
                  near(point.y / kMapHeight, sample.y, 1e-12),
              "projection of " + std::to_string(sample.longitude) + "," +
                  std::to_string(sample.latitude));
    }
    check(normalizedToMap(0.5, 1) == MapPoint{kMapWidth / 2, kMapHeight}, "normalized scaling");

    // Kyiv on the Web Mercator world square, computed independently.
    const double kyivWorldX = (30.5234 + 180) / 360;
    const double kyivWorldY =
        (1 - std::log(std::tan(std::numbers::pi / 4 + 50.4501 * std::numbers::pi / 360)) / std::numbers::pi) / 2;
    const MapPoint viaWorld = worldToMap(kyivWorldX, kyivWorldY);
    const MapPoint direct = lonLatToMap(30.5234, 50.4501);
    check(near(viaWorld.x, direct.x, 1e-9) && near(viaWorld.y, direct.y, 1e-9), "world square to map plane");
    const MapPoint back = mapToWorld(direct);
    check(near(back.x, kyivWorldX, 1e-12) && near(back.y, kyivWorldY, 1e-12), "map plane to world square");

    // At zoom z a 256-pixel tile spans 1/2^z of the world.
    const MapPoint tileStart = worldToMap(0, 0), tileEnd = worldToMap(1.0 / 64, 0);
    const double pixelsPerUnit = 256 / (tileEnd.x - tileStart.x);
    check(near(tileZoomForScale(pixelsPerUnit), 6, 1e-9), "tile zoom for scale");
}

void testSimplifyRing() {
    std::vector<MapPoint> square;
    for (int i = 0; i < 10; ++i) square.push_back({double(i), 0});
    for (int i = 0; i < 10; ++i) square.push_back({10, double(i)});
    for (int i = 10; i > 0; --i) square.push_back({double(i), 10});
    for (int i = 10; i > 0; --i) square.push_back({0, double(i)});
    square.push_back(square.front());

    const auto simplified = simplifyRing(square, 0.01);
    check(simplified.size() == 4, "collinear points collapse to the four corners");
    check(near(ringArea(simplified), 100, 1e-9), "simplified square keeps its area");

    std::vector<MapPoint> zigzag = {{0, 0}, {5, 0.5}, {10, 0}, {10, 10}, {0, 10}};
    check(simplifyRing(zigzag, 0.1).size() == 5, "deviation above tolerance is kept");
    check(simplifyRing(zigzag, 1).size() == 4, "deviation below tolerance is dropped");

    check(simplifyRing({{0, 0}, {1, 1}, {0, 0}}, 0.01).empty(), "degenerate ring is dropped");
    check(simplifyRing({{0, 0}, {0.001, 0}, {0.001, 0.001}, {0, 0.001}}, 0.1).empty(),
          "ring smaller than tolerance collapses");
}

void testBuildShapes() {
    const json polygonWithHole = {
        {"type", "Polygon"},
        {"coordinates", json::array({squareRing(30, 48, 1), squareRing(30.25, 48.25, 0.5)})}};
    const json multiPolygon = {
        {"type", "MultiPolygon"},
        {"coordinates", json::array({json::array({squareRing(25, 46, 0.5)}),
                                     json::array({squareRing(26, 46, 0.5)}),
                                     json::array({squareRing(27, 46, 0.00001)})})}};
    const json collection = {
        {"type", "FeatureCollection"},
        {"features", json::array({feature("7", kOblastRank, polygonWithHole),
                                  feature("42", kRaionRank, multiPolygon)})}};

    const auto shapes = buildRegionShapes(collection, 0.001);
    check(shapes.size() == 2, "two shapes");
    if (shapes.size() != 2) return;

    const RegionShape& holed = shapes[0];
    check(holed.id == "7" && holed.rank == kOblastRank && holed.name == "region 7", "properties");
    const MapPoint corner = lonLatToMap(30, 49), opposite = lonLatToMap(31, 48);
    check(near(holed.boundsMin.x, corner.x, 1e-9) && near(holed.boundsMin.y, corner.y, 1e-9) &&
              near(holed.boundsMax.x, opposite.x, 1e-9) && near(holed.boundsMax.y, opposite.y, 1e-9),
          "bounds span the outer ring");
    check(holed.outlines.size() == 2, "outer ring and hole outlines");
    if (holed.outlines.size() == 2) {
        const double expected = ringArea(holed.outlines[0]) - ringArea(holed.outlines[1]);
        check(expected > 0 && near(triangleArea(holed), expected, expected * 1e-9),
              "triangles cover the polygon minus its hole");
    }

    const RegionShape& multi = shapes[1];
    check(multi.outlines.size() == 2, "collapsed part dropped, two parts kept");
    if (multi.outlines.size() == 2) {
        const double expected = ringArea(multi.outlines[0]) + ringArea(multi.outlines[1]);
        check(near(triangleArea(multi), expected, expected * 1e-9), "multipolygon area");
    }
    for (const auto index : multi.indices) {
        check(index < multi.vertices.size(), "index within vertex range");
    }

    const json unsupported = {
        {"features", json::array({feature("1", kOblastRank, {{"type", "LineString"},
                                                             {"coordinates", json::array()}})})}};
    bool threw = false;
    try {
        buildRegionShapes(unsupported, 0.001);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "unsupported geometry type throws");
}

void testOverrideRegionGeometry() {
    const json original = {{"type", "Polygon"}, {"coordinates", json::array({squareRing(30, 48, 1)})}};
    const json replacement = {{"type", "Polygon"}, {"coordinates", json::array({squareRing(31, 47, 0.5)})}};
    json regions = {{"type", "FeatureCollection"},
                    {"features", json::array({feature("1", kOblastRank, original),
                                              feature("9999", kOblastRank, original)})}};
    const json overrides = {
        {"type", "FeatureCollection"},
        {"features", json::array({{{"type", "Feature"},
                                   {"properties", {{"regionId", "9999"}}},
                                   {"geometry", replacement}}})}};

    overrideRegionGeometry(regions, overrides);
    check(regions["features"][0]["geometry"] == original, "other regions untouched");
    check(regions["features"][1]["geometry"] == replacement, "geometry replaced");
    check(regions["features"][1]["properties"]["name"] == "region 9999", "properties kept");

    const auto shapes = buildRegionShapes(regions, 0.001);
    check(shapes.size() == 2 && shapes[1].outlines.size() == 1 && shapes[1].polygons.size() == 1,
          "overridden shape built");
    if (shapes.size() == 2 && !shapes[1].outlines.empty() && !shapes[1].polygons.empty()) {
        const double drawnArea = ringArea(shapes[1].outlines[0]);
        const double hitArea = ringArea(shapes[1].polygons[0][0]);
        check(near(drawnArea, ringArea(shapes[0].outlines[0]) / 4, drawnArea * 0.05),
              "drawn outline is the override");
        check(near(hitArea, ringArea(shapes[0].polygons[0][0]), hitArea * 1e-9),
              "hit test polygon stays the site's geometry");
    }
    overrideRegionGeometry(regions, overrides);
    check(buildRegionShapes(regions, 0.001)[1].polygons == shapes[1].polygons,
          "repeated override keeps the original site geometry");

    json unknown = overrides;
    unknown["features"][0]["properties"]["regionId"] = "404";
    bool threw = false;
    try {
        overrideRegionGeometry(regions, unknown);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "override of a missing region throws");
}

bool insideRing(const std::vector<MapPoint>& ring, const MapPoint& point) {
    bool inside = false;
    for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
        if ((ring[j].y > point.y) != (ring[i].y > point.y) &&
            point.x < (ring[i].x - ring[j].x) * (point.y - ring[j].y) / (ring[i].y - ring[j].y) + ring[j].x) {
            inside = !inside;
        }
    }
    return inside;
}

void testLabelPoint() {
    RegionShape holed;
    holed.outlines = {{{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {{3, 3}, {7, 3}, {7, 7}, {3, 7}}};
    const MapPoint holedLabel = labelPoint(holed);
    check(insideRing(holed.outlines[0], holedLabel) && !insideRing(holed.outlines[1], holedLabel),
          "label inside the region, outside its hole");
    const double clearance = std::min({holedLabel.x, 10 - holedLabel.x, holedLabel.y, 10 - holedLabel.y});
    check(clearance > 1.2, "label keeps away from the outer border");

    // An L whose bounding-box centre lies outside the shape.
    RegionShape bent;
    bent.outlines = {{{0, 0}, {10, 0}, {10, 2}, {2, 2}, {2, 10}, {0, 10}}};
    check(insideRing(bent.outlines[0], labelPoint(bent)), "label inside a concave region");

    RegionShape empty;
    empty.id = "0";
    bool threw = false;
    try {
        labelPoint(empty);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "region without outline throws");
}

void testPreparedRegions() {
    const json collection = {
        {"type", "FeatureCollection"},
        {"features",
         json::array({feature("7", kOblastRank,
                              {{"type", "Polygon"},
                               {"coordinates", json::array({squareRing(30, 48, 1), squareRing(30.25, 48.25, 0.5)})}}),
                      feature("42", kRaionRank,
                              {{"type", "Polygon"}, {"coordinates", json::array({squareRing(25, 46, 0.5)})}})})}};
    const PreparedRegions original{buildRegionShapes(collection, 0.001), buildRegionShapes(collection, 0.0001)};
    const std::string bytes = writePreparedRegions(original);
    const PreparedRegions restored = readPreparedRegions(bytes);

    // Points come back within half a quantization step: 1/65535 of the
    // shape's extent, here taken from its bounds and its exact polygons.
    double tolerance = 0;
    const auto samePoints = [&](const std::vector<MapPoint>& a, const std::vector<MapPoint>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (!near(a[i].x, b[i].x, tolerance) || !near(a[i].y, b[i].y, tolerance)) return false;
        }
        return true;
    };
    const auto sameShape = [&](const RegionShape& a, const RegionShape& b, bool withPolygons) {
        MapPoint low = a.boundsMin;
        MapPoint high = a.boundsMax;
        for (const auto& polygon : a.polygons) {
            for (const auto& ring : polygon) {
                for (const MapPoint& point : ring) {
                    low = {std::min(low.x, point.x), std::min(low.y, point.y)};
                    high = {std::max(high.x, point.x), std::max(high.y, point.y)};
                }
            }
        }
        tolerance = std::max(high.x - low.x, high.y - low.y) / 65535 / 2 + 1e-12;
        bool same = a.id == b.id && a.name == b.name && a.rank == b.rank && a.indices == b.indices &&
                    samePoints(a.vertices, b.vertices) && a.outlines.size() == b.outlines.size() &&
                    samePoints({a.boundsMin, a.boundsMax}, {b.boundsMin, b.boundsMax});
        for (size_t i = 0; same && i < a.outlines.size(); ++i) same = samePoints(a.outlines[i], b.outlines[i]);
        if (!withPolygons) return same && b.polygons.empty();
        same = same && a.polygons.size() == b.polygons.size();
        for (size_t i = 0; same && i < a.polygons.size(); ++i) {
            same = a.polygons[i].size() == b.polygons[i].size();
            for (size_t j = 0; same && j < a.polygons[i].size(); ++j) {
                same = samePoints(a.polygons[i][j], b.polygons[i][j]);
            }
        }
        return same;
    };
    check(restored.coarse.size() == 2 && restored.detailed.size() == 2, "both levels come back");
    for (size_t i = 0; i < std::min(restored.coarse.size(), original.coarse.size()); ++i) {
        check(sameShape(original.coarse[i], restored.coarse[i], true), "coarse shape survives with its polygons");
    }
    for (size_t i = 0; i < std::min(restored.detailed.size(), original.detailed.size()); ++i) {
        check(sameShape(original.detailed[i], restored.detailed[i], false),
              "detailed shape survives without polygons");
    }

    const auto rejects = [](std::string_view data) {
        try {
            readPreparedRegions(data);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    check(rejects(std::string_view(bytes).substr(0, bytes.size() - 1)), "truncated data is rejected");
    check(rejects(bytes + "x"), "trailing data is rejected");
    check(rejects("NOTGEO__" + bytes.substr(8)), "a foreign file is rejected");
    check(rejects(""), "empty data is rejected");
    check(rejects(std::string("ALARMGEO") + std::string(11, '\xff')), "an endless number is rejected");

    // An outline longer than the bytes left after it: ring lengths count
    // vertices, which precede them.
    std::vector<json> ring;
    for (int i = 0; i < 64; ++i) {
        const double angle = 2 * std::numbers::pi * i / 64;
        ring.push_back({30 + std::cos(angle), 48 + std::sin(angle)});
    }
    ring.push_back(ring.front());
    const json round = {{"type", "FeatureCollection"},
                        {"features", json::array({feature("9", kOblastRank,
                                                          {{"type", "Polygon"},
                                                           {"coordinates", json::array({json(ring)})}})})}};
    const PreparedRegions circle{buildRegionShapes(round, 0.0001), buildRegionShapes(round, 0.0001)};
    check(!rejects(writePreparedRegions(circle)), "a long last outline reads back");
}

void testMapView() {
    MapView view;
    view.setViewport(1600, 1000);
    const double fit = std::min((1600 - 48) / kMapWidth, (1000 - 48) / kMapHeight);
    check(near(view.pixelsPerUnit(), fit, 1e-12), "zoom 1 fits the map");
    const MapPoint middle = view.mapToScreen({kMapWidth / 2, kMapHeight / 2});
    check(near(middle.x, 800, 1e-9) && near(middle.y, 500, 1e-9), "map centre in the middle");

    const MapPoint screen{700, 430};
    const MapPoint anchor = view.screenToMap(screen);
    const MapPoint back = view.mapToScreen(anchor);
    check(near(back.x, screen.x, 1e-9) && near(back.y, screen.y, 1e-9), "screen/map round trip");

    view.zoomAt(screen, 4);
    check(near(view.zoom(), 4, 1e-12), "zoomed in");
    const MapPoint still = view.screenToMap(screen);
    check(near(still.x, anchor.x, 1e-9) && near(still.y, anchor.y, 1e-9), "point under the cursor stays");

    const MapPoint before = view.center();
    view.panByPixels(100, -50);
    check(near(view.center().x, before.x - 100 / view.pixelsPerUnit(), 1e-9) &&
              near(view.center().y, before.y + 50 / view.pixelsPerUnit(), 1e-9),
          "panning moves the centre against the drag");

    view.panByPixels(1e6, 1e6);
    check(near(view.center().x, 0, 1e-12) && near(view.center().y, 0, 1e-12),
          "centre cannot leave the map plane");

    view.zoomAt(screen, 1e6);
    check(near(view.zoom(), MapView::kMaxZoom, 1e-12), "zoom capped");
    view.zoomAt(screen, 1e-6);
    check(near(view.zoom(), MapView::kMinZoom, 1e-12), "zoom floored");

    view.zoomAt({1500, 900}, 4);
    view.zoomAt({100, 100}, 1e-6);
    check(view.center() == MapPoint{kMapWidth / 2, kMapHeight / 2}, "minimum zoom recentres the map");

    view.reset();
    check(view.zoom() == 1 && view.center() == MapPoint{kMapWidth / 2, kMapHeight / 2}, "reset");
}

void testCities() {
    const json collection = {
        {"type", "FeatureCollection"},
        {"features",
         json::array({{{"type", "Feature"},
                       {"properties", {{"name", "Київ"}, {"kind", "capital"}, {"population", 2709000}, {"minZoom", 3.7}}},
                       {"geometry", {{"type", "Point"}, {"coordinates", json::array({30.5234, 50.4501})}}}},
                      {{"type", "Feature"},
                       {"properties", {{"name", "Ніжин"}, {"kind", "town"}, {"population", 76000}, {"minZoom", 7}}},
                       {"geometry", {{"type", "Point"}, {"coordinates", json::array({31.88, 51.05})}}}}})}};
    const auto cities = loadCities(collection);
    check(cities.size() == 2, "two cities");
    json village = collection;
    village["features"][1]["properties"]["kind"] = "village";
    check(loadCities(village)[1].kind == CityKind::Village, "village kind");
    if (cities.size() == 2) {
        check(cities[0].name == "Київ" && cities[0].kind == CityKind::Capital && cities[0].minZoom == 3.7,
              "capital properties");
        check(cities[0].position == lonLatToMap(30.5234, 50.4501), "projected position");
        check(cities[1].kind == CityKind::Town && cities[1].population == 76000, "town properties");
    }

    json badKind = collection;
    badKind["features"][0]["properties"]["kind"] = "hamlet";
    bool threw = false;
    try {
        loadCities(badKind);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "unknown kind throws");

}

void testTimestamps() {
    check(parseUtcTimestampMs("1970-01-01T00:00:00Z") == 0, "epoch");
    check(parseUtcTimestampMs("2022-04-04T16:45:00Z") == 1649090700000LL, "whole seconds");
    check(parseUtcTimestampMs("2026-10-17T21:09:22.806644Z") == 1792271362806LL, "microseconds truncated");
    check(parseUtcTimestampMs("2026-10-17T21:09:22.8Z") == 1792271362800LL, "short fraction");
    check(parseUtcTimestampMs("2024-02-29T12:00:00Z").has_value(), "leap day");
    for (const char* bad : {"", "2026-10-17 21:09:22Z", "2026-10-17T21:09:22", "2026-13-01T00:00:00Z",
                            "2026-02-30T00:00:00Z", "2026-10-17T21:09:22.Z", "2026-10-17T21:09:22.8xZ",
                            "abcd-10-17T21:09:22Z"}) {
        check(!parseUtcTimestampMs(bad), std::string("rejects '") + bad + "'");
    }
}

void testAlertRegions() {
    const json payload = {
        {"alerts", json::array({{{"regionId", "16"}, {"activeAlerts", json::array({{{"type", "AIR"}}})}},
                                {{"regionId", "3"}, {"activeAlerts", json::array()}},
                                {{"regionId", "99"}, {"activeAlerts", json::array({{{"type", "ARTILLERY"}}})}},
                                {{"regionId", "7"}, {"activeAlerts", json::array({{{"type", 1}}})}},
                                {{"regionId", "8"}, {"activeAlerts", json::array({{{"type", " air "}}})}},
                                {{"regionId", "9"},
                                 {"activeAlerts", json::array({{{"type", "AIR"}, {"lastUpdate", "2026-10-17T21:00:00Z"}},
                                                               {{"type", "ARTILLERY"}, {"lastUpdate", "2026-10-17T21:05:00.5Z"}},
                                                               {{"type", "CHEMICAL"}, {"lastUpdate", "garbage"}}})}}})}};
    const json envelope = {{"data", {{"data", payload}, {"offset", 1}}}};
    const RegionAlerts alerts = activeRegionAlerts(envelope);
    check(alerts.any == std::set<std::string>{"16", "7", "8", "9", "99"}, "regions with any active alert");
    check(alerts.air == std::set<std::string>{"16", "7", "8", "9"}, "regions with an air alert");
    check(alerts.startedAtMs.size() == 1 && alerts.startedAtMs.at("9") == 1792271100500LL,
          "latest readable start per region");

    bool threw = false;
    try {
        activeRegionAlerts(json{{"other", 1}});
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw, "payload without alerts throws");
}

}  // namespace

int main() {
    try {
        testProjectionMatchesSite();
        testSimplifyRing();
        testBuildShapes();
        testOverrideRegionGeometry();
        testLabelPoint();
        testPreparedRegions();
        testMapView();
        testCities();
        testTimestamps();
        testAlertRegions();
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
