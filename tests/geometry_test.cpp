// Usage: geometry_test <tests/data directory>

#include "map_update.hpp"
#include "map_view.hpp"
#include "projection.hpp"
#include "region_geometry.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
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

    view.reset();
    check(view.zoom() == 1 && view.center() == MapPoint{kMapWidth / 2, kMapHeight / 2}, "reset");
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
        testMapView();
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
