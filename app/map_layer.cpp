#include "map_layer.hpp"

#include "projection.hpp"
#include "render_util.hpp"

#include <rlgl.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

using alarmmap::kHromadaRank;
using alarmmap::kMapHeight;
using alarmmap::kMapWidth;
using alarmmap::kOblastRank;
using alarmmap::kRaionRank;
using alarmmap::MapPoint;
using alarmmap::RegionShape;

namespace {

// Glow along the edges of a mask. Next to an edge the blurred mask is about
// 0.5, deep inside 1 and far outside 0, so the inner glow lights only the
// rim of a region and shared borders inside the mask stay dark. Colours are
// rgb + strength; output is premultiplied by alpha.
constexpr const char* kGlowShader = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform sampler2D sharpMask;
uniform vec4 innerGlow;
uniform vec4 outerGlow;
out vec4 finalColor;

void main() {
    float blurred = texture(texture0, fragTexCoord).a;
    float mask = texture(sharpMask, fragTexCoord).a;
    float inner = mask * clamp(2.0 * (1.0 - blurred), 0.0, 1.0) * innerGlow.a;
    float outer = (1.0 - mask) * clamp(2.0 * blurred, 0.0, 1.0) * outerGlow.a;
    float alpha = inner + outer;
    if (alpha < 0.002) discard;
    finalColor = vec4(innerGlow.rgb * inner + outerGlow.rgb * outer, alpha);
}
)";

// Rendering at twice the window size and shrinking with bilinear filtering
// averages 2x2 samples per pixel: smooth edges without MSAA render targets.
constexpr float kSupersampling = 2;
constexpr float kMarginPx = 24;
constexpr float kOblastBorderPx = 1.5f;
constexpr float kRaionBorderPx = 0.8f;
constexpr float kCountryHaloRadiusPx = 26;
constexpr float kAlertGlowRadiusPx = 10;
// Triangles per rlBegin/rlEnd block; keeps every block inside one render batch.
constexpr int kTrianglesPerBlock = 1024;
constexpr float kLabelFontPx = 13;
constexpr float kLabelSpacingPx = 0.4f;
constexpr Vector2 kLabelShadowOffsetPx = {0.8f, 1};
constexpr int kVignetteSize = 512;
constexpr float kVignetteClearShare = 0.35f;

constexpr Color kBackground = {14, 17, 22, 255};
constexpr Color kLand = {32, 37, 46, 255};
constexpr Color kAlertFill = {84, 22, 30, 255};
constexpr Color kOblastBorder = {125, 135, 150, 255};
constexpr Color kRaionBorder = {58, 66, 78, 255};
constexpr Color kVignetteEdge = {0, 0, 0, 130};
constexpr Color kLabel = {176, 186, 202, 210};
constexpr Color kHighlight = {255, 70, 55, 255};
constexpr Color kLabelShadow = {0, 0, 0, 170};
constexpr Color kMaskColor = WHITE;
constexpr Vector4 kNoGlow = {0, 0, 0, 0};
constexpr Vector4 kCountryHalo = {80 / 255.0f, 120 / 255.0f, 190 / 255.0f, 0.45f};
constexpr Vector4 kAlertInnerGlow = {1.0f, 80 / 255.0f, 70 / 255.0f, 0.9f};
constexpr Vector4 kAlertOuterGlow = {1.0f, 60 / 255.0f, 50 / 255.0f, 0.4f};

Vector2 toVector(const MapPoint& point) {
    return {static_cast<float>(point.x), static_cast<float>(point.y)};
}

void drawFill(const RegionShape& shape, Color color) {
    const size_t triangleCount = shape.indices.size() / 3;
    for (size_t first = 0; first < triangleCount; first += kTrianglesPerBlock) {
        const size_t last = std::min(triangleCount, first + kTrianglesPerBlock);
        rlCheckRenderBatchLimit(static_cast<int>((last - first) * 3));
        rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);
        for (size_t index = first * 3; index < last * 3; ++index) {
            const MapPoint& vertex = shape.vertices[shape.indices[index]];
            rlVertex2f(static_cast<float>(vertex.x), static_cast<float>(vertex.y));
        }
        rlEnd();
    }
}

std::string shortRegionName(const std::string& name) {
    constexpr std::pair<std::string_view, std::string_view> kSpecialNames[] = {
        {"м. Київ", "Київ"},
        {"Автономна Республіка Крим", "Крим"},
    };
    for (const auto& [full, shortName] : kSpecialNames) {
        if (name == full) return std::string(shortName);
    }
    constexpr std::string_view kOblastSuffix = " область";
    if (name.ends_with(kOblastSuffix)) return name.substr(0, name.size() - kOblastSuffix.size());
    return name;
}

Texture2D makeVignette() {
    Image image = GenImageGradientRadial(kVignetteSize, kVignetteSize, kVignetteClearShare, BLANK,
                                         kVignetteEdge);
    const Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image);
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    return texture;
}

}  // namespace

Camera2D fitMapCamera(int width, int height, float scale) {
    const float margin = kMarginPx * scale;
    const float zoom = std::max(0.01f, std::min((width - 2 * margin) / static_cast<float>(kMapWidth),
                                                (height - 2 * margin) / static_cast<float>(kMapHeight)));
    Camera2D camera{};
    camera.offset = {width / 2.0f, height / 2.0f};
    camera.target = {static_cast<float>(kMapWidth / 2), static_cast<float>(kMapHeight / 2)};
    camera.zoom = zoom;
    return camera;
}

MapLayer::MapLayer(const std::vector<RegionShape>& shapes, const Font& font)
    : font_(font),
      glowShader_(loadFragmentShader(kGlowShader, "edge glow")),
      sharpMaskLocation_(GetShaderLocation(glowShader_, "sharpMask")),
      innerGlowLocation_(GetShaderLocation(glowShader_, "innerGlow")),
      outerGlowLocation_(GetShaderLocation(glowShader_, "outerGlow")),
      vignette_(makeVignette()) {
    for (const RegionShape& shape : shapes) {
        DrawableRegion region{&shape, {}};
        for (const auto& ring : shape.outlines) {
            auto& points = region.closedOutlines.emplace_back();
            for (const MapPoint& point : ring) points.push_back(toVector(point));
            points.push_back(points.front());
        }
        regions_.push_back(std::move(region));
        if (shape.rank == kOblastRank && !shape.outlines.empty()) {
            labels_.push_back({shortRegionName(shape.name), toVector(alarmmap::labelPoint(shape))});
        }
    }
}

MapLayer::~MapLayer() {
    if (width_ > 0) {
        for (const RenderTexture2D& target :
             {cache_, country_.sharp, country_.blurred, alerts_.sharp, alerts_.blurred}) {
            UnloadRenderTexture(target);
        }
    }
    UnloadTexture(vignette_);
    UnloadShader(glowShader_);
}

void MapLayer::setAlertRegions(const std::set<std::string>* regionIds) {
    std::set<std::string> next = regionIds ? *regionIds : std::set<std::string>{};
    if (next == alertRegionIds_) return;
    alertRegionIds_ = std::move(next);
    dirty_ = true;
}

void MapLayer::resize(int width, int height) {
    if (width_ > 0) {
        for (const RenderTexture2D& target :
             {cache_, country_.sharp, country_.blurred, alerts_.sharp, alerts_.blurred}) {
            UnloadRenderTexture(target);
        }
    }
    cache_ = loadLinearTarget(static_cast<int>(width * kSupersampling),
                              static_cast<int>(height * kSupersampling));
    // Glows are soft anyway, so their masks use the window resolution.
    country_ = {loadLinearTarget(width, height), loadLinearTarget(width, height)};
    alerts_ = {loadLinearTarget(width, height), loadLinearTarget(width, height)};
    width_ = width;
    height_ = height;
    dirty_ = true;
}

void MapLayer::drawFills(bool alertsOnly, Color color) const {
    // Coarser regions first, so a district alert stays visible over its oblast.
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (const DrawableRegion& region : regions_) {
            if (region.shape->rank != rank) continue;
            if (alertsOnly ? alertRegionIds_.contains(region.shape->id) : rank == kOblastRank) {
                drawFill(*region.shape, color);
            }
        }
    }
}

void MapLayer::renderMask(const GlowMasks& masks, bool alertsOnly, float blurRadius) {
    const Camera2D camera = fitMapCamera(width_, height_);
    for (const RenderTexture2D* target : {&masks.sharp, &masks.blurred}) {
        BeginTextureMode(*target);
        ClearBackground(BLANK);
        BeginMode2D(camera);
        drawFills(alertsOnly, kMaskColor);
        EndMode2D();
        EndTextureMode();
    }
    blur_.apply(masks.blurred, blurRadius);
}

void MapLayer::drawBorders(const Camera2D& camera, float scale) const {
    const auto drawRank = [&](int rank, float widthPx, Color color) {
        for (const DrawableRegion& region : regions_) {
            if (region.shape->rank != rank) continue;
            for (const auto& outline : region.closedOutlines) {
                DrawSplineLinear(outline.data(), static_cast<int>(outline.size()),
                                 widthPx * scale / camera.zoom, color);
            }
        }
    };
    drawRank(kRaionRank, kRaionBorderPx, kRaionBorder);
    drawRank(kOblastRank, kOblastBorderPx, kOblastBorder);
}

void MapLayer::drawGlow(const GlowMasks& masks, Vector4 inner, Vector4 outer) const {
    BeginShaderMode(glowShader_);
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    // After BeginBlendMode: its batch flush also drops extra texture bindings.
    SetShaderValueTexture(glowShader_, sharpMaskLocation_, masks.sharp.texture);
    SetShaderValue(glowShader_, innerGlowLocation_, &inner, SHADER_UNIFORM_VEC4);
    SetShaderValue(glowShader_, outerGlowLocation_, &outer, SHADER_UNIFORM_VEC4);
    drawTargetStretched(masks.blurred, static_cast<float>(cache_.texture.width),
                        static_cast<float>(cache_.texture.height));
    EndBlendMode();
    EndShaderMode();
}

void MapLayer::drawLabels(const Camera2D& camera, float scale) const {
    const float size = kLabelFontPx * scale;
    const float spacing = kLabelSpacingPx * scale;
    for (const Label& label : labels_) {
        const Vector2 anchor = GetWorldToScreen2D(label.position, camera);
        const Vector2 extent = MeasureTextEx(font_, label.text.c_str(), size, spacing);
        const Vector2 origin = {anchor.x - extent.x / 2, anchor.y - extent.y / 2};
        DrawTextEx(font_, label.text.c_str(),
                   {origin.x + kLabelShadowOffsetPx.x * scale, origin.y + kLabelShadowOffsetPx.y * scale},
                   size, spacing, kLabelShadow);
        DrawTextEx(font_, label.text.c_str(), origin, size, spacing, kLabel);
    }
}

void MapLayer::prepare(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width != width_ || height != height_) resize(width, height);
    if (!dirty_) return;

    renderMask(country_, false, kCountryHaloRadiusPx);
    const bool hasAlerts = !alertRegionIds_.empty();
    if (hasAlerts) renderMask(alerts_, true, kAlertGlowRadiusPx);

    const Camera2D camera =
        fitMapCamera(cache_.texture.width, cache_.texture.height, kSupersampling);
    BeginTextureMode(cache_);
    ClearBackground(kBackground);
    drawGlow(country_, kNoGlow, kCountryHalo);
    BeginMode2D(camera);
    drawFills(false, kLand);
    drawFills(true, kAlertFill);
    EndMode2D();
    if (hasAlerts) drawGlow(alerts_, kAlertInnerGlow, kAlertOuterGlow);
    BeginMode2D(camera);
    drawBorders(camera, kSupersampling);
    EndMode2D();
    drawLabels(camera, kSupersampling);
    DrawTexturePro(vignette_, {0, 0, kVignetteSize, kVignetteSize},
                   {0, 0, static_cast<float>(cache_.texture.width),
                    static_cast<float>(cache_.texture.height)},
                   {0, 0}, 0, WHITE);
    EndTextureMode();
    dirty_ = false;
}

void MapLayer::drawHighlights(const std::map<std::string, float>& opacityByRegion, int width,
                              int height) const {
    if (opacityByRegion.empty()) return;
    BeginMode2D(fitMapCamera(width, height));
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (const DrawableRegion& region : regions_) {
            if (region.shape->rank != rank) continue;
            const auto opacity = opacityByRegion.find(region.shape->id);
            if (opacity != opacityByRegion.end()) drawFill(*region.shape, Fade(kHighlight, opacity->second));
        }
    }
    EndMode2D();
}

void MapLayer::draw(int width, int height) const {
    if (width_ == 0) return;
    drawTargetStretched(cache_, static_cast<float>(width), static_cast<float>(height));
}
