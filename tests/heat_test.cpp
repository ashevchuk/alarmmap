// Usage: heat_test <tests/data directory>
//
// heat_strength.jsonl holds trail cells with the strength and liveness the
// site's own detailTargetHeatTiming.js computes for them at several ages.

#include "heat_cells.hpp"
#include "heat_timing.hpp"
#include "marker_layout.hpp"
#include "region_geometry.hpp"
#include "target_kind.hpp"
#include "target_motion.hpp"
#include "target_snapshot.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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

bool near(double actual, double expected, double tolerance = 1e-12) {
    return std::abs(actual - expected) <= tolerance;
}

void testTimingMatchesSite(const std::filesystem::path& dataDirectory) {
    std::ifstream input(dataDirectory / "heat_strength.jsonl");
    check(input.good(), "fixture opens");
    size_t evaluated = 0, positive = 0, samplesSeen = 0, retentionSeen = 0;
    for (std::string line; std::getline(input, line);) {
        if (line.empty()) continue;
        const json record = json::parse(line);
        const json snapshot = {{"targets", json::array()}, {"trail", json::array({record["cell"]})}};
        const HeatCell cell = decodeTargetSnapshot(snapshot).trail.at(0);
        const double fog = record["fog"].get<double>();
        samplesSeen += cell.samples.has_value();
        retentionSeen += cell.retentionAge.has_value() && cell.minimumStrength > 0;
        for (const json& result : record["results"]) {
            const double elapsed = result[0].get<double>();
            const HeatCell aged = agedHeatCell(cell, elapsed);
            const double expected = result[1].get<double>();
            check(near(heatStrength(aged, fog), expected),
                  "strength at +" + std::to_string(elapsed) + " s: " +
                      std::to_string(heatStrength(aged, fog)) + " vs " + std::to_string(expected));
            check(isHeatCellAlive(aged, fog) == result[2].get<bool>(), "alive flag");
            ++evaluated;
            positive += expected > 0;
        }
    }
    check(evaluated > 0 && positive > 0 && positive < evaluated, "fixture has live and dead cases");
    check(samplesSeen > 0 && retentionSeen > 0, "fixture exercises samples and retention");
}

json squareRing(double west, double south, double size) {
    return json::array({json::array({west, south}), json::array({west + size, south}),
                        json::array({west + size, south + size}),
                        json::array({west, south + size}), json::array({west, south})});
}

json feature(const std::string& id, const json& rings) {
    return {{"type", "Feature"},
            {"properties", {{"regionId", id}, {"name", id}, {"rank", kOblastRank}}},
            {"geometry", {{"type", "Polygon"}, {"coordinates", rings}}}};
}

// Cell whose footprint is exactly the given lon/lat square corner and size.
Cell cellAt(double longitude, double latitude, double sizeDegrees) {
    const MapPoint a = lonLatToMap(longitude, latitude + sizeDegrees);
    const MapPoint b = lonLatToMap(longitude + sizeDegrees, latitude);
    return {(a.x + b.x) / 2 / kMapWidth, (a.y + b.y) / 2 / kMapHeight, (b.x - a.x) / kMapWidth,
            (b.y - a.y) / kMapHeight, ""};
}

void testAirAlertFilter() {
    const json collection = {
        {"features",
         json::array({feature("holed", json::array({squareRing(30, 48, 1), squareRing(30.4, 48.4, 0.2)})),
                      feature("town", json::array({squareRing(33.02, 48.02, 0.01)})),
                      feature("quiet", json::array({squareRing(35, 48, 1)}))})}};
    const auto shapes = buildRegionShapes(collection, 0.001);
    AirAlertFilter filter(shapes);
    filter.setAirRegions({"holed", "town", "unknown-id"});

    check(filter.intersects(cellRect(cellAt(30.1, 48.1, 0.1))), "cell inside region");
    check(filter.intersects(cellRect(cellAt(29.95, 48.5, 0.1))), "cell straddling the border");
    check(!filter.intersects(cellRect(cellAt(30.45, 48.45, 0.1))), "cell inside the hole");
    check(filter.intersects(cellRect(cellAt(33.0, 48.0, 0.1))), "small region inside the cell");
    check(!filter.intersects(cellRect(cellAt(35.2, 48.2, 0.1))), "region without air alert");
    check(!filter.intersects(cellRect(cellAt(32.0, 50.0, 0.1))), "cell far from any region");

    filter.setAirRegions({});
    check(!filter.intersects(cellRect(cellAt(30.1, 48.1, 0.1))), "no alerts, nothing visible");
}

HeatCandidate candidate(double x, double y, bool extrapolation, double age = 0) {
    HeatCell cell;
    cell.cell = {x, y, 0.01, 0.01, ""};
    cell.age = age;
    cell.hits = 1;
    return {cell, extrapolation, {}};
}

void testConnectionFilter() {
    const std::vector<HeatCandidate> candidates = {
        candidate(0.50, 0.50, false),  // live trail cell
        candidate(0.51, 0.50, true),   // shares an edge with it
        candidate(0.52, 0.51, true),   // touches the previous one by a corner
        candidate(0.70, 0.70, true),   // isolated
    };
    const auto quads = heatQuadsAt(candidates, 0, 30);
    check(quads.size() == 3, "isolated extrapolated cell dropped, chain kept");
    if (quads.size() == 3) {
        check(quads[2].rect == cellRect(candidates[2].cell.cell), "corner-connected cell kept");
        check(near(quads[0].strength, 0.45), "default strength for one hit");
    }

    const std::vector<HeatCandidate> onlyExtrapolation = {candidate(0.51, 0.50, true)};
    check(heatQuadsAt(onlyExtrapolation, 0, 30).empty(), "no anchors, no extrapolated heat");

    HeatCandidate held = candidate(0.30, 0.30, true);
    HeatCell anchorCell;
    anchorCell.cell = {0.29, 0.30, 0.01, 0.01, ""};
    held.holdAnchors.push_back(cellRect(anchorCell.cell));
    check(heatQuadsAt({held}, 0, 30).size() == 1, "hold anchor keeps extrapolated cell");

    const std::vector<HeatCandidate> fading = {candidate(0.50, 0.50, false),
                                               candidate(0.51, 0.50, true)};
    const auto halfway = heatQuadsAt(fading, 20, 30);
    check(halfway.size() == 2 && near(halfway[0].strength, 0.45 * 0.5), "linear fade after delay");
    check(heatQuadsAt(fading, 31, 30).empty(), "everything gone after fog time");
}

Target target(const std::string& id, double x, double y, int durationMs = 20000) {
    Target target;
    target.trackId = id;
    target.cell = {x, y, 0.01, 0.01, ""};
    target.movementDurationMs = durationMs;
    return target;
}

void testTargetMotion() {
    TargetMotion motion;
    motion.update({target("a", 0.1, 0.1)}, 0);
    auto markers = motion.markersAt(5);
    check(markers.size() == 1 && markers[0].position == normalizedToMap(0.1, 0.1),
          "new track appears in place");

    motion.update({target("a", 0.2, 0.1)}, 10);
    markers = motion.markersAt(20);
    check(markers.size() == 1 && near(markers[0].position.x, normalizedToMap(0.15, 0.1).x, 1e-9),
          "halfway through a 20 s glide");

    motion.update({target("a", 0.2, 0.1)}, 20);
    markers = motion.markersAt(25);
    check(near(markers[0].position.x, normalizedToMap(0.175, 0.1).x, 1e-9),
          "same cell does not restart the glide");

    motion.update({target("a", 0.3, 0.1)}, 20);
    markers = motion.markersAt(20);
    check(near(markers[0].position.x, normalizedToMap(0.15, 0.1).x, 1e-9),
          "new cell glides from the drawn position");

    motion.update({target("b", 0.5, 0.5)}, 30);
    check(motion.markersAt(30).size() == 1, "a new track starts fully transparent");
    markers = motion.markersAt(31.5);
    check(markers.size() == 2 && markers[0].target.trackId == "b" && near(markers[0].opacity, 0.5) &&
              markers[1].target.trackId == "a" && near(markers[1].opacity, 1),
          "new track fades in, missing track is held");
    markers = motion.markersAt(50);
    check(markers.size() == 2 && near(markers[1].opacity, 0.5), "missing track fades after the hold");
    markers = motion.markersAt(60);
    check(markers.size() == 1 && markers[0].target.trackId == "b", "faded track is not drawn");

    motion.update({target("b", 0.5, 0.5), target("a", 0.3, 0.1)}, 45);
    markers = motion.markersAt(45);
    check(markers.size() == 2 && markers[1].target.trackId == "a" && near(markers[1].opacity, 1),
          "track coming back before it faded is fully visible again");

    motion.update({target("b", 0.5, 0.5)}, 46);
    motion.update({target("b", 0.5, 0.5)}, 80);
    motion.update({target("b", 0.5, 0.5), target("a", 0.9, 0.9)}, 81);
    markers = motion.markersAt(81 + TargetMotion::kFadeInSeconds);
    check(markers.size() == 2 && markers[1].position == normalizedToMap(0.9, 0.9),
          "track faded out completely returns as new, without a glide");
}

TargetMotion::Marker settledMarker(const std::string& id, double x, double y, int type,
                                   double jitterX = 0) {
    TargetMotion::Marker marker;
    marker.target = target(id, x, y);
    marker.target.type = type;
    marker.target.markerOffsetX = jitterX;
    marker.position = normalizedToMap(x + jitterX, y);
    return marker;
}

void testMarkerLayout() {
    // Cell of 0.01 normalized units is 2.6 map units wide; 100 px per unit
    // makes it 260 px, wide enough for full-size columns.
    const double pixelsPerUnit = 100;
    std::vector<TargetMotion::Marker> markers = {
        settledMarker("a", 0.5, 0.5, 8, 0.002),   // same cell and type, different jitter
        settledMarker("b", 0.5, 0.5, 8, -0.001),
        settledMarker("c", 0.5, 0.5, 8),
        settledMarker("m", 0.5, 0.5, 3),          // another type in the same cell
        settledMarker("far", 0.7, 0.7, 8),
    };
    markers[2].fadingOut = true;

    auto placements = layoutMarkers(markers, pixelsPerUnit);
    check(placements.size() == 3, "three markers drawn: group, other type, far target");
    if (placements.size() == 3) {
        check(placements[0].index == 0 && placements[0].count == 2,
              "group led by a steady member, fading member not counted");
        check(placements[1].index == 3 && placements[1].count == 1, "other type stands alone");
        check(near(placements[0].offsetPx, 24) && near(placements[1].offsetPx, -24),
              "types in one cell are spread apart");
        check(near(placements[0].scale, 1), "wide cell keeps full size");
        check(placements[2].index == 4 && near(placements[2].offsetPx, 0), "lone target centred");
    }

    markers[1].position.x += 0.5;  // still gliding into the cell
    markers[1].gliding = true;
    placements = layoutMarkers(markers, pixelsPerUnit);
    check(placements.size() == 4, "a marker still on its way is not merged");

    const auto narrow = layoutMarkers(markers, 5);
    check(!narrow.empty() && near(narrow.front().scale, 0.7), "narrow cell shrinks the icons");
    check(std::all_of(narrow.begin(), narrow.end(),
                      [](const MarkerPlacement& placement) { return std::abs(placement.offsetPx) <= 13 * 0.35 + 1e-9; }),
          "spread stays within 70% of a narrow cell");

    markers = {settledMarker("x", 0.3, 0.3, 8), settledMarker("y", 0.3, 0.3, 8)};
    markers[0].fadingOut = markers[1].fadingOut = true;
    placements = layoutMarkers(markers, pixelsPerUnit);
    check(placements.size() == 1 && placements[0].count == 2, "all fading members are counted");
}

void testTargetKinds() {
    // Every icon name of the site's target type table and detected types.
    const std::pair<const char*, TargetKind> expected[] = {
        {"ic_shahed", TargetKind::Drone},
        {"ic_shahed_mesh", TargetKind::Drone},
        {"ic_drone", TargetKind::Drone},
        {"ic_fixed_wing_loitering", TargetKind::Drone},
        {"ic_fixed_wing_very_small", TargetKind::Drone},
        {"ic_fixed_wing_small", TargetKind::Drone},
        {"ic_shahed_jet", TargetKind::JetDrone},
        {"ic_shahed_jet_mesh", TargetKind::JetDrone},
        {"ic_missile", TargetKind::CruiseMissile},
        {"ic_missile_sr_tasm", TargetKind::CruiseMissile},
        {"ic_missile_arm", TargetKind::CruiseMissile},
        {"ic_missile_sscm", TargetKind::CruiseMissile},
        {"ic_fixed_wing_lacm", TargetKind::CruiseMissile},
        {"ic_fixed_wing_lr_tasm", TargetKind::CruiseMissile},
        {"ic_ballistic", TargetKind::BallisticMissile},
        {"ic_ballistic_lcr", TargetKind::BallisticMissile},
        {"ic_ballistic_mrr", TargetKind::BallisticMissile},
        {"ic_ballistic_srr", TargetKind::BallisticMissile},
        {"ic_airbomb", TargetKind::GuidedBomb},
        {"ic_rotary_wing", TargetKind::Helicopter},
        {"ic_ground_simple", TargetKind::Ground},
        {"ic_ground_fixed_warning_t_small", TargetKind::Ground},
        {"ic_ground_emitter", TargetKind::Ground},
        {"ic_plane", TargetKind::Plane},
        {"ic_plane_predicted", TargetKind::Plane},
        {"ic_fixed_wing_medium", TargetKind::Plane},
        {"ic_fixed_wing_large", TargetKind::Plane},
        {"ic_fixed_wing_airliner", TargetKind::Plane},
        {"ic_fixed_wing_turboprop", TargetKind::Plane},
        {"ic_asterix_plane_stroke", TargetKind::Plane},
        {"ic_asterix_plane_masked", TargetKind::Plane},
        {"ic_air_threat_combined_white", TargetKind::Plane},
        {"", TargetKind::Plane},
    };
    for (const auto& [iconName, kind] : expected) {
        check(targetKind(iconName) == kind, std::string("kind of '") + iconName + "'");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: heat_test <data directory>\n";
        return 2;
    }
    try {
        testTimingMatchesSite(argv[1]);
        testAirAlertFilter();
        testConnectionFilter();
        testTargetMotion();
        testTargetKinds();
        testMarkerLayout();
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
