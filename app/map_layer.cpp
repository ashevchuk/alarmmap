#include "map_layer.hpp"

#include "projection.hpp"
#include "render_util.hpp"

#include <rlgl.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <tuple>
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
// The cache reaches this share of the window size past every edge, so a
// pan shows cached map instead of empty space until the re-render.
constexpr float kCacheMarginShare = 0.25f;
// A moving view is re-rendered once it rests this long, and at least this
// often while it keeps moving.
constexpr double kSettleSeconds = 0.12;
constexpr double kMaxStaleSeconds = 0.5;
constexpr float kOblastBorderPx = 1.5f;
constexpr float kRaionBorderPx = 0.8f;
constexpr float kHromadaBorderPx = 0.6f;
// Hromadas are too small to outline on the whole-country view.
constexpr double kHromadaBordersMinZoom = 4;
// Largest simplification error, in screen pixels, the coarse shapes may show.
constexpr double kMaxSimplificationPx = 0.6;
constexpr float kCountryHaloRadiusPx = 26;
constexpr float kAlertGlowRadiusPx = 10;
// Triangles per rlBegin/rlEnd block; keeps every block inside one render batch.
constexpr int kTrianglesPerBlock = 1024;
constexpr float kLabelFontPx = 13;
constexpr float kLabelSpacingPx = 0.4f;
constexpr Vector2 kLabelShadowOffsetPx = {0.8f, 1};
constexpr float kCapitalFontPx = 15;
constexpr float kRegionalCityFontPx = 13;
constexpr float kTownFontPx = 12;
constexpr float kVillageFontPx = 11;
constexpr float kCapitalDotPx = 4.5f;
constexpr float kRegionalCityDotPx = 3.2f;
constexpr float kTownDotPx = 2.3f;
constexpr float kVillageDotPx = 1.6f;
constexpr float kCityLabelGapPx = 4;
constexpr int kVignetteSize = 512;
constexpr float kVignetteClearShare = 0.35f;

constexpr Color kBackground = {14, 17, 22, 255};
constexpr Color kLand = {32, 37, 46, 255};
constexpr Color kAlertFill = {84, 22, 30, 255};
constexpr Color kOblastBorder = {125, 135, 150, 255};
constexpr Color kRaionBorder = {58, 66, 78, 255};
constexpr Color kHromadaBorder = {44, 50, 60, 255};
constexpr Color kVignetteEdge = {0, 0, 0, 130};
constexpr Color kLabel = {176, 186, 202, 210};
constexpr Color kHighlight = {255, 70, 55, 255};
constexpr Color kLabelShadow = {0, 0, 0, 170};
constexpr Color kCityLabel = {232, 236, 244, 255};
constexpr Color kTownLabel = {196, 202, 214, 240};
constexpr Color kVillageLabel = {160, 168, 182, 230};
constexpr Color kCityDot = {236, 240, 248, 255};
constexpr Color kCityDotOutline = {10, 12, 16, 255};
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

Camera2D viewCamera(const alarmmap::MapView& view, float targetWidth, float targetHeight, float scale) {
    Camera2D camera{};
    camera.offset = {targetWidth / 2, targetHeight / 2};
    camera.target = toVector(view.center());
    camera.zoom = static_cast<float>(view.pixelsPerUnit()) * scale;
    return camera;
}

Camera2D viewCamera(const alarmmap::MapView& view) {
    return viewCamera(view, static_cast<float>(view.width()), static_cast<float>(view.height()));
}

std::vector<MapLayer::DrawableRegion> MapLayer::makeDrawable(const std::vector<RegionShape>& shapes) {
    std::vector<DrawableRegion> regions;
    for (const RegionShape& shape : shapes) {
        DrawableRegion region{&shape, {}};
        for (const auto& ring : shape.outlines) {
            auto& points = region.closedOutlines.emplace_back();
            for (const MapPoint& point : ring) points.push_back(toVector(point));
            points.push_back(points.front());
        }
        regions.push_back(std::move(region));
    }
    return regions;
}

bool MapLayer::Selection::covers(const DrawableRegion& region) const {
    const RegionShape& shape = *region.shape;
    return shape.boundsMax.x >= min.x && shape.boundsMin.x <= max.x && shape.boundsMax.y >= min.y &&
           shape.boundsMin.y <= max.y;
}

MapLayer::MapLayer(const std::vector<RegionShape>& coarse, double coarseTolerance,
                   const std::vector<RegionShape>& detailed, const std::vector<alarmmap::City>& cities,
                   const Font& font)
    : coarse_(makeDrawable(coarse)),
      detailed_(makeDrawable(detailed)),
      coarseTolerance_(coarseTolerance),
      cities_(cities),
      font_(font),
      glowShader_(loadFragmentShader(kGlowShader, "edge glow")),
      sharpMaskLocation_(GetShaderLocation(glowShader_, "sharpMask")),
      innerGlowLocation_(GetShaderLocation(glowShader_, "innerGlow")),
      outerGlowLocation_(GetShaderLocation(glowShader_, "outerGlow")),
      vignette_(makeVignette()) {
    for (const RegionShape& shape : coarse) {
        // The capital is labelled as a city instead of as a region.
        if (shape.name == "м. Київ") continue;
        if (shape.rank == kOblastRank && !shape.outlines.empty()) {
            labels_.push_back({shortRegionName(shape.name), toVector(alarmmap::labelPoint(shape))});
        }
    }
}

MapLayer::Selection MapLayer::select(const alarmmap::MapView& view, double halfWidthPx,
                                     double halfHeightPx) const {
    const double scale = view.pixelsPerUnit();
    const bool coarseIsFineEnough = coarseTolerance_ * scale <= kMaxSimplificationPx;
    const MapPoint center = view.center();
    return {coarseIsFineEnough ? &coarse_ : &detailed_,
            {center.x - halfWidthPx / scale, center.y - halfHeightPx / scale},
            {center.x + halfWidthPx / scale, center.y + halfHeightPx / scale},
            view.zoom() >= kHromadaBordersMinZoom};
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
    const int areaWidth = static_cast<int>(width * (1 + 2 * kCacheMarginShare));
    const int areaHeight = static_cast<int>(height * (1 + 2 * kCacheMarginShare));
    cache_ = loadLinearTarget(static_cast<int>(areaWidth * kSupersampling),
                              static_cast<int>(areaHeight * kSupersampling));
    // Glows are soft anyway, so their masks use the window resolution.
    country_ = {loadLinearTarget(areaWidth, areaHeight), loadLinearTarget(areaWidth, areaHeight)};
    alerts_ = {loadLinearTarget(areaWidth, areaHeight), loadLinearTarget(areaWidth, areaHeight)};
    width_ = width;
    height_ = height;
    dirty_ = true;
}

void MapLayer::drawFills(const Selection& selection, bool alertsOnly, Color color) const {
    // Coarser regions first, so a district alert stays visible over its oblast.
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (const DrawableRegion& region : *selection.regions) {
            if (region.shape->rank != rank || !selection.covers(region)) continue;
            if (alertsOnly ? alertRegionIds_.contains(region.shape->id) : rank == kOblastRank) {
                drawFill(*region.shape, color);
            }
        }
    }
}

void MapLayer::renderMask(const GlowMasks& masks, const Selection& selection, const Camera2D& camera,
                          bool alertsOnly, float blurRadius) {
    for (const RenderTexture2D* target : {&masks.sharp, &masks.blurred}) {
        BeginTextureMode(*target);
        ClearBackground(BLANK);
        BeginMode2D(camera);
        drawFills(selection, alertsOnly, kMaskColor);
        EndMode2D();
        EndTextureMode();
    }
    blur_.apply(masks.blurred, blurRadius);
}

void MapLayer::drawBorders(const Selection& selection, const Camera2D& camera, float scale) const {
    const auto drawRank = [&](int rank, float widthPx, Color color) {
        for (const DrawableRegion& region : *selection.regions) {
            if (region.shape->rank != rank || !selection.covers(region)) continue;
            for (const auto& outline : region.closedOutlines) {
                DrawSplineLinear(outline.data(), static_cast<int>(outline.size()),
                                 widthPx * scale / camera.zoom, color);
            }
        }
    };
    if (selection.showHromadaBorders) drawRank(kHromadaRank, kHromadaBorderPx, kHromadaBorder);
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

void MapLayer::drawLabels(const Camera2D& camera, float scale, double viewZoom) const {
    const float spacing = kLabelSpacingPx * scale;
    const Vector2 shadowOffset = {kLabelShadowOffsetPx.x * scale, kLabelShadowOffsetPx.y * scale};
    const auto drawText = [&](const std::string& text, Vector2 origin, float size, Color color) {
        DrawTextEx(font_, text.c_str(), {origin.x + shadowOffset.x, origin.y + shadowOffset.y}, size,
                   spacing, kLabelShadow);
        DrawTextEx(font_, text.c_str(), origin, size, spacing, color);
    };
    std::vector<Rectangle> placed;
    const auto isFree = [&](const Rectangle& rectangle) {
        return std::none_of(placed.begin(), placed.end(), [&](const Rectangle& other) {
            return CheckCollisionRecs(rectangle, other);
        });
    };

    const auto placeOblastLabels = [&] {
        const float size = kLabelFontPx * scale;
        for (const Label& label : labels_) {
            const Vector2 anchor = GetWorldToScreen2D(label.position, camera);
            const Vector2 extent = MeasureTextEx(font_, label.text.c_str(), size, spacing);
            const Rectangle area = {anchor.x - extent.x / 2, anchor.y - extent.y / 2, extent.x, extent.y};
            if (!isFree(area)) continue;
            drawText(label.text, {area.x, area.y}, size, kLabel);
            placed.push_back(area);
        }
    };

    const double webZoom = alarmmap::webZoomForView(viewZoom);
    const auto targetWidth = static_cast<float>(cache_.texture.width);
    const auto targetHeight = static_cast<float>(cache_.texture.height);
    std::vector<std::pair<const alarmmap::City*, Vector2>> cities;
    for (const alarmmap::City& city : cities_) {
        if (city.minZoom > webZoom) continue;
        const Vector2 anchor = GetWorldToScreen2D(toVector(city.position), camera);
        if (anchor.x < 0 || anchor.y < 0 || anchor.x > targetWidth || anchor.y > targetHeight) continue;
        cities.emplace_back(&city, anchor);
    }
    // Capital first, then by how early a place appears, then by size.
    std::stable_sort(cities.begin(), cities.end(), [](const auto& left, const auto& right) {
        const auto priority = [](const alarmmap::City* city) {
            return std::tuple(city->kind != alarmmap::CityKind::Capital, city->minZoom, -city->population);
        };
        return priority(left.first) < priority(right.first);
    });

    // The capital and oblast centres outrank oblast names, which outrank
    // every other place.
    bool oblastLabelsPlaced = false;
    for (const auto& [city, anchor] : cities) {
        const bool major = city->kind == alarmmap::CityKind::Capital || city->kind == alarmmap::CityKind::Regional;
        if (!major && !oblastLabelsPlaced) {
            placeOblastLabels();
            oblastLabelsPlaced = true;
        }
        const bool capital = city->kind == alarmmap::CityKind::Capital;
        const bool town = city->kind == alarmmap::CityKind::Town;
        const bool village = city->kind == alarmmap::CityKind::Village;
        const float dot =
            (capital ? kCapitalDotPx : village ? kVillageDotPx : town ? kTownDotPx : kRegionalCityDotPx) * scale;
        const float size =
            (capital ? kCapitalFontPx : village ? kVillageFontPx : town ? kTownFontPx : kRegionalCityFontPx) *
            scale;
        const Vector2 extent = MeasureTextEx(font_, city->name.c_str(), size, spacing);
        const Rectangle dotArea = {anchor.x - dot, anchor.y - dot, 2 * dot, 2 * dot};
        const float gap = dot + kCityLabelGapPx * scale;
        const Rectangle right = {anchor.x + gap, anchor.y - extent.y / 2, extent.x, extent.y};
        const Rectangle left = {anchor.x - gap - extent.x, anchor.y - extent.y / 2, extent.x, extent.y};
        if (!isFree(dotArea)) continue;
        const Rectangle* label = isFree(right) ? &right : isFree(left) ? &left : nullptr;
        if (!label) continue;

        DrawCircleV(anchor, dot + scale, kCityDotOutline);
        DrawCircleV(anchor, dot, kCityDot);
        if (capital) {
            DrawCircleV(anchor, dot * 0.55f, kCityDotOutline);
            DrawCircleV(anchor, dot * 0.3f, kCityDot);
        }
        drawText(city->name, {label->x, label->y}, size,
                 village ? kVillageLabel : town ? kTownLabel : kCityLabel);
        placed.push_back(dotArea);
        placed.push_back(*label);
    }
    if (!oblastLabelsPlaced) placeOblastLabels();
}

void MapLayer::prepare(const alarmmap::MapView& view, double now) {
    const auto width = static_cast<int>(view.width());
    const auto height = static_cast<int>(view.height());
    if (width <= 0 || height <= 0) return;
    if (width != width_ || height != height_) resize(width, height);
    if (!(view == pendingView_)) {
        pendingView_ = view;
        pendingSince_ = now;
    }
    if (!(view == renderedView_) &&
        (now - pendingSince_ >= kSettleSeconds || now - renderedAt_ >= kMaxStaleSeconds)) {
        dirty_ = true;
    }
    if (!dirty_) return;
    render(view);
    renderedView_ = view;
    renderedAt_ = now;
    dirty_ = false;
}

void MapLayer::render(const alarmmap::MapView& view) {
    const auto maskWidth = static_cast<float>(country_.sharp.texture.width);
    const auto maskHeight = static_cast<float>(country_.sharp.texture.height);
    const Camera2D maskCamera = viewCamera(view, maskWidth, maskHeight);
    const Selection selection = select(view, maskWidth / 2, maskHeight / 2);
    renderMask(country_, selection, maskCamera, false, kCountryHaloRadiusPx);
    const bool hasAlerts = !alertRegionIds_.empty();
    if (hasAlerts) renderMask(alerts_, selection, maskCamera, true, kAlertGlowRadiusPx);

    const Camera2D camera = viewCamera(view, static_cast<float>(cache_.texture.width),
                                       static_cast<float>(cache_.texture.height), kSupersampling);
    BeginTextureMode(cache_);
    ClearBackground(kBackground);
    drawGlow(country_, kNoGlow, kCountryHalo);
    BeginMode2D(camera);
    drawFills(selection, false, kLand);
    drawFills(selection, true, kAlertFill);
    EndMode2D();
    if (hasAlerts) drawGlow(alerts_, kAlertInnerGlow, kAlertOuterGlow);
    BeginMode2D(camera);
    drawBorders(selection, camera, kSupersampling);
    EndMode2D();
    drawLabels(camera, kSupersampling, view.zoom());
    EndTextureMode();
}

void MapLayer::drawVignette(float width, float height) const {
    DrawTexturePro(vignette_, {0, 0, kVignetteSize, kVignetteSize}, {0, 0, width, height}, {0, 0}, 0,
                   WHITE);
}

void MapLayer::drawHighlights(const std::map<std::string, float>& opacityByRegion,
                              const alarmmap::MapView& view) const {
    if (opacityByRegion.empty()) return;
    const Selection selection = select(view, view.width() / 2, view.height() / 2);
    BeginMode2D(viewCamera(view));
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (const DrawableRegion& region : *selection.regions) {
            if (region.shape->rank != rank || !selection.covers(region)) continue;
            const auto opacity = opacityByRegion.find(region.shape->id);
            if (opacity != opacityByRegion.end()) drawFill(*region.shape, Fade(kHighlight, opacity->second));
        }
    }
    EndMode2D();
}

void MapLayer::draw(const alarmmap::MapView& view) const {
    if (width_ == 0) return;
    // The cache covers a map area around the view it was rendered for;
    // place that area where it falls in the current view.
    const double halfWidth = cache_.texture.width / kSupersampling / 2 / renderedView_.pixelsPerUnit();
    const double halfHeight = cache_.texture.height / kSupersampling / 2 / renderedView_.pixelsPerUnit();
    const MapPoint center = renderedView_.center();
    const MapPoint topLeft = view.mapToScreen({center.x - halfWidth, center.y - halfHeight});
    const MapPoint bottomRight = view.mapToScreen({center.x + halfWidth, center.y + halfHeight});
    DrawTexturePro(cache_.texture, flippedSource(cache_.texture),
                   {static_cast<float>(topLeft.x), static_cast<float>(topLeft.y),
                    static_cast<float>(bottomRight.x - topLeft.x),
                    static_cast<float>(bottomRight.y - topLeft.y)},
                   {0, 0}, 0, WHITE);
}
