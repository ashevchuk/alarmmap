// Map of Ukraine with region borders, active alerts, the heat trail of targets
// and the targets themselves.
//
// Usage: alarmmap [--targets FILE] [--alerts FILE] [--screenshot PNG] [--benchmark FRAMES]
//        alarmmap --version
//   --targets     target snapshot (compact v2 packet or REST JSON)
//   --alerts      updateMap payload
//   --screenshot  render one frame into PNG and exit
//   --benchmark   render FRAMES frames without a frame rate cap, print the
//                 average frame time and exit
// Without --targets and --alerts the data comes live from map.ukrainealarm.com.

#include "heat_cells.hpp"
#include "heat_renderer.hpp"
#include "app_icon.hpp"
#include "cities.hpp"
#include "http.hpp"
#include "live_feed.hpp"
#include "map_layer.hpp"
#include "map_update.hpp"
#include "map_view.hpp"
#include "marker_layout.hpp"
#include "projection.hpp"
#include "region_geometry.hpp"
#include "render_util.hpp"
#include "snout_icon.hpp"
#include "target_effects.hpp"
#include "target_kind.hpp"
#include "target_motion.hpp"
#include "target_snapshot.hpp"
#include "tile_store.hpp"
#include "ui_font.hpp"

#include <raylib.h>
#include <rlgl.h>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android_native_app_glue.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace alarmmap;

namespace {

constexpr const char* kGeometryUrl =
    "https://map.ukrainealarm.com/data/real-map/regions-standard-v1.geojson";
constexpr const char* kGeometryFileName = "regions-standard-v1.geojson";
// Land outline of Crimea; the site's own outline follows the territorial sea.
constexpr const char* kCrimeaOverrideFile = "crimea.geojson";
constexpr const char* kCitiesFile = "cities.geojson";
constexpr const char* kTileJsonUrl = "https://tiles.openfreemap.org/planet";
constexpr const char* kTargetsChannel = "mapTargets_heat_legacy_v2";
constexpr const char* kTargetsSnapshotUrl =
    "https://map.ukrainealarm.com/api/v2/data/targetsForMapsState/public?separateTracks=false";
constexpr const char* kAlertsChannel = "updateMap";
constexpr const char* kAlertsSnapshotUrl = "https://map.ukrainealarm.com/api/v2/data/mapUpdate";
constexpr auto kScreenshotDataTimeout = std::chrono::seconds(30);
constexpr auto kScreenshotPollInterval = std::chrono::milliseconds(100);
// About 240 m on the ground: under one pixel even with the country on a 4K
// screen. Zoomed in, the detailed shapes take over; the source data itself
// has a resolution of roughly 0.01 map units, so finer would change nothing.
constexpr double kSimplifyTolerance = 0.05;
constexpr double kDetailedTolerance = 0.002;

// Animations here are slow (20 s glides, multi-second fades); 30 frames per
// second look the same as 60 at half the cost. Panning and zooming get 60
// while the user interacts and shortly after.
constexpr double kFramesPerSecond = 30;
constexpr double kInteractiveFramesPerSecond = 60;
constexpr double kInteractionHoldSeconds = 1;
constexpr double kWheelZoomStep = 1.25;
constexpr double kKeyZoomStep = 1.5;
constexpr int kInitialWidth = 1600;
constexpr int kInitialHeight = 1000;
constexpr int kMinimumWidth = 320;
constexpr int kMinimumHeight = 220;
constexpr float kStatusMarginPx = 24;
constexpr float kStatusFontPx = 17;
constexpr float kStatusDotRadiusPx = 5;
constexpr float kTextSpacingPx = 0.3f;
constexpr float kPanelPaddingPx = 12;
constexpr float kPanelRowPx = 28;
constexpr float kPanelIconPx = 22;
constexpr float kPanelFontPx = 15;
constexpr float kPanelIconGapPx = 10;
constexpr float kPanelNumberGapPx = 20;
constexpr float kTargetIconPx = 34;
constexpr Vector2 kShadowOffsetPx = {3, 4};
// A region whose alert has just started pulses for a while, fading out.
constexpr double kPulseWindowSeconds = 180;
constexpr double kPulsePeriodSeconds = 2;
constexpr float kPulseMaxOpacity = 0.45f;
constexpr float kBadgeFontPx = 13;
constexpr Vector2 kBadgePaddingPx = {6, 1};
constexpr Color kBadgeBackground = {20, 12, 16, 235};
constexpr Color kBadgeBorder = {255, 214, 100, 255};
constexpr Color kBadgeText = {255, 240, 200, 255};
constexpr float kShadowOpacity = 0.5f;

constexpr Color kStatusColor = {176, 184, 198, 255};
constexpr Color kLiveDot = {90, 210, 120, 255};
constexpr Color kConnectingDot = {240, 180, 60, 255};
constexpr Color kOfflineDot = {235, 80, 70, 255};
constexpr Color kPanelBackground = {12, 15, 20, 200};
constexpr Color kPanelBorder = {70, 80, 96, 160};
constexpr Color kPanelText = {205, 212, 224, 255};
constexpr Color kScreenClear = BLACK;
constexpr Color kCreditsColor = {120, 128, 142, 200};
constexpr float kCreditsFontPx = 12;
// GeoNames data is CC BY 4.0, which requires naming the source.
constexpr const char* kCreditsText =
    "Дані: map.ukrainealarm.com · OpenFreeMap © OpenMapTiles © OpenStreetMap · GeoNames (CC BY 4.0) · Natural Earth";

// Summary panel rows, most dangerous first.
constexpr std::pair<TargetKind, const char*> kKindLabels[] = {
    {TargetKind::BallisticMissile, "Балістика"},
    {TargetKind::CruiseMissile, "Крилаті ракети"},
    {TargetKind::JetDrone, "Реактивні дрони"},
    {TargetKind::Drone, "Ударні дрони"},
    {TargetKind::GuidedBomb, "КАБ"},
    {TargetKind::Helicopter, "Гелікоптери"},
    {TargetKind::Plane, "Літаки"},
    {TargetKind::Ground, "Наземні цілі"},
    {TargetKind::Unknown, "Невідомі"},
};

// Name of one target of a kind, for targets the source gives no name.
constexpr std::pair<TargetKind, const char*> kKindNames[] = {
    {TargetKind::BallisticMissile, "Балістична ракета"},
    {TargetKind::CruiseMissile, "Крилата ракета"},
    {TargetKind::JetDrone, "Реактивний дрон"},
    {TargetKind::Drone, "Ударний дрон"},
    {TargetKind::GuidedBomb, "КАБ"},
    {TargetKind::Helicopter, "Гелікоптер"},
    {TargetKind::Plane, "Літак"},
    {TargetKind::Ground, "Наземна ціль"},
    {TargetKind::Unknown, "Невідома ціль"},
};

std::string kindName(TargetKind kind) {
    for (const auto& [candidate, name] : kKindNames) {
        if (candidate == kind) return name;
    }
    return {};
}

constexpr float kHoverRadiusShare = 0.6f;
constexpr float kTooltipGapPx = 10;
constexpr float kTooltipPaddingPx = 8;

struct StatusLine {
    std::string text;
    Color dot;
};

struct Options {
    std::optional<fs::path> targets;
    std::optional<fs::path> alerts;
    std::optional<fs::path> screenshot;
    std::optional<int> benchmarkFrames;
    bool version = false;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--version") {
            options.version = true;
            continue;
        }
        if (i + 1 >= argc) throw std::runtime_error("missing value for " + name);
        const fs::path value = argv[++i];
        if (name == "--targets") options.targets = value;
        else if (name == "--alerts") options.alerts = value;
        else if (name == "--screenshot") options.screenshot = value;
        else if (name == "--benchmark") options.benchmarkFrames = std::stoi(value.string());
        else throw std::runtime_error("unknown option " + name);
    }
    return options;
}

bool isLive(const Options& options) { return !options.targets && !options.alerts; }

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#ifdef __ANDROID__
// Files the APK carries. Android exposes assets only through AAssetManager,
// so they are copied into internal storage, where the regular file code
// reads them as data and cache.
constexpr const char* kBundledAssets[] = {
    "cities.geojson", "crimea.geojson", "fonts/NotoSans-Medium.ttf",
    "regions-standard-v1.geojson", "targets.json", "alerts.json",
};

fs::path androidStorage() { return GetAndroidApp()->activity->internalDataPath; }

void extractAssets() {
    AAssetManager* manager = GetAndroidApp()->activity->assetManager;
    for (const char* name : kBundledAssets) {
        AAsset* asset = AAssetManager_open(manager, name, AASSET_MODE_BUFFER);
        if (!asset) throw std::runtime_error(std::string("missing asset ") + name);
        const auto* data = static_cast<const char*>(AAsset_getBuffer(asset));
        const auto size = static_cast<std::streamsize>(AAsset_getLength64(asset));
        const fs::path path = androidStorage() / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        if (data) output.write(data, size);
        AAsset_close(asset);
        if (!data || !output) throw std::runtime_error("cannot extract asset " + std::string(name));
    }
}

fs::path cacheDirectory() { return androidStorage(); }

fs::path dataDirectory() { return androidStorage(); }
#else
fs::path cacheDirectory() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "alarmmap";
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("neither XDG_CACHE_HOME nor HOME is set");
    return fs::path(home) / ".cache" / "alarmmap";
}

// Installed data lives in share/alarmmap beside the bin directory, which
// keeps an AppImage or any other relocated install working; a build tree
// uses the source data directory.
fs::path dataDirectory() {
    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    if (!error) {
        const fs::path installed = executable.parent_path().parent_path() / "share" / "alarmmap";
        if (fs::exists(installed / kCitiesFile)) return installed;
    }
    return ALARMMAP_DATA_DIR;
}
#endif

fs::path ensureGeometryFile() {
    const fs::path path = cacheDirectory() / kGeometryFileName;
    if (fs::exists(path)) return path;

    std::cout << "downloading " << kGeometryUrl << '\n';
    const std::string body = httpGet(kGeometryUrl);
    fs::create_directories(path.parent_path());
    const fs::path partial = path.string() + ".partial";
    {
        std::ofstream output(partial, std::ios::binary);
        output << body;
        if (!output) throw std::runtime_error("cannot write " + partial.string());
    }
    fs::rename(partial, path);
    return path;
}

json readJsonFile(const fs::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return json::parse(input);
}

Vector2 toVector(const MapPoint& point) {
    return {static_cast<float>(point.x), static_cast<float>(point.y)};
}

// One icon texture per target kind.
class TargetIcons {
public:
    TargetIcons() {
        for (const TargetKind kind : kAllTargetKinds) icons_[kind] = std::make_unique<SnoutIcon>(kind);
    }
    TargetIcons(const TargetIcons&) = delete;
    TargetIcons& operator=(const TargetIcons&) = delete;

    const SnoutIcon& forKind(TargetKind kind) const { return *icons_.at(kind); }
    const SnoutIcon& forTarget(const Target& target) const {
        return forKind(targetKind(target.iconName, target.detectedType));
    }

private:
    std::map<TargetKind, std::unique_ptr<SnoutIcon>> icons_;
};

struct RegionShapes {
    std::vector<RegionShape> coarse;
    std::vector<RegionShape> detailed;
    std::vector<City> cities;
};

RegionShapes loadShapes() {
    const fs::path geometryPath = ensureGeometryFile();
    json regions = readJsonFile(geometryPath);
    overrideRegionGeometry(regions, readJsonFile(dataDirectory() / kCrimeaOverrideFile));
    RegionShapes shapes{buildRegionShapes(regions, kSimplifyTolerance),
                        buildRegionShapes(regions, kDetailedTolerance),
                        loadCities(readJsonFile(dataDirectory() / kCitiesFile))};
    const auto triangles = [](const std::vector<RegionShape>& level) {
        size_t count = 0;
        for (const auto& shape : level) count += shape.indices.size() / 3;
        return count;
    };
    std::cout << "regions=" << shapes.coarse.size() << " triangles=" << triangles(shapes.coarse) << '/'
              << triangles(shapes.detailed) << '\n';
    return shapes;
}

// Everything the renderer draws. Holds pointers into its own shapes, so it
// is neither copied nor moved.
class Scene {
public:
    Scene(RegionShapes shapes, const Font& font, const fs::path& tileCache)
        : shapes_(std::move(shapes)),
          font_(font),
          tiles_({kTileJsonUrl, tileCache, {}}),
          mapLayer_(shapes_.coarse, kSimplifyTolerance, shapes_.detailed, shapes_.cities, tiles_, font_),
          airFilter_(shapes_.coarse) {}
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Applies the latest data; unchanged parts keep their animation state.
    void apply(const std::optional<TargetSnapshot>& targets, const std::optional<RegionAlerts>& alerts,
               double now) {
        const bool alertsChanged = alerts != alerts_;
        const bool targetsChanged =
            targets.has_value() != targets_.has_value() ||
            (targets && targets->serverTimeMs != targets_->serverTimeMs);
        if (alertsChanged) {
            alerts_ = alerts;
            if (alerts_) airFilter_.setAirRegions(alerts_->air);
            mapLayer_.setAlertRegions(alerts_ ? &alerts_->any : nullptr);
        }
        if (targetsChanged) {
            targets_ = targets;
            targetsReceivedAt_ = now;
            motion_.update(targets_ ? targets_->targets : std::vector<Target>{}, now);
        }
        if (!alertsChanged && !targetsChanged) return;
        ++dataVersion_;

        // The site shows targets and heat only inside air alert regions;
        // until alerts are known nothing is filtered.
        const AirAlertFilter* filter = alerts_ ? &airFilter_ : nullptr;
        heatCandidates_ = targets_ ? selectHeatCandidates(*targets_, filter)
                                   : std::vector<HeatCandidate>{};
        // Tracks fading out after leaving the snapshot keep their visibility.
        std::unordered_set<std::string> visible;
        for (const std::string& trackId : motion_.trackIds()) {
            if (visibleTracks_.contains(trackId)) visible.insert(trackId);
        }
        if (targets_) {
            for (const Target& target : targets_->targets) {
                if (!filter || filter->intersects(cellRect(target.cell))) {
                    visible.insert(target.trackId);
                } else {
                    visible.erase(target.trackId);
                }
            }
        }
        visibleTracks_ = std::move(visible);
    }

    // Changes whenever targets or alerts change.
    std::uint64_t dataVersion() const { return dataVersion_; }

    std::vector<HeatQuad> heatQuadsAt(double now) const {
        if (!targets_) return {};
        return alarmmap::heatQuadsAt(heatCandidates_, now - targetsReceivedAt_, targets_->fogSeconds);
    }

    // Offscreen work that switches render targets; runs before the frame.
    void prepare(const MapView& view, double now) { mapLayer_.prepare(view, now); }

    // `pointer` is the mouse position while it hovers over the map.
    void draw(const MapView& view, const Camera2D& camera, const HeatRenderer& heat,
              const TargetIcons& icons, double now, std::optional<Vector2> pointer) const {
        const auto width = static_cast<float>(view.width());
        const auto height = static_cast<float>(view.height());
        ClearBackground(kScreenClear);
        mapLayer_.draw(view);
        mapLayer_.drawHighlights(freshAlertPulses(), view);
        heat.composite();
        mapLayer_.drawVignette(width, height);
        std::vector<TargetMotion::Marker> markers = motion_.markersAt(now);
        std::erase_if(markers, [&](const TargetMotion::Marker& marker) {
            return !visibleTracks_.contains(marker.target.trackId);
        });
        // The layout works in unscaled pixels; screenPosition() scales its offsets.
        const std::vector<MarkerPlacement> placements = layoutMarkers(markers, camera.zoom / uiScale());
        std::vector<TargetMotion::Explosion> explosions = motion_.explosionsAt(now);
        std::erase_if(explosions, [&](const TargetMotion::Explosion& explosion) {
            return !visibleTracks_.contains(explosion.target.trackId);
        });
        for (const auto& explosion : explosions) {
            const Vector2 center = GetWorldToScreen2D(toVector(explosion.position), camera);
            drawScaledAround(center, [&] {
                drawScorch(center, explosion.age, TargetMotion::kAftermathSeconds,
                           effectSeed(explosion.target.trackId));
            });
        }
        const MarkerPlacement* hovered =
            pointer ? hoveredPlacement(placements, markers, camera, *pointer) : nullptr;
        if (hovered) drawPaths(*hovered, markers, camera, now);
        drawTargets(markers, placements, camera, icons);
        for (const auto& placement : placements) {
            const TargetMotion::Marker& marker = markers[placement.index];
            const Vector2 center = screenPosition(placement, markers, camera);
            drawScaledAround(center, [&] { drawTeleport(center, marker.age, effectSeed(marker.target.trackId)); });
        }
        for (const auto& explosion : explosions) {
            const Vector2 center = GetWorldToScreen2D(toVector(explosion.position), camera);
            drawScaledAround(center, [&] {
                drawExplosion(center, explosion.age, effectSeed(explosion.target.trackId));
            });
        }
        // Overlays are laid out in unscaled pixels over a window shrunk by the
        // UI scale, then drawn scaled up.
        const float ui = uiScale();
        drawScaledAround({0, 0}, [&] {
            if (hovered) drawTooltip(*hovered, markers, camera, width / ui, height / ui);
            drawSummary(markers, icons, static_cast<int>(height / ui));
            if (status) drawStatus(*status);
            drawCredits(width / ui, height / ui);
        });
    }

    std::optional<StatusLine> status;

private:
    // Pulse opacity of regions whose alert started recently, by the wall
    // clock, because alert start times come from the server in UTC.
    std::map<std::string, float> freshAlertPulses() const {
        std::map<std::string, float> pulses;
        if (!alerts_) return pulses;
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        for (const auto& [regionId, startedAtMs] : alerts_->startedAtMs) {
            // A start slightly in the future means clock skew: treat it as now.
            const double age = std::max<double>(0, (nowMs - startedAtMs) / 1000.0);
            if (age >= kPulseWindowSeconds) continue;
            const double envelope = 1 - age / kPulseWindowSeconds;
            const double wave = 0.5 - 0.5 * std::cos(2 * PI * age / kPulsePeriodSeconds);
            pulses[regionId] = static_cast<float>(kPulseMaxOpacity * envelope * wave);
        }
        return pulses;
    }

    void drawCredits(float width, float height) const {
        const Vector2 extent = MeasureTextEx(font_, kCreditsText, kCreditsFontPx, kTextSpacingPx);
        DrawTextEx(font_, kCreditsText,
                   {width - kStatusMarginPx - extent.x, height - kStatusMarginPx / 2 - extent.y},
                   kCreditsFontPx, kTextSpacingPx, kCreditsColor);
    }

    void drawStatus(const StatusLine& line) const {
        const Vector2 extent = MeasureTextEx(font_, line.text.c_str(), kStatusFontPx, kTextSpacingPx);
        DrawCircleV({kStatusMarginPx + kStatusDotRadiusPx, kStatusMarginPx + extent.y / 2},
                    kStatusDotRadiusPx, line.dot);
        DrawTextEx(font_, line.text.c_str(),
                   {kStatusMarginPx + 3 * kStatusDotRadiusPx, kStatusMarginPx}, kStatusFontPx,
                   kTextSpacingPx, kStatusColor);
    }

    void drawSummary(const std::vector<TargetMotion::Marker>& markers, const TargetIcons& icons,
                     int height) const {
        std::map<TargetKind, int> counts;
        for (const auto& marker : markers) {
            ++counts[targetKind(marker.target.iconName, marker.target.detectedType)];
        }
        if (counts.empty()) return;

        float labelWidth = 0;
        float numberWidth = 0;
        for (const auto& [kind, label] : kKindLabels) {
            const auto count = counts.find(kind);
            if (count == counts.end()) continue;
            labelWidth = std::max(labelWidth, MeasureTextEx(font_, label, kPanelFontPx, kTextSpacingPx).x);
            const std::string number = std::to_string(count->second);
            numberWidth = std::max(numberWidth, MeasureTextEx(font_, number.c_str(), kPanelFontPx, kTextSpacingPx).x);
        }
        const float panelWidth = 2 * kPanelPaddingPx + kPanelIconPx + kPanelIconGapPx + labelWidth +
                                 kPanelNumberGapPx + numberWidth;
        const float panelHeight = 2 * kPanelPaddingPx + kPanelRowPx * counts.size();
        const Rectangle panel = {kStatusMarginPx, height - kStatusMarginPx - panelHeight, panelWidth,
                                 panelHeight};
        DrawRectangleRounded(panel, 0.12f, 8, kPanelBackground);
        DrawRectangleRoundedLinesEx(panel, 0.12f, 8, 1, kPanelBorder);
        float rowTop = panel.y + kPanelPaddingPx;
        for (const auto& [kind, label] : kKindLabels) {
            const auto count = counts.find(kind);
            if (count == counts.end()) continue;
            const float rowCenter = rowTop + kPanelRowPx / 2;
            icons.forKind(kind).draw({panel.x + kPanelPaddingPx + kPanelIconPx / 2, rowCenter},
                                     kPanelIconPx, 0);
            const float textTop = rowCenter - kPanelFontPx * 0.62f;
            DrawTextEx(font_, label, {panel.x + kPanelPaddingPx + kPanelIconPx + kPanelIconGapPx, textTop},
                       kPanelFontPx, kTextSpacingPx, kPanelText);
            const std::string number = std::to_string(count->second);
            const float width = MeasureTextEx(font_, number.c_str(), kPanelFontPx, kTextSpacingPx).x;
            DrawTextEx(font_, number.c_str(), {panel.x + panel.width - kPanelPaddingPx - width, textTop},
                       kPanelFontPx, kTextSpacingPx, kPanelText);
            rowTop += kPanelRowPx;
        }
    }

    static Vector2 screenPosition(const MarkerPlacement& placement,
                                  const std::vector<TargetMotion::Marker>& markers, const Camera2D& camera) {
        const Vector2 position = GetWorldToScreen2D(toVector(markers[placement.index].position), camera);
        return Vector2{position.x + static_cast<float>(placement.offsetPx) * uiScale(), position.y};
    }

    // The marker under the pointer, the nearest one where markers overlap.
    static const MarkerPlacement* hoveredPlacement(const std::vector<MarkerPlacement>& placements,
                                                   const std::vector<TargetMotion::Marker>& markers,
                                                   const Camera2D& camera, Vector2 pointer) {
        const MarkerPlacement* nearest = nullptr;
        float nearestDistance = 0;
        for (const auto& placement : placements) {
            const Vector2 position = screenPosition(placement, markers, camera);
            const float distance = std::hypot(position.x - pointer.x, position.y - pointer.y);
            const float radius =
                kTargetIconPx * uiScale() * static_cast<float>(placement.scale) * kHoverRadiusShare;
            if (distance <= radius && (!nearest || distance < nearestDistance)) {
                nearest = &placement;
                nearestDistance = distance;
            }
        }
        return nearest;
    }

    // Paths of all targets a marker stands for, ending at the marker.
    void drawPaths(const MarkerPlacement& placement, const std::vector<TargetMotion::Marker>& markers,
                   const Camera2D& camera, double now) const {
        const Vector2 end = screenPosition(placement, markers, camera);
        for (const size_t index : placement.members) {
            const TargetMotion::Marker& marker = markers[index];
            const std::vector<MapPoint> path = motion_.pathAt(marker.target.trackId, now);
            if (path.empty()) continue;
            std::vector<Vector2> points;
            points.reserve(path.size());
            for (const MapPoint& point : path) points.push_back(GetWorldToScreen2D(toVector(point), camera));
            points.back() = end;
            drawDashedPath(points, static_cast<float>(marker.opacity));
        }
    }

    // Name of the hovered target beside its marker, kept inside the window.
    void drawTooltip(const MarkerPlacement& placement, const std::vector<TargetMotion::Marker>& markers,
                     const Camera2D& camera, float width, float height) const {
        const Target& target = markers[placement.index].target;
        const std::string kind = kindName(targetKind(target.iconName, target.detectedType));
        std::string title = target.formular.empty() ? kind : target.formular;
        if (placement.count > 1) title += " ×" + std::to_string(placement.count);
        const std::string subtitle = target.formular.empty() || target.formular == kind ? "" : kind;

        const Vector2 titleExtent = MeasureTextEx(font_, title.c_str(), kPanelFontPx, kTextSpacingPx);
        const Vector2 subtitleExtent =
            subtitle.empty() ? Vector2{0, 0}
                             : MeasureTextEx(font_, subtitle.c_str(), kBadgeFontPx, kTextSpacingPx);
        const float panelWidth = std::max(titleExtent.x, subtitleExtent.x) + 2 * kTooltipPaddingPx;
        const float panelHeight = titleExtent.y + subtitleExtent.y + 2 * kTooltipPaddingPx;
        const Vector2 iconOnScreen = screenPosition(placement, markers, camera);
        const Vector2 icon = {iconOnScreen.x / uiScale(), iconOnScreen.y / uiScale()};
        const float iconHalf = kTargetIconPx * static_cast<float>(placement.scale) / 2;
        float left = icon.x + iconHalf + kTooltipGapPx;
        if (left + panelWidth > width) left = icon.x - iconHalf - kTooltipGapPx - panelWidth;
        const float top = std::clamp(icon.y - panelHeight / 2, 0.0f, std::max(0.0f, height - panelHeight));
        const Rectangle panel = {left, top, panelWidth, panelHeight};
        DrawRectangleRounded(panel, 0.2f, 8, kPanelBackground);
        DrawRectangleRoundedLinesEx(panel, 0.2f, 8, 1, kPanelBorder);
        DrawTextEx(font_, title.c_str(), {left + kTooltipPaddingPx, top + kTooltipPaddingPx}, kPanelFontPx,
                   kTextSpacingPx, kPanelText);
        if (!subtitle.empty()) {
            DrawTextEx(font_, subtitle.c_str(),
                       {left + kTooltipPaddingPx, top + kTooltipPaddingPx + titleExtent.y}, kBadgeFontPx,
                       kTextSpacingPx, kStatusColor);
        }
    }

    void drawTargets(const std::vector<TargetMotion::Marker>& markers,
                     const std::vector<MarkerPlacement>& placements, const Camera2D& camera,
                     const TargetIcons& icons) const {
        const auto screenPosition = [&](const MarkerPlacement& placement) {
            return Scene::screenPosition(placement, markers, camera);
        };
        const auto iconScale = [&](const MarkerPlacement& placement) {
            return static_cast<float>(placement.scale) * teleportIconScale(markers[placement.index].age);
        };
        // Layer by layer, so a shadow never covers a neighbouring icon.
        for (const bool shadow : {true, false}) {
            for (const auto& placement : placements) {
                const TargetMotion::Marker& marker = markers[placement.index];
                const Vector2 position = screenPosition(placement);
                const auto opacity = static_cast<float>(marker.opacity);
                const float size = kTargetIconPx * uiScale() * iconScale(placement);
                // Without a heading the icon points north.
                const auto heading = static_cast<float>(marker.target.headingDegrees.value_or(0));
                const SnoutIcon& icon = icons.forTarget(marker.target);
                if (shadow) {
                    const Vector2 shadowAt = {position.x + kShadowOffsetPx.x * uiScale(),
                                              position.y + kShadowOffsetPx.y * uiScale()};
                    icon.draw(shadowAt, size, heading, Fade(BLACK, kShadowOpacity * opacity));
                } else {
                    icon.draw(position, size, heading, Fade(WHITE, opacity));
                }
            }
        }
        for (const auto& placement : placements) {
            if (placement.count > 1) {
                const Vector2 center = screenPosition(placement);
                drawScaledAround(center, [&] {
                    drawCountBadge(center, static_cast<float>(placement.scale), placement.count,
                                   static_cast<float>(markers[placement.index].opacity));
                });
            }
        }
    }

    // "×N" pill at the upper right of a grouped marker.
    void drawCountBadge(Vector2 iconCenter, float scale, int count, float opacity) const {
        const std::string text = "×" + std::to_string(count);
        const Vector2 extent = MeasureTextEx(font_, text.c_str(), kBadgeFontPx, kTextSpacingPx);
        const float height = extent.y + 2 * kBadgePaddingPx.y;
        const float width = std::max(height, extent.x + 2 * kBadgePaddingPx.x);
        const float iconHalf = kTargetIconPx * scale / 2;
        const Rectangle pill = {iconCenter.x + iconHalf * 0.55f, iconCenter.y - iconHalf - height * 0.35f,
                                width, height};
        DrawRectangleRounded(pill, 1, 8, Fade(kBadgeBackground, opacity));
        DrawRectangleRoundedLinesEx(pill, 1, 8, 1, Fade(kBadgeBorder, opacity));
        DrawTextEx(font_, text.c_str(),
                   {pill.x + (width - extent.x) / 2, pill.y + kBadgePaddingPx.y}, kBadgeFontPx,
                   kTextSpacingPx, Fade(kBadgeText, opacity));
    }

    RegionShapes shapes_;
    const Font& font_;
    TileStore tiles_;
    MapLayer mapLayer_;
    AirAlertFilter airFilter_;
    std::optional<RegionAlerts> alerts_;
    std::optional<TargetSnapshot> targets_;
    double targetsReceivedAt_ = 0;
    std::vector<HeatCandidate> heatCandidates_;
    std::unordered_set<std::string> visibleTracks_;
    TargetMotion motion_;
    std::uint64_t dataVersion_ = 0;
};

// Latest live data, written on the feed thread and copied by the renderer.
enum class Connection { Connecting, Live, Offline };

struct LiveData {
    std::optional<TargetSnapshot> targets;
    std::optional<RegionAlerts> alerts;
    Connection connection = Connection::Connecting;
    // When the next connection attempt starts, while offline.
    double retryAt = 0;
    std::uint64_t version = 0;
};

class LiveState {
public:
    // Feed thread.
    void onPublication(const std::string& channel, const json& payload) {
        if (channel == kTargetsChannel) {
            std::optional<TargetSnapshot> snapshot = reader_.read(payload);
            if (!snapshot) return;
            std::lock_guard lock(mutex_);
            // The REST snapshot and a queued publication can arrive out of order.
            if (data_.targets && snapshot->serverTimeMs < data_.targets->serverTimeMs) return;
            data_.targets = std::move(snapshot);
            ++data_.version;
        } else if (channel == kAlertsChannel) {
            RegionAlerts alerts = activeRegionAlerts(payload);
            std::lock_guard lock(mutex_);
            data_.alerts = std::move(alerts);
            ++data_.version;
        }
    }

    // Feed thread.
    void onEvent(const std::string& event, const json& details) {
        std::cout << "feed: " << event << ' ' << details.dump() << '\n';
        Connection connection;
        double retryAt = 0;
        if (event == "connected") {
            connection = Connection::Live;
        } else if (event == "session_error") {
            connection = Connection::Offline;
            retryAt = nowSeconds() + details.value("retryInSeconds", 0);
        } else if (event == "session_start" || event == "token_renewal") {
            connection = Connection::Connecting;
        } else {
            return;
        }
        std::lock_guard lock(mutex_);
        data_.connection = connection;
        data_.retryAt = retryAt;
        ++data_.version;
    }

    // Copies the data into `out` if it changed since `version`.
    bool copyIfNewer(std::uint64_t& version, LiveData& out) const {
        std::lock_guard lock(mutex_);
        if (data_.version == version) return false;
        out = data_;
        version = data_.version;
        return true;
    }

private:
    mutable std::mutex mutex_;
    LiveData data_;
    TargetSnapshotReader reader_;  // used on the feed thread only
};

// Runs the feed on its own thread and always stops it, even during unwinding.
class FeedThread {
public:
    explicit FeedThread(LiveFeed& feed) : feed_(feed), thread_([&feed] { feed.run(); }) {}
    ~FeedThread() {
        feed_.stop();
        thread_.join();
    }
    FeedThread(const FeedThread&) = delete;
    FeedThread& operator=(const FeedThread&) = delete;

private:
    LiveFeed& feed_;
    std::thread thread_;
};

StatusLine describeLiveStatus(const LiveData& data, double now) {
    StatusLine line;
    switch (data.connection) {
        case Connection::Live:
            line = {"наживо", kLiveDot};
            break;
        case Connection::Connecting:
            line = {"підключення…", kConnectingDot};
            break;
        case Connection::Offline: {
            const auto retryIn = static_cast<int>(std::ceil(std::max(0.0, data.retryAt - now)));
            line = {"немає з'єднання · повтор за " + std::to_string(retryIn) + " с", kOfflineDot};
            break;
        }
    }
    if (data.targets) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto ageSeconds = std::max<std::int64_t>(0, (nowMs - data.targets->serverTimeMs) / 1000);
        line.text += " · цілей: " + std::to_string(data.targets->targets.size()) + " · оновлено " +
                     std::to_string(ageSeconds) + " с тому";
    }
    if (data.alerts) line.text += " · регіонів з тривогою: " + std::to_string(data.alerts->any.size());
    return line;
}

void waitForFirstData(const LiveState& state, LiveData& data) {
    std::uint64_t version = 0;
    const auto deadline = std::chrono::steady_clock::now() + kScreenshotDataTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        state.copyIfNewer(version, data);
        if (data.targets && data.alerts) return;
        std::this_thread::sleep_for(kScreenshotPollInterval);
    }
    std::cerr << "alarmmap: live data incomplete after "
              << std::chrono::duration_cast<std::chrono::seconds>(kScreenshotDataTimeout).count()
              << " s, rendering what arrived\n";
}

// Mouse: wheel zooms at the cursor, left button drags. Keys: + and -
// zoom at the centre, 0, Home or R show the whole country. Returns whether
// the view changed.
bool handleInput(MapView& view) {
    const MapView before = view;
    const Vector2 mouse = GetMousePosition();
    if (const float wheel = GetMouseWheelMove(); wheel != 0) {
        view.zoomAt({mouse.x, mouse.y}, std::pow(kWheelZoomStep, wheel));
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const Vector2 delta = GetMouseDelta();
        if (delta.x != 0 || delta.y != 0) view.panByPixels(delta.x, delta.y);
    }
    const MapPoint middle{view.width() / 2, view.height() / 2};
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) view.zoomAt(middle, kKeyZoomStep);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) view.zoomAt(middle, 1 / kKeyZoomStep);
    if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_R)) view.reset();
    return !(view == before);
}

void renderFrame(Scene& scene, HeatRenderer& heat, const TargetIcons& icons, const MapView& view,
                 double now, const RenderTexture2D* target, std::optional<Vector2> pointer = std::nullopt) {
    const auto width = static_cast<int>(view.width());
    const auto height = static_cast<int>(view.height());
    const Camera2D camera = viewCamera(view);
    scene.prepare(view, now);
    // Offscreen heat passes switch render targets, so they run before the frame.
    if (heat.isStale(now, scene.dataVersion(), camera, width, height)) {
        heat.prepare(scene.heatQuadsAt(now), camera, width, height, now, scene.dataVersion());
    }
    if (target) {
        BeginTextureMode(*target);
    } else {
        BeginDrawing();
    }
    scene.draw(view, camera, heat, icons, now, pointer);
    if (target) {
        EndTextureMode();
    } else {
        EndDrawing();
    }
}

void saveScreenshot(Scene& scene, HeatRenderer& heat, const TargetIcons& icons,
                    const fs::path& path) {
    const RenderTexture2D target = LoadRenderTexture(kInitialWidth, kInitialHeight);
    MapView view;
    view.setViewport(kInitialWidth, kInitialHeight);
    // Render the moment new targets have finished appearing.
    const double settled = std::max(TargetMotion::kFadeInSeconds, kTeleportSeconds);
    renderFrame(scene, heat, icons, view, nowSeconds() + settled, &target);
    Image image = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&image);  // render textures are stored bottom-up
    // Blending leaves the render target's alpha below 1 in places; the window
    // ignores it, so the screenshot drops it too.
    ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
    const bool saved = ExportImage(image, path.c_str());
    UnloadImage(image);
    UnloadRenderTexture(target);
    if (!saved) throw std::runtime_error("cannot save screenshot " + path.string());
}

int run(const Options& options) {
    RegionShapes shapes = loadShapes();
    std::optional<TargetSnapshot> fileTargets;
    std::optional<RegionAlerts> fileAlerts;
    if (options.targets) fileTargets = decodeTargetSnapshot(readJsonFile(*options.targets));
    if (options.alerts) fileAlerts = activeRegionAlerts(readJsonFile(*options.alerts));

    LiveState state;
    LiveFeed feed({{kTargetsChannel, kTargetsSnapshotUrl}, {kAlertsChannel, kAlertsSnapshotUrl}},
                  [&state](const std::string& channel, const json& payload) {
                      state.onPublication(channel, payload);
                  },
                  [&state](const std::string& event, const json& details) {
                      state.onEvent(event, details);
                  });
    std::optional<FeedThread> feedThread;
    if (isLive(options)) feedThread.emplace(feed);

    // No FLAG_MSAA_4X_HINT: on the NVIDIA driver, interactive resizing of a
    // multisampled window triggers Xid 69 GPU errors that stall the compositor.
    unsigned flags = FLAG_WINDOW_RESIZABLE;
    if (options.screenshot) flags |= FLAG_WINDOW_HIDDEN;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
#ifdef __ANDROID__
    // Zero size takes the whole display; any other size is letterboxed into it.
    InitWindow(0, 0, "alarmmap");
#else
    InitWindow(kInitialWidth, kInitialHeight, "alarmmap");
#endif
    if (!IsWindowReady()) throw std::runtime_error("cannot open window");
    SetWindowMinSize(kMinimumWidth, kMinimumHeight);
    // Triangulated polygons and mirrored icon parts come in both windings.
    rlDisableBackfaceCulling();

    {
        // GPU resources must be created after the window opens and released
        // before it closes.
        const UiFont font(dataDirectory() / "fonts");
        Scene scene(std::move(shapes), font.get(), cacheDirectory() / "tiles");
        if (!isLive(options)) scene.apply(fileTargets, fileAlerts, nowSeconds());
        HeatRenderer heat;
        const TargetIcons icons;
        setWindowIcon(icons.forKind(TargetKind::Drone));
        LiveData liveData;
        std::uint64_t liveVersion = 0;

        if (options.screenshot) {
            if (isLive(options)) {
                waitForFirstData(state, liveData);
                scene.apply(liveData.targets, liveData.alerts, nowSeconds());
                scene.status = describeLiveStatus(liveData, nowSeconds());
            }
            saveScreenshot(scene, heat, icons, *options.screenshot);
            std::cout << "screenshot: " << *options.screenshot << '\n';
        } else if (options.benchmarkFrames) {
            SetTargetFPS(0);
            const double started = nowSeconds();
            MapView view;
            view.setViewport(GetScreenWidth(), GetScreenHeight());
            for (int frame = 0; frame < *options.benchmarkFrames && !WindowShouldClose(); ++frame) {
                renderFrame(scene, heat, icons, view, nowSeconds(), nullptr);
            }
            const double elapsed = nowSeconds() - started;
            std::cout << "frames=" << *options.benchmarkFrames
                      << " avg_ms=" << elapsed * 1000 / *options.benchmarkFrames << '\n';
        } else {
            // Paced here instead of with SetTargetFPS: raylib's limiter
            // busy-waits through the end of every frame.
            const auto frameDuration = [](double framesPerSecond) {
                return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(1 / framesPerSecond));
            };
            auto nextFrame = std::chrono::steady_clock::now();
            MapView view;
            double lastInteraction = -kInteractionHoldSeconds;
            while (!WindowShouldClose()) {
                const double now = nowSeconds();
                view.setViewport(GetScreenWidth(), GetScreenHeight());
                if (handleInput(view)) lastInteraction = now;
                if (isLive(options)) {
                    if (state.copyIfNewer(liveVersion, liveData)) {
                        scene.apply(liveData.targets, liveData.alerts, now);
                    }
                    // Refreshed every frame so the age of the snapshot keeps counting.
                    scene.status = describeLiveStatus(liveData, nowSeconds());
                }
                // No hover while dragging the map.
                const bool hovering = IsCursorOnScreen() && !IsMouseButtonDown(MOUSE_BUTTON_LEFT);
                renderFrame(scene, heat, icons, view, now, nullptr,
                            hovering ? std::optional(GetMousePosition()) : std::nullopt);
                const bool interacting = now - lastInteraction < kInteractionHoldSeconds;
                // A late frame resets the schedule instead of rushing to catch up.
                nextFrame = std::max(
                    nextFrame + frameDuration(interacting ? kInteractiveFramesPerSecond : kFramesPerSecond),
                    std::chrono::steady_clock::now());
                std::this_thread::sleep_until(nextFrame);
            }
        }
    }
    CloseWindow();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifndef __ANDROID__
    // Before any thread starts: curl_global_init is not thread-safe.
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    int exitCode = 1;
    try {
        Options options = parseOptions(argc, argv);
#ifdef __ANDROID__
        // Without network access the app shows the snapshot its APK carries.
        extractAssets();
        options.targets = androidStorage() / "targets.json";
        options.alerts = androidStorage() / "alerts.json";
#endif
        if (options.version) {
            std::cout << "alarmmap " ALARMMAP_VERSION "\n";
            exitCode = 0;
        } else {
            exitCode = run(options);
        }
    } catch (const std::exception& error) {
        std::cerr << "alarmmap: " << error.what() << '\n';
#ifdef __ANDROID__
        // Standard error goes nowhere on Android; raylib's log reaches logcat.
        TraceLog(LOG_ERROR, "alarmmap: %s", error.what());
#endif
    }
#ifndef __ANDROID__
    curl_global_cleanup();
#endif
    return exitCode;
}
