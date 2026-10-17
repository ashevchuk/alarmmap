// Map of Ukraine with region borders, active alerts, the heat trail of targets
// and the targets themselves.
//
// Usage: alarmmap [--targets FILE] [--alerts FILE] [--screenshot PNG] [--benchmark FRAMES]
//   --targets     target snapshot (compact v2 packet or REST JSON)
//   --alerts      updateMap payload
//   --screenshot  render one frame into PNG and exit
//   --benchmark   render FRAMES frames without a frame rate cap, print the
//                 average frame time and exit
// Without --targets and --alerts the data comes live from map.ukrainealarm.com.

#include "heat_cells.hpp"
#include "heat_renderer.hpp"
#include "http.hpp"
#include "live_feed.hpp"
#include "map_layer.hpp"
#include "map_update.hpp"
#include "marker_layout.hpp"
#include "projection.hpp"
#include "region_geometry.hpp"
#include "snout_icon.hpp"
#include "target_kind.hpp"
#include "target_motion.hpp"
#include "target_snapshot.hpp"
#include "ui_font.hpp"

#include <curl/curl.h>
#include <raylib.h>
#include <rlgl.h>

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
constexpr const char* kCrimeaOverrideFile = ALARMMAP_DATA_DIR "/crimea.geojson";
constexpr const char* kTargetsChannel = "mapTargets_heat_legacy_v2";
constexpr const char* kTargetsSnapshotUrl =
    "https://map.ukrainealarm.com/api/v2/data/targetsForMapsState/public?separateTracks=false";
constexpr const char* kAlertsChannel = "updateMap";
constexpr const char* kAlertsSnapshotUrl = "https://map.ukrainealarm.com/api/v2/data/mapUpdate";
constexpr auto kScreenshotDataTimeout = std::chrono::seconds(30);
constexpr auto kScreenshotPollInterval = std::chrono::milliseconds(100);
// About 240 m on the ground: under one pixel even with the country on a 4K screen.
constexpr double kSimplifyTolerance = 0.05;

// Animations here are slow (20 s glides, multi-second fades); 30 frames per
// second look the same as 60 at half the cost.
constexpr double kFramesPerSecond = 30;
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
constexpr float kPanelWidthPx = 230;
constexpr float kTargetIconPx = 34;
constexpr float kHaloPx = 76;
constexpr int kHaloTextureSize = 128;
constexpr Color kHaloColor = {255, 214, 100, 255};
constexpr float kHaloOpacity = 0.85f;
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

struct StatusLine {
    std::string text;
    Color dot;
};

struct Options {
    std::optional<fs::path> targets;
    std::optional<fs::path> alerts;
    std::optional<fs::path> screenshot;
    std::optional<int> benchmarkFrames;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
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

fs::path cacheDirectory() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        return fs::path(xdg) / "alarmmap";
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("neither XDG_CACHE_HOME nor HOME is set");
    return fs::path(home) / ".cache" / "alarmmap";
}

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
// One icon texture per target kind and the soft halo drawn under targets.
class TargetIcons {
public:
    TargetIcons() {
        for (const TargetKind kind : kAllTargetKinds) icons_[kind] = std::make_unique<SnoutIcon>(kind);
        // Fades to black rather than to transparent: halos are drawn with MAX
        // blending, which ignores alpha.
        Image image = GenImageGradientRadial(kHaloTextureSize, kHaloTextureSize, 0, kHaloColor, BLACK);
        halo_ = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(halo_, TEXTURE_FILTER_BILINEAR);
    }
    ~TargetIcons() { UnloadTexture(halo_); }
    TargetIcons(const TargetIcons&) = delete;
    TargetIcons& operator=(const TargetIcons&) = delete;

    const SnoutIcon& forKind(TargetKind kind) const { return *icons_.at(kind); }
    const SnoutIcon& forTarget(const Target& target) const {
        return forKind(targetKind(target.iconName, target.detectedType));
    }

    // Call between beginHalos() and endHalos().
    void drawHalo(Vector2 center, float scale, float opacity) const {
        const float size = kHaloPx * scale;
        const auto level = static_cast<unsigned char>(255 * kHaloOpacity * opacity);
        DrawTexturePro(halo_, {0, 0, kHaloTextureSize, kHaloTextureSize},
                       {center.x - size / 2, center.y - size / 2, size, size}, {0, 0}, 0,
                       {level, level, level, 255});
    }

    // Overlapping halos take the brighter value instead of adding up into
    // a glaring patch around clusters of targets.
    static void beginHalos() {
        rlSetBlendFactors(RL_ONE, RL_ONE, RL_MAX);
        BeginBlendMode(BLEND_CUSTOM);
    }
    static void endHalos() { EndBlendMode(); }

private:
    std::map<TargetKind, std::unique_ptr<SnoutIcon>> icons_;
    Texture2D halo_{};
};

std::vector<RegionShape> loadShapes() {
    const fs::path geometryPath = ensureGeometryFile();
    json regions = readJsonFile(geometryPath);
    overrideRegionGeometry(regions, readJsonFile(kCrimeaOverrideFile));
    std::vector<RegionShape> shapes = buildRegionShapes(regions, kSimplifyTolerance);
    size_t triangles = 0;
    for (const auto& shape : shapes) triangles += shape.indices.size() / 3;
    std::cout << "regions=" << shapes.size() << " triangles=" << triangles << '\n';
    return shapes;
}

// Everything the renderer draws. Holds pointers into its own shapes, so it
// is neither copied nor moved.
class Scene {
public:
    Scene(std::vector<RegionShape> shapes, const Font& font)
        : shapes_(std::move(shapes)), font_(font), mapLayer_(shapes_, font_), airFilter_(shapes_) {}
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
    void prepare(int width, int height) { mapLayer_.prepare(width, height); }

    void draw(int width, int height, const Camera2D& camera, const HeatRenderer& heat,
              const TargetIcons& icons, double now) const {
        ClearBackground(kScreenClear);
        mapLayer_.draw(width, height);
        mapLayer_.drawHighlights(freshAlertPulses(), width, height);
        heat.composite();
        std::vector<TargetMotion::Marker> markers = motion_.markersAt(now);
        std::erase_if(markers, [&](const TargetMotion::Marker& marker) {
            return !visibleTracks_.contains(marker.target.trackId);
        });
        drawTargets(markers, camera, icons);
        drawSummary(markers, icons, height);
        if (status) drawStatus(*status);
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

        const float panelHeight = 2 * kPanelPaddingPx + kPanelRowPx * counts.size();
        const Rectangle panel = {kStatusMarginPx, height - kStatusMarginPx - panelHeight,
                                 kPanelWidthPx, panelHeight};
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
            DrawTextEx(font_, label, {panel.x + kPanelPaddingPx + kPanelIconPx + 10, textTop},
                       kPanelFontPx, kTextSpacingPx, kPanelText);
            const std::string number = std::to_string(count->second);
            const float numberWidth =
                MeasureTextEx(font_, number.c_str(), kPanelFontPx, kTextSpacingPx).x;
            DrawTextEx(font_, number.c_str(),
                       {panel.x + panel.width - kPanelPaddingPx - numberWidth, textTop}, kPanelFontPx,
                       kTextSpacingPx, kPanelText);
            rowTop += kPanelRowPx;
        }
    }

    void drawTargets(const std::vector<TargetMotion::Marker>& markers, const Camera2D& camera,
                     const TargetIcons& icons) const {
        const std::vector<MarkerPlacement> placements = layoutMarkers(markers, camera.zoom);
        const auto screenPosition = [&](const MarkerPlacement& placement) {
            const Vector2 position = GetWorldToScreen2D(toVector(markers[placement.index].position), camera);
            return Vector2{position.x + static_cast<float>(placement.offsetPx), position.y};
        };
        // Layer by layer, so a halo never covers a neighbouring icon.
        TargetIcons::beginHalos();
        for (const auto& placement : placements) {
            icons.drawHalo(screenPosition(placement), static_cast<float>(placement.scale),
                           static_cast<float>(markers[placement.index].opacity));
        }
        TargetIcons::endHalos();
        for (const bool shadow : {true, false}) {
            for (const auto& placement : placements) {
                const TargetMotion::Marker& marker = markers[placement.index];
                const Vector2 position = screenPosition(placement);
                const auto opacity = static_cast<float>(marker.opacity);
                const float size = kTargetIconPx * static_cast<float>(placement.scale);
                // Without a heading the icon points north.
                const auto heading = static_cast<float>(marker.target.headingDegrees.value_or(0));
                const SnoutIcon& icon = icons.forTarget(marker.target);
                if (shadow) {
                    icon.draw({position.x + kShadowOffsetPx.x, position.y + kShadowOffsetPx.y}, size,
                              heading, Fade(BLACK, kShadowOpacity * opacity));
                } else {
                    icon.draw(position, size, heading, Fade(WHITE, opacity));
                }
            }
        }
        for (const auto& placement : placements) {
            if (placement.count > 1) {
                drawCountBadge(screenPosition(placement), static_cast<float>(placement.scale),
                               placement.count, static_cast<float>(markers[placement.index].opacity));
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

    std::vector<RegionShape> shapes_;
    const Font& font_;
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

void renderFrame(Scene& scene, HeatRenderer& heat, const TargetIcons& icons, int width,
                 int height, double now, const RenderTexture2D* target) {
    const Camera2D camera = fitMapCamera(width, height);
    scene.prepare(width, height);
    // Offscreen heat passes switch render targets, so they run before the frame.
    if (heat.isStale(now, scene.dataVersion(), width, height)) {
        heat.prepare(scene.heatQuadsAt(now), camera, width, height, now, scene.dataVersion());
    }
    if (target) {
        BeginTextureMode(*target);
    } else {
        BeginDrawing();
    }
    scene.draw(width, height, camera, heat, icons, now);
    if (target) {
        EndTextureMode();
    } else {
        EndDrawing();
    }
}

void saveScreenshot(Scene& scene, HeatRenderer& heat, const TargetIcons& icons,
                    const fs::path& path) {
    const RenderTexture2D target = LoadRenderTexture(kInitialWidth, kInitialHeight);
    // Render the moment new targets have finished fading in.
    renderFrame(scene, heat, icons, kInitialWidth, kInitialHeight,
                nowSeconds() + TargetMotion::kFadeInSeconds, &target);
    Image image = LoadImageFromTexture(target.texture);
    ImageFlipVertical(&image);  // render textures are stored bottom-up
    const bool saved = ExportImage(image, path.c_str());
    UnloadImage(image);
    UnloadRenderTexture(target);
    if (!saved) throw std::runtime_error("cannot save screenshot " + path.string());
}

int run(const Options& options) {
    std::vector<RegionShape> shapes = loadShapes();
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

    unsigned flags = FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE;
    if (options.screenshot) flags |= FLAG_WINDOW_HIDDEN;
    SetConfigFlags(flags);
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(kInitialWidth, kInitialHeight, "alarmmap");
    if (!IsWindowReady()) throw std::runtime_error("cannot open window");
    SetWindowMinSize(kMinimumWidth, kMinimumHeight);
    // Triangulated polygons and mirrored icon parts come in both windings.
    rlDisableBackfaceCulling();

    {
        // GPU resources must be created after the window opens and released
        // before it closes.
        const UiFont font;
        Scene scene(std::move(shapes), font.get());
        if (!isLive(options)) scene.apply(fileTargets, fileAlerts, nowSeconds());
        HeatRenderer heat;
        const TargetIcons icons;
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
            for (int frame = 0; frame < *options.benchmarkFrames && !WindowShouldClose(); ++frame) {
                renderFrame(scene, heat, icons, GetScreenWidth(), GetScreenHeight(), nowSeconds(),
                            nullptr);
            }
            const double elapsed = nowSeconds() - started;
            std::cout << "frames=" << *options.benchmarkFrames
                      << " avg_ms=" << elapsed * 1000 / *options.benchmarkFrames << '\n';
        } else {
            // Paced here instead of with SetTargetFPS: raylib's limiter
            // busy-waits through the end of every frame.
            const auto frameDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1 / kFramesPerSecond));
            auto nextFrame = std::chrono::steady_clock::now();
            while (!WindowShouldClose()) {
                const double now = nowSeconds();
                if (isLive(options)) {
                    if (state.copyIfNewer(liveVersion, liveData)) {
                        scene.apply(liveData.targets, liveData.alerts, now);
                    }
                    // Refreshed every frame so the age of the snapshot keeps counting.
                    scene.status = describeLiveStatus(liveData, nowSeconds());
                }
                renderFrame(scene, heat, icons, GetScreenWidth(), GetScreenHeight(), now, nullptr);
                // A late frame resets the schedule instead of rushing to catch up.
                nextFrame = std::max(nextFrame + frameDuration, std::chrono::steady_clock::now());
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
    int exitCode = 1;
    try {
        exitCode = run(parseOptions(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "alarmmap: " << error.what() << '\n';
    }
    curl_global_cleanup();
    return exitCode;
}
