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
#include "live_feed.hpp"
#include "map_layer.hpp"
#include "map_update.hpp"
#include "map_view.hpp"
#include "marker_layout.hpp"
#include "projection.hpp"
#include "region_geometry.hpp"
#include "region_store.hpp"
#include "site.hpp"
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

#include <curl/curl.h>

#ifdef __ANDROID__
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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

// Region shapes prepared at build time by tools/geometry.
constexpr const char* kPreparedRegionsFile = "regions.bin";
constexpr const char* kCitiesFile = "cities.geojson";
constexpr const char* kTileJsonUrl = "https://tiles.openfreemap.org/planet";
constexpr auto kScreenshotDataTimeout = std::chrono::seconds(30);
constexpr auto kScreenshotPollInterval = std::chrono::milliseconds(100);

// Animations here are slow (20 s glides, multi-second fades); 30 frames per
// second look the same as 60 at half the cost. Panning and zooming get 60
// while the user interacts and shortly after.
constexpr double kFramesPerSecond = 30;
constexpr double kInteractiveFramesPerSecond = 60;
constexpr double kInteractionHoldSeconds = 1;
#ifndef __ANDROID__
constexpr double kWheelZoomStep = 1.25;
#endif
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
// Markers start at this share of their size on the whole-country view and
// reach full size at kFullMarkerZoom, growing with the logarithm of zoom.
constexpr float kMarkerShareAtMinZoom = 0.7f;
constexpr double kFullMarkerZoom = 4;
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
// Connected, but the targets shown are not current.
constexpr Color kDegradedDot = {240, 180, 60, 255};
constexpr Color kOfflineDot = {235, 80, 70, 255};
constexpr Color kPanelBackground = {12, 15, 20, 200};
constexpr Color kPanelBorder = {70, 80, 96, 160};
constexpr Color kPanelText = {205, 212, 224, 255};
constexpr Color kScreenClear = BLACK;
constexpr Color kCreditsColor = {120, 128, 142, 200};
#ifdef __ANDROID__
// A phone has no room to spare; the credits only need to be there.
constexpr float kCreditsFontPx = 6;
#else
constexpr float kCreditsFontPx = 12;
#endif
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
// A fingertip covers more than a cursor, so a tap reaches at least this far.
constexpr float kTapReachPx = 24;
// A touch that travels further than this is a drag, not a tap.
constexpr float kTapSlopPx = 12;
// Eight quick taps on Kyiv send every target on the map back to Russia.
constexpr int kTapsToRetaliate = 8;
constexpr double kTapStreakGapSeconds = 1.0;
constexpr float kCapitalReachPx = 24;
constexpr double kTurnSeconds = 0.7;
constexpr double kReturnFlightSeconds = 2.6;
constexpr double kMaxLaunchStaggerSeconds = 0.9;
// Launched from around Kyiv when the sky happens to be empty.
constexpr int kStandInDrones = 6;
constexpr double kStandInSpreadUnits = 3;
constexpr float kTooltipGapPx = 10;
constexpr float kTooltipPaddingPx = 8;

struct StatusLine {
    std::string text;
    Color dot;
};

// What the user singles out: the mouse hovering on the desktop, or the
// target last tapped on a touch screen. Its name and path are shown.
struct Focus {
    std::optional<Vector2> pointer;
    std::optional<std::string> trackId;
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
constexpr const char* kBundledAssets[] = {"cities.geojson", "fonts/NotoSans-Medium.ttf"};

fs::path androidStorage() { return GetAndroidApp()->activity->internalDataPath; }

// Standard output and error go nowhere on Android; a thread copies them to
// logcat line by line.
void redirectOutputToLogcat() {
    int pipeEnds[2];
    if (pipe(pipeEnds) != 0) return;
    std::cout.setf(std::ios::unitbuf);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(pipeEnds[1], STDOUT_FILENO);
    dup2(pipeEnds[1], STDERR_FILENO);
    std::thread([readEnd = pipeEnds[0]] {
        std::string line;
        char buffer[512];
        ssize_t count;
        while ((count = read(readEnd, buffer, sizeof buffer)) > 0) {
            for (ssize_t i = 0; i < count; ++i) {
                if (buffer[i] != '\n') {
                    line += buffer[i];
                    continue;
                }
                __android_log_write(ANDROID_LOG_INFO, "alarmmap", line.c_str());
                line.clear();
            }
        }
    }).detach();
}

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

#ifdef __ANDROID__
// Read straight from the APK: copying it to storage first would only add a
// write of several megabytes to every start.
std::string preparedRegionsBytes() {
    AAsset* asset = AAssetManager_open(GetAndroidApp()->activity->assetManager, kPreparedRegionsFile,
                                       AASSET_MODE_BUFFER);
    if (!asset) throw std::runtime_error(std::string("missing asset ") + kPreparedRegionsFile);
    const auto* data = static_cast<const char*>(AAsset_getBuffer(asset));
    std::string bytes;
    if (data) bytes.assign(data, static_cast<size_t>(AAsset_getLength64(asset)));
    AAsset_close(asset);
    if (!data) throw std::runtime_error(std::string("cannot read asset ") + kPreparedRegionsFile);
    return bytes;
}
#else
// An install keeps the file with the other data; a build tree has it in the
// build directory.
std::string preparedRegionsBytes() {
    fs::path path = dataDirectory() / kPreparedRegionsFile;
    if (!fs::exists(path)) path = ALARMMAP_PREPARED_REGIONS;
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
#endif

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
    PreparedRegions regions = readPreparedRegions(preparedRegionsBytes());
    RegionShapes shapes{std::move(regions.coarse), std::move(regions.detailed),
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
          mapLayer_(shapes_.coarse, kCoarseTolerance, shapes_.detailed, shapes_.cities, tiles_, font_),
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
    void prepare(const MapView& view, double now, bool gestureActive) {
        const double growth = std::clamp(std::log2(view.zoom()) / std::log2(kFullMarkerZoom), 0.0, 1.0);
        const double share = kMarkerShareAtMinZoom + (1 - kMarkerShareAtMinZoom) * growth;
        markerScale_ = markerScale() * static_cast<float>(share);
        if (retaliation_ && !retaliating(now)) retaliation_.reset();
        mapLayer_.prepare(view, now, gestureActive);
    }

    void draw(const MapView& view, const Camera2D& camera, const HeatRenderer& heat,
              const TargetIcons& icons, double now, const Focus& focus) const {
        const auto width = static_cast<float>(view.width());
        const auto height = static_cast<float>(view.height());
        ClearBackground(kScreenClear);
        mapLayer_.draw(view);
        mapLayer_.drawHighlights(freshAlertPulses(), view);
        heat.composite();
        mapLayer_.drawVignette(width, height);
        const std::vector<TargetMotion::Marker> markers = visibleMarkers(now);
        const float ui = uiScale();
        if (retaliating(now)) {
            drawRetaliation(camera, icons, now);
            drawScaledAround({0, 0}, ui, [&] {
                drawSummary(markers, icons, static_cast<int>(height / ui));
                if (status) drawStatus(*status);
                drawCredits(width / ui, height / ui);
            });
            return;
        }
        const std::vector<MarkerPlacement> placements = layout(markers, camera);
        std::vector<TargetMotion::Explosion> explosions = motion_.explosionsAt(now);
        std::erase_if(explosions, [&](const TargetMotion::Explosion& explosion) {
            return !visibleTracks_.contains(explosion.target.trackId);
        });
        for (const auto& explosion : explosions) {
            const Vector2 center = GetWorldToScreen2D(toVector(explosion.position), camera);
            drawScaledAround(center, markerScale_, [&] {
                drawScorch(center, explosion.age, TargetMotion::kAftermathSeconds,
                           effectSeed(explosion.target.trackId));
            });
        }
        const MarkerPlacement* hovered = focusedPlacement(placements, markers, camera, focus);
        if (hovered) drawPaths(*hovered, markers, camera, now);
        drawTargets(markers, placements, camera, icons);
        for (const auto& placement : placements) {
            const TargetMotion::Marker& marker = markers[placement.index];
            const Vector2 center = screenPosition(placement, markers, camera);
            drawScaledAround(center, markerScale_,
                             [&] { drawTeleport(center, marker.age, effectSeed(marker.target.trackId)); });
        }
        for (const auto& explosion : explosions) {
            const Vector2 center = GetWorldToScreen2D(toVector(explosion.position), camera);
            drawScaledAround(center, markerScale_, [&] {
                drawExplosion(center, explosion.age, effectSeed(explosion.target.trackId));
            });
        }
        // Overlays are laid out in unscaled pixels over a window shrunk by the
        // UI scale, then drawn scaled up.
        drawScaledAround({0, 0}, ui, [&] {
            if (hovered) drawTooltip(*hovered, markers, camera, width / ui, height / ui);
            drawSummary(markers, icons, static_cast<int>(height / ui));
            if (status) drawStatus(*status);
            drawCredits(width / ui, height / ui);
        });
    }

    // Track of the target marker under a tap, if any.
    std::optional<std::string> trackAt(const Camera2D& camera, double now, Vector2 point) const {
        const std::vector<TargetMotion::Marker> markers = visibleMarkers(now);
        const std::vector<MarkerPlacement> placements = layout(markers, camera);
        const MarkerPlacement* placement = placementAt(placements, markers, camera, point, kTapReachPx * uiScale());
        if (!placement) return std::nullopt;
        return markers[placement->index].target.trackId;
    }

    // Turns every target on the map round and sends it to a random point in
    // Russia, where it goes off as a mushroom cloud. Only the drawing changes;
    // the live data goes on and the real markers return afterwards.
    void retaliate(double now) {
        std::mt19937 random(static_cast<std::uint32_t>(now * 1000));
        std::uniform_real_distribution<double> unit(0, 1);
        Retaliation strike;
        strike.startedAt = now;
        for (const TargetMotion::Marker& marker : visibleMarkers(now)) {
            strike.flights.push_back({marker.position, {},
                                      static_cast<float>(marker.target.headingDegrees.value_or(0)),
                                      targetKind(marker.target.iconName, marker.target.detectedType),
                                      0, effectSeed(marker.target.trackId)});
        }
        if (strike.flights.empty()) {
            const MapPoint kyiv = capitalPosition();
            for (int i = 0; i < kStandInDrones; ++i) {
                const MapPoint from = {kyiv.x + (unit(random) * 2 - 1) * kStandInSpreadUnits,
                                       kyiv.y + (unit(random) * 2 - 1) * kStandInSpreadUnits};
                strike.flights.push_back({from, {}, static_cast<float>(unit(random) * 360), TargetKind::Drone, 0,
                                          static_cast<std::uint32_t>(random())});
            }
        }
        strike.endsAt = now;
        for (Retaliation::Flight& flight : strike.flights) {
            flight.to = randomPointInRussia(random);
            flight.delay = unit(random) * kMaxLaunchStaggerSeconds;
            strike.endsAt =
                std::max(strike.endsAt, now + flight.delay + kTurnSeconds + kReturnFlightSeconds + kMushroomSeconds);
        }
        retaliation_ = std::move(strike);
    }

    bool retaliating(double now) const { return retaliation_ && now < retaliation_->endsAt; }

    // Whether `point` on screen falls on Kyiv's label dot.
    bool isCapitalAt(const Camera2D& camera, Vector2 point) const {
        const Vector2 kyiv = GetWorldToScreen2D(toVector(capitalPosition()), camera);
        return std::hypot(kyiv.x - point.x, kyiv.y - point.y) <= kCapitalReachPx * uiScale();
    }

    std::optional<StatusLine> status;

private:
    MapPoint capitalPosition() const {
        const auto capital = std::find_if(shapes_.cities.begin(), shapes_.cities.end(),
                                          [](const City& city) { return city.kind == CityKind::Capital; });
        return capital != shapes_.cities.end() ? capital->position : lonLatToMap(30.5234, 50.4501);
    }

    // A point inside Russia beyond Ukraine's eastern or north-eastern border,
    // where the whole-country view shows a mushroom cloud whole even in a
    // 16:10 window.
    static MapPoint randomPointInRussia(std::mt19937& random) {
        struct Area {
            double west, east, south, north;
        };
        constexpr Area kEast = {40.5, 41.3, 47.4, 50.0};
        constexpr Area kNorthEast = {36.5, 40.3, 50.9, 51.9};
        std::uniform_real_distribution<double> unit(0, 1);
        const Area& area = unit(random) < 0.35 ? kEast : kNorthEast;
        return lonLatToMap(area.west + (area.east - area.west) * unit(random),
                           area.south + (area.north - area.south) * unit(random));
    }

    void drawRetaliation(const Camera2D& camera, const TargetIcons& icons, double now) const {
        const double elapsed = now - retaliation_->startedAt;
        const double arrival = kTurnSeconds + kReturnFlightSeconds;
        for (const Retaliation::Flight& flight : retaliation_->flights) {
            const double age = elapsed - flight.delay - arrival;
            if (age < 0 || age >= kMushroomSeconds) continue;
            const Vector2 ground = GetWorldToScreen2D(toVector(flight.to), camera);
            drawScaledAround(ground, markerScale_, [&] { drawMushroom(ground, age, flight.seed); });
        }
        const float size = kTargetIconPx * markerScale_;
        for (const bool shadow : {true, false}) {
            for (const Retaliation::Flight& flight : retaliation_->flights) {
                const double local = elapsed - flight.delay;
                if (local >= arrival) continue;
                const double dx = flight.to.x - flight.from.x;
                const double dy = flight.to.y - flight.from.y;
                const auto bearing = static_cast<float>(std::atan2(dx, -dy) * 180 / PI);
                float heading = bearing;
                MapPoint position = flight.from;
                if (local < kTurnSeconds) {
                    const auto turned = static_cast<float>(std::clamp(local / kTurnSeconds, 0.0, 1.0));
                    const float delta = std::fmod(bearing - flight.heading + 540.0f, 360.0f) - 180;
                    heading = flight.heading + delta * turned * turned * (3 - 2 * turned);
                } else {
                    // Gathering speed like a launch.
                    const double flown = std::pow((local - kTurnSeconds) / kReturnFlightSeconds, 2);
                    position = {flight.from.x + dx * flown, flight.from.y + dy * flown};
                }
                const Vector2 at = GetWorldToScreen2D(toVector(position), camera);
                const SnoutIcon& icon = icons.forKind(flight.kind);
                if (shadow) {
                    icon.draw({at.x + kShadowOffsetPx.x * markerScale_, at.y + kShadowOffsetPx.y * markerScale_}, size,
                              heading, Fade(BLACK, kShadowOpacity));
                } else {
                    icon.draw(at, size, heading, WHITE);
                }
            }
        }
    }

    std::vector<TargetMotion::Marker> visibleMarkers(double now) const {
        std::vector<TargetMotion::Marker> markers = motion_.markersAt(now);
        std::erase_if(markers, [&](const TargetMotion::Marker& marker) {
            return !visibleTracks_.contains(marker.target.trackId);
        });
        return markers;
    }

    std::vector<MarkerPlacement> layout(const std::vector<TargetMotion::Marker>& markers,
                                        const Camera2D& camera) const {
        // The layout works in unscaled pixels; screenPosition() scales its offsets.
        return layoutMarkers(markers, camera.zoom / markerScale_);
    }

    const MarkerPlacement* focusedPlacement(const std::vector<MarkerPlacement>& placements,
                                                   const std::vector<TargetMotion::Marker>& markers,
                                            const Camera2D& camera, const Focus& focus) const {
        if (focus.pointer) return placementAt(placements, markers, camera, *focus.pointer, 0);
        if (!focus.trackId) return nullptr;
        const auto found = std::find_if(placements.begin(), placements.end(), [&](const MarkerPlacement& placement) {
            return std::any_of(placement.members.begin(), placement.members.end(),
                               [&](size_t index) { return markers[index].target.trackId == *focus.trackId; });
        });
        return found == placements.end() ? nullptr : &*found;
    }

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
#ifndef __ANDROID__
        // On a phone the dot alone tells the connection state; the text would
        // cover the map.
        DrawTextEx(font_, line.text.c_str(),
                   {kStatusMarginPx + 3 * kStatusDotRadiusPx, kStatusMarginPx}, kStatusFontPx,
                   kTextSpacingPx, kStatusColor);
#endif
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

    Vector2 screenPosition(const MarkerPlacement& placement, const std::vector<TargetMotion::Marker>& markers,
                           const Camera2D& camera) const {
        const Vector2 position = GetWorldToScreen2D(toVector(markers[placement.index].position), camera);
        return Vector2{position.x + static_cast<float>(placement.offsetPx) * markerScale_, position.y};
    }

    // The marker at `pointer`, the nearest one where markers overlap. The
    // reach covers most of the icon and at least `minimumReachPx`.
    const MarkerPlacement* placementAt(const std::vector<MarkerPlacement>& placements,
                                       const std::vector<TargetMotion::Marker>& markers, const Camera2D& camera,
                                       Vector2 pointer, float minimumReachPx) const {
        const MarkerPlacement* nearest = nullptr;
        float nearestDistance = 0;
        for (const auto& placement : placements) {
            const Vector2 position = screenPosition(placement, markers, camera);
            const float distance = std::hypot(position.x - pointer.x, position.y - pointer.y);
            const float radius = std::max(
                kTargetIconPx * markerScale_ * static_cast<float>(placement.scale) * kHoverRadiusShare,
                minimumReachPx);
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
        const float iconHalf = kTargetIconPx * markerScale_ / uiScale() * static_cast<float>(placement.scale) / 2;
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
            return this->screenPosition(placement, markers, camera);
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
                const float size = kTargetIconPx * markerScale_ * iconScale(placement);
                // Without a heading the icon points north.
                const auto heading = static_cast<float>(marker.target.headingDegrees.value_or(0));
                const SnoutIcon& icon = icons.forTarget(marker.target);
                if (shadow) {
                    const Vector2 shadowAt = {position.x + kShadowOffsetPx.x * markerScale_,
                                              position.y + kShadowOffsetPx.y * markerScale_};
                    icon.draw(shadowAt, size, heading, Fade(BLACK, kShadowOpacity * opacity));
                } else {
                    icon.draw(position, size, heading, Fade(WHITE, opacity));
                }
            }
        }
        for (const auto& placement : placements) {
            if (placement.count > 1) {
                // The badge text keeps the UI size however small the marker is;
                // only its place beside the icon follows the marker's size.
                const Vector2 center = screenPosition(placement);
                const float iconScale = static_cast<float>(placement.scale) * markerScale_ / uiScale();
                drawScaledAround(center, uiScale(), [&] {
                    drawCountBadge(center, iconScale, placement.count,
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
    // Size multiplier of target markers for the current zoom.
    float markerScale_ = 1;

    struct Retaliation {
        struct Flight {
            MapPoint from;
            MapPoint to;
            float heading;
            TargetKind kind;
            double delay;
            std::uint32_t seed;
        };
        double startedAt = 0;
        double endsAt = 0;
        std::vector<Flight> flights;
    };
    std::optional<Retaliation> retaliation_;
    std::vector<HeatCandidate> heatCandidates_;
    std::unordered_set<std::string> visibleTracks_;
    TargetMotion motion_;
    std::uint64_t dataVersion_ = 0;
};

// Latest live data, written on the feed thread and copied by the renderer.
enum class Connection { Connecting, Live, Offline };

struct LiveData {
    std::optional<TargetSnapshot> targets;
    // Local clock (nowSeconds), so that a skewed system clock cannot make
    // fresh data look stale.
    double targetsReceivedAt = 0;
    std::optional<RegionAlerts> alerts;
    std::optional<std::int64_t> alertsActionIndex;
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
            if (snapshot->skippedEntries > 0) {
                std::cout << "feed: targets read in part: skipped entries=" << snapshot->skippedEntries << '\n';
            }
            reportUnknownIcons(*snapshot);
            std::lock_guard lock(mutex_);
            // The REST snapshot and a queued publication can arrive out of order.
            if (data_.targets && snapshot->serverTimeMs < data_.targets->serverTimeMs) return;
            data_.targets = std::move(snapshot);
            data_.targetsReceivedAt = nowSeconds();
            ++data_.version;
        } else if (channel == kAlertsChannel) {
            AlertUpdate update = readAlertUpdate(payload);
            if (update.skippedRegions > 0 || !update.actionIndex) {
                std::cout << "feed: updateMap read in part: skipped regions=" << update.skippedRegions
                          << (update.actionIndex ? "" : ", no action index") << '\n';
            }
            std::lock_guard lock(mutex_);
            // A late snapshot must not roll back a newer publication; without
            // indexes on both the order is unknown and the newest arrival wins.
            if (data_.alerts && update.actionIndex && data_.alertsActionIndex &&
                *update.actionIndex < *data_.alertsActionIndex) {
                return;
            }
            data_.alerts = std::move(update.alerts);
            data_.alertsActionIndex = update.actionIndex;
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
    // Feed thread. Once per icon name, so that a new kind of target shows up
    // in the log without flooding it.
    void reportUnknownIcons(const TargetSnapshot& snapshot) {
        for (const Target& target : snapshot.targets) {
            if (!isIdentified(target.iconName, target.detectedType) ||
                targetKind(target.iconName, target.detectedType) != TargetKind::Unknown ||
                !reportedIcons_.insert(target.iconName).second) {
                continue;
            }
            std::cout << "feed: unknown target icon " << target.iconName << " (detected type "
                      << target.detectedType << ")\n";
        }
    }

    mutable std::mutex mutex_;
    LiveData data_;
    TargetSnapshotReader reader_;  // used on the feed thread only
    std::unordered_set<std::string> reportedIcons_;  // used on the feed thread only
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

// The live feed and the thread running it, started and stopped together.
struct LiveConnection {
    explicit LiveConnection(LiveState& state)
        : feed({{kTargetsChannel, "", true}, {kAlertsChannel, kAlertsSnapshotUrl}},
               [&state](const std::string& channel, const json& payload) { state.onPublication(channel, payload); },
               [&state](const std::string& event, const json& details) { state.onEvent(event, details); }),
          thread(feed) {}

    LiveFeed feed;
    FeedThread thread;  // after `feed`, so it stops before the feed goes away
};

#ifdef __ANDROID__
// Android reports the app's lifecycle through raylib's command callback.
// This hook runs ahead of it and stops the live feed once the app goes to
// the background, where the window is not drawn and the connection would
// only drain the battery. Both run on the main loop's thread.
struct BackgroundWatch {
    LiveFeed* feed = nullptr;
    bool feedStopped = false;
    void (*forward)(android_app*, int32_t) = nullptr;
};
BackgroundWatch backgroundWatch;

void onAppCommand(android_app* app, int32_t command) {
    if (command == APP_CMD_STOP && backgroundWatch.feed) {
        backgroundWatch.feed->stop();
        backgroundWatch.feedStopped = true;
    }
    backgroundWatch.forward(app, command);
}

void watchAppLifecycle() {
    android_app* app = GetAndroidApp();
    backgroundWatch.forward = app->onAppCmd;
    app->onAppCmd = onAppCommand;
}
#endif

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
    const bool live = data.connection == Connection::Live;
    if (data.targets && data.targets->targetsPaused) {
        if (live) line = {"показ цілей призупинено сайтом", kDegradedDot};
        else line.text += " · показ цілей призупинено сайтом";
    } else if (data.targets) {
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        const auto ageSeconds = std::max<std::int64_t>(0, (nowMs - data.targets->serverTimeMs) / 1000);
        const double ttl = data.targets->snapshotTtlSeconds;
        if (live && ttl > 0 && now - data.targetsReceivedAt > ttl) line = {"дані застаріли", kDegradedDot};
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

#ifndef __ANDROID__
// Wheel zooms at the cursor, left button drags.
void handleMouse(MapView& view) {
    const Vector2 mouse = GetMousePosition();
    if (const float wheel = GetMouseWheelMove(); wheel != 0) {
        view.zoomAt({mouse.x, mouse.y}, std::pow(kWheelZoomStep, wheel));
    }
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        const Vector2 delta = GetMouseDelta();
        if (delta.x != 0 || delta.y != 0) view.panByPixels(delta.x, delta.y);
    }
}
#endif

// + and - zoom at the centre, 0, Home or R show the whole country.
void handleKeys(MapView& view) {
    const MapPoint middle{view.width() / 2, view.height() / 2};
    if (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD)) view.zoomAt(middle, kKeyZoomStep);
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT)) view.zoomAt(middle, 1 / kKeyZoomStep);
    if (IsKeyPressed(KEY_ZERO) || IsKeyPressed(KEY_HOME) || IsKeyPressed(KEY_R)) view.reset();
}

// One finger drags the map, two pinch-zoom around their midpoint; a touch
// that ends where it started is a tap. Fingers are followed by id, so the
// map does not jump when a second finger lands or lifts.
class TouchInput {
public:
    // Returns the position of a tap that ended this frame.
    std::optional<Vector2> update(MapView& view) {
        std::map<int, Vector2> touches;
        for (int i = 0; i < GetTouchPointCount(); ++i) touches[GetTouchPointId(i)] = GetTouchPosition(i);

        if (touches.size() == 1 && previous_.contains(touches.begin()->first)) {
            const Vector2 now = touches.begin()->second;
            const Vector2 before = previous_.at(touches.begin()->first);
            view.panByPixels(now.x - before.x, now.y - before.y);
        } else if (touches.size() >= 2) {
            const auto first = touches.begin();
            const auto second = std::next(first);
            if (previous_.contains(first->first) && previous_.contains(second->first)) {
                const Vector2 a0 = previous_.at(first->first);
                const Vector2 b0 = previous_.at(second->first);
                const Vector2 a1 = first->second;
                const Vector2 b1 = second->second;
                const float before = std::hypot(b0.x - a0.x, b0.y - a0.y);
                const float after = std::hypot(b1.x - a1.x, b1.y - a1.y);
                const Vector2 middleBefore = {(a0.x + b0.x) / 2, (a0.y + b0.y) / 2};
                const Vector2 middleAfter = {(a1.x + b1.x) / 2, (a1.y + b1.y) / 2};
                if (before > 0 && after > 0) view.zoomAt({middleBefore.x, middleBefore.y}, after / before);
                view.panByPixels(middleAfter.x - middleBefore.x, middleAfter.y - middleBefore.y);
            }
        }

        std::optional<Vector2> tap;
        if (previous_.empty() && touches.size() == 1) {
            tapStart_ = touches.begin()->second;
            tapPossible_ = true;
        }
        if (touches.size() > 1) tapPossible_ = false;
        if (touches.size() == 1 && tapPossible_) {
            const Vector2 at = touches.begin()->second;
            if (std::hypot(at.x - tapStart_.x, at.y - tapStart_.y) > kTapSlopPx * uiScale()) tapPossible_ = false;
        }
        if (touches.empty() && !previous_.empty() && tapPossible_) tap = tapStart_;
        if (touches.empty()) tapPossible_ = false;
        previous_ = std::move(touches);
        return tap;
    }

    // Whether a finger is on the screen.
    bool active() const { return !previous_.empty(); }

private:
    std::map<int, Vector2> previous_;
    Vector2 tapStart_{};
    bool tapPossible_ = false;
};

void renderFrame(Scene& scene, HeatRenderer& heat, const TargetIcons& icons, const MapView& view,
                 double now, const RenderTexture2D* target, const Focus& focus = {},
                 bool gestureActive = false) {
    const auto width = static_cast<int>(view.width());
    const auto height = static_cast<int>(view.height());
    const Camera2D camera = viewCamera(view);
    scene.prepare(view, now, gestureActive);
    // Offscreen heat passes switch render targets, so they run before the frame.
    if (heat.isStale(now, scene.dataVersion(), camera, width, height)) {
        heat.prepare(scene.heatQuadsAt(now), camera, width, height, now, scene.dataVersion());
    }
    if (target) {
        BeginTextureMode(*target);
    } else {
        BeginDrawing();
    }
    scene.draw(view, camera, heat, icons, now, focus);
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
    std::unique_ptr<LiveConnection> connection;
    if (isLive(options)) connection = std::make_unique<LiveConnection>(state);

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
#ifdef __ANDROID__
    // raylib installs its command callback while opening the window.
    watchAppLifecycle();
    if (connection) backgroundWatch.feed = &connection->feed;
#endif
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
            Focus focus;
            // Quick taps on Kyiv in a row, for the retaliation.
            int capitalTaps = 0;
            double lastCapitalTap = -kTapStreakGapSeconds;
#ifndef __ANDROID__
            Vector2 pressedAt{};
#endif
#ifdef __ANDROID__
            TouchInput touch;
#endif
            while (!WindowShouldClose()) {
                const double now = nowSeconds();
                view.setViewport(GetScreenWidth(), GetScreenHeight(), uiScale());
                const MapView before = view;
                handleKeys(view);
#ifdef __ANDROID__
                // A tap on a target shows its name and path until the next tap.
                const std::optional<Vector2> tap = touch.update(view);
                if (tap) focus.trackId = scene.trackAt(viewCamera(view), now, *tap);
#else
                handleMouse(view);
                // No hover while dragging the map.
                const bool hovering = IsCursorOnScreen() && !IsMouseButtonDown(MOUSE_BUTTON_LEFT);
                focus.pointer = hovering ? std::optional(GetMousePosition()) : std::nullopt;
                // A click is a press and release without dragging in between.
                std::optional<Vector2> tap;
                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) pressedAt = GetMousePosition();
                if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
                    const Vector2 releasedAt = GetMousePosition();
                    if (std::hypot(releasedAt.x - pressedAt.x, releasedAt.y - pressedAt.y) <= kTapSlopPx) {
                        tap = releasedAt;
                    }
                }
#endif
                if (tap) {
                    if (scene.isCapitalAt(viewCamera(view), *tap)) {
                        capitalTaps = now - lastCapitalTap <= kTapStreakGapSeconds ? capitalTaps + 1 : 1;
                        lastCapitalTap = now;
                        if (capitalTaps == kTapsToRetaliate) {
                            capitalTaps = 0;
                            scene.retaliate(now);
                        }
                    } else {
                        capitalTaps = 0;
                    }
                }
                if (!(view == before) || scene.retaliating(now)) lastInteraction = now;
#ifdef __ANDROID__
                if (backgroundWatch.feedStopped) {
                    // Back from the background: a new connection fetches the
                    // current state.
                    backgroundWatch.feed = nullptr;
                    connection.reset();
                    connection = std::make_unique<LiveConnection>(state);
                    backgroundWatch.feed = &connection->feed;
                    backgroundWatch.feedStopped = false;
                }
#endif
                if (isLive(options)) {
                    if (state.copyIfNewer(liveVersion, liveData)) {
                        scene.apply(liveData.targets, liveData.alerts, now);
                    }
                    // Refreshed every frame so the age of the snapshot keeps counting.
                    scene.status = describeLiveStatus(liveData, nowSeconds());
                }
#ifdef __ANDROID__
                renderFrame(scene, heat, icons, view, now, nullptr, focus, touch.active());
#else
                renderFrame(scene, heat, icons, view, now, nullptr, focus);
#endif
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
    // Before any thread starts: curl_global_init is not thread-safe.
    curl_global_init(CURL_GLOBAL_DEFAULT);
#ifdef __ANDROID__
    redirectOutputToLogcat();
#endif
    int exitCode = 1;
    try {
        const Options options = parseOptions(argc, argv);
#ifdef __ANDROID__
        extractAssets();
#endif
        if (options.version) {
            std::cout << "alarmmap " ALARMMAP_VERSION "\n";
            exitCode = 0;
        } else {
            exitCode = run(options);
        }
    } catch (const std::exception& error) {
        std::cerr << "alarmmap: " << error.what() << '\n';
    }
    curl_global_cleanup();
#ifdef __ANDROID__
    // Android keeps a finished app's process and would start the next launch
    // in it, with this code's and raylib's state left from the last run.
    std::exit(exitCode);
#endif
    return exitCode;
}
