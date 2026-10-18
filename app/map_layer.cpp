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

// Darkens everything outside a mask; output is premultiplied by alpha.
constexpr const char* kDimShader = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;

void main() {
    float outside = 1.0 - texture(texture0, fragTexCoord).a;
    finalColor = vec4(0.0, 0.0, 0.0, 0.62 * outside);
}
)";

// Rendering at twice the window size and shrinking with bilinear filtering
// averages 2x2 samples per pixel: smooth edges without MSAA render targets.
// Phone displays are dense enough without it, and their GPUs pay four times
// the pixels on every re-render.
#ifdef __ANDROID__
constexpr float kSupersampling = 1;
// Glow masks at this share of the window resolution; glows are soft, and
// half the size takes a quarter of the pixels and fewer blur passes.
constexpr float kMaskScale = 0.5f;
#else
constexpr float kSupersampling = 2;
constexpr float kMaskScale = 1;
#endif
// The cache reaches this share of the window size past every edge, so a
// pan shows cached map instead of empty space until the re-render.
constexpr float kCacheMarginShare = 0.25f;
// A moving view is re-rendered once it rests this long, and at least this
// often while it keeps moving.
constexpr double kSettleSeconds = 0.12;
constexpr double kMaxStaleSeconds = 0.5;
// Newly arrived basemap tiles are drawn in batches at most this often.
constexpr double kTileRedrawSeconds = 0.3;
constexpr int kMaxTileZoom = 14;
// How far up the tile pyramid to look for a stand-in for a missing tile.
constexpr int kAncestorLevels = 4;
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
// A dark outline around every glyph keeps labels readable over roads,
// built-up areas and alert fills alike.
constexpr float kLabelOutlinePx = 1.3f;
constexpr int kLabelOutlineDirections = 8;
constexpr float kCapitalFontPx = 18;
constexpr float kRegionalCityFontPx = 16;
constexpr float kTownFontPx = 14.5f;
constexpr float kVillageFontPx = 13;
// Places become the main landmarks when zoomed in, so their labels grow by
// this much per doubling of the zoom, up to the maximum.
constexpr float kCityFontGrowthPx = 1;
constexpr float kMaxCityFontGrowthPx = 3;
constexpr float kCapitalDotPx = 4.5f;
constexpr float kRegionalCityDotPx = 3.2f;
constexpr float kTownDotPx = 2.3f;
constexpr float kVillageDotPx = 1.6f;
constexpr float kCityLabelGapPx = 4;
constexpr int kVignetteSize = 512;
constexpr float kVignetteClearShare = 0.35f;

constexpr Color kLandBase = {34, 38, 46, 255};
constexpr Color kAlertFill = {150, 30, 40, 105};
// Basemap palette, indexed like kAllBasemapFills and kAllBasemapLines.
constexpr Color kFillColors[] = {
    {16, 26, 40, 255},  // water
    {32, 44, 40, 255},  // wood
    {30, 40, 44, 255},  // wetland
    {44, 46, 54, 255},  // residential
    {47, 45, 53, 255},  // industrial
};
struct LineStyle {
    Color color;
    float widthPx;
    // Tile zoom from which the line is drawn.
    int minZoom;
};
constexpr LineStyle kLineStyles[] = {
    {{44, 72, 104, 255}, 1.4f, 0},    // river
    {{34, 56, 82, 255}, 0.7f, 11},    // stream
    {{164, 128, 74, 255}, 1.8f, 0},   // motorway
    {{146, 118, 76, 255}, 1.5f, 0},   // trunk
    {{118, 108, 92, 255}, 1.2f, 0},   // primary
    {{92, 92, 100, 255}, 1.0f, 8},    // secondary
    {{74, 76, 86, 255}, 0.8f, 10},    // tertiary
    {{62, 64, 74, 255}, 0.6f, 12},    // minor
    {{84, 82, 96, 255}, 0.8f, 8},     // rail
};
static_assert(std::size(kFillColors) == std::size(alarmmap::kAllBasemapFills));
static_assert(std::size(kLineStyles) == std::size(alarmmap::kAllBasemapLines));
constexpr Color kOblastBorder = {125, 135, 150, 255};
constexpr Color kRaionBorder = {58, 66, 78, 255};
constexpr Color kHromadaBorder = {44, 50, 60, 255};
constexpr Color kVignetteEdge = {0, 0, 0, 130};
constexpr Color kLabel = {176, 186, 202, 210};
constexpr Color kHighlight = {255, 70, 55, 255};
constexpr Color kLabelOutline = {8, 9, 12, 225};
constexpr Color kCityLabel = {242, 244, 250, 255};
constexpr Color kTownLabel = {222, 226, 236, 255};
constexpr Color kVillageLabel = {196, 202, 214, 255};
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

void drawTriangles(const std::vector<MapPoint>& vertices, const std::vector<std::uint32_t>& indices,
                   Color color) {
    const size_t triangleCount = indices.size() / 3;
    for (size_t first = 0; first < triangleCount; first += kTrianglesPerBlock) {
        const size_t last = std::min(triangleCount, first + kTrianglesPerBlock);
        rlCheckRenderBatchLimit(static_cast<int>((last - first) * 3));
        rlBegin(RL_TRIANGLES);
        rlColor4ub(color.r, color.g, color.b, color.a);
        for (size_t index = first * 3; index < last * 3; ++index) {
            const MapPoint& vertex = vertices[indices[index]];
            rlVertex2f(static_cast<float>(vertex.x), static_cast<float>(vertex.y));
        }
        rlEnd();
    }
}

void drawFill(const RegionShape& shape, Color color) { drawTriangles(shape.vertices, shape.indices, color); }

int tileZoomFor(const alarmmap::MapView& view) {
    return std::clamp(static_cast<int>(std::floor(alarmmap::tileZoomForScale(view.pixelsPerUnit()))), 0,
                      kMaxTileZoom);
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
                   alarmmap::TileStore& tiles, const Font& font)
    : coarse_(makeDrawable(coarse)),
      detailed_(makeDrawable(detailed)),
      coarseTolerance_(coarseTolerance),
      cities_(cities),
      tiles_(tiles),
      dimShader_(loadFragmentShader(kDimShader, "outside dimming")),
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
    UnloadShader(dimShader_);
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
    // Glows are soft anyway, so their masks use at most the window resolution.
    const int maskWidth = static_cast<int>(areaWidth * kMaskScale);
    const int maskHeight = static_cast<int>(areaHeight * kMaskScale);
    country_ = {loadLinearTarget(maskWidth, maskHeight), loadLinearTarget(maskWidth, maskHeight)};
    alerts_ = {loadLinearTarget(maskWidth, maskHeight), loadLinearTarget(maskWidth, maskHeight)};
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
    BeginTextureMode(masks.sharp);
    ClearBackground(BLANK);
    BeginMode2D(camera);
    drawFills(selection, alertsOnly, kMaskColor);
    EndMode2D();
    EndTextureMode();
    // Copying the sharp mask is far cheaper than filling the regions again.
    BeginTextureMode(masks.blurred);
    ClearBackground(BLANK);
    rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    drawTargetStretched(masks.sharp, static_cast<float>(masks.blurred.texture.width),
                        static_cast<float>(masks.blurred.texture.height));
    EndBlendMode();
    EndTextureMode();
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

void MapLayer::drawLabels(const Camera2D& camera, float scale, const alarmmap::MapView& view) const {
    const float spacing = kLabelSpacingPx * scale;
    const float outline = kLabelOutlinePx * scale;
    const auto drawText = [&](const std::string& text, Vector2 origin, float size, Color color) {
        for (int i = 0; i < kLabelOutlineDirections; ++i) {
            const float angle = 2 * PI * i / kLabelOutlineDirections;
            DrawTextEx(font_, text.c_str(),
                       {origin.x + outline * std::cos(angle), origin.y + outline * std::sin(angle)}, size,
                       spacing, kLabelOutline);
        }
        DrawTextEx(font_, text.c_str(), origin, size, spacing, color);
    };
    const float cityFontGrowth = static_cast<float>(
        std::clamp(kCityFontGrowthPx * std::log2(view.zoom()), 0.0, static_cast<double>(kMaxCityFontGrowthPx)));
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

    const double tileZoom = alarmmap::tileZoomForScale(view.pixelsPerUnit());
    const auto targetWidth = static_cast<float>(cache_.texture.width);
    const auto targetHeight = static_cast<float>(cache_.texture.height);
    std::vector<std::pair<const alarmmap::City*, Vector2>> cities;
    for (const alarmmap::City& city : cities_) {
        if (city.minZoom > tileZoom) continue;
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
            ((capital ? kCapitalFontPx : village ? kVillageFontPx : town ? kTownFontPx : kRegionalCityFontPx) +
             cityFontGrowth) *
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

void MapLayer::prepare(const alarmmap::MapView& view, double now, bool gestureActive) {
    const auto width = static_cast<int>(view.width());
    const auto height = static_cast<int>(view.height());
    if (width <= 0 || height <= 0) return;
    if (width != width_ || height != height_) resize(width, height);
    if (!(view == pendingView_)) {
        pendingView_ = view;
        pendingSince_ = now;
    }
    // Ask for the tiles of the current view right away, so they download
    // while the view is still settling.
    const double halfWidth = view.width() * (0.5 + kCacheMarginShare) / view.pixelsPerUnit();
    const double halfHeight = view.height() * (0.5 + kCacheMarginShare) / view.pixelsPerUnit();
    const MapPoint center = view.center();
    std::vector<alarmmap::TileId> wanted =
        alarmmap::tilesCovering({center.x - halfWidth, center.y - halfHeight},
                                {center.x + halfWidth, center.y + halfHeight}, tileZoomFor(view));
    if (wanted != wantedTiles_) {
        tiles_.want(wanted);
        wantedTiles_ = std::move(wanted);
    }
    if (tiles_.version() != renderedTilesVersion_ && now - renderedAt_ >= kTileRedrawSeconds) dirty_ = true;
    if (!(view == renderedView_) &&
        (now - pendingSince_ >= kSettleSeconds || now - renderedAt_ >= kMaxStaleSeconds)) {
        dirty_ = true;
    }
    if (!dirty_ || (gestureActive && rendered_)) return;
    render(view);
    rendered_ = true;
    renderedView_ = view;
    renderedAt_ = now;
    dirty_ = false;
}

void MapLayer::render(const alarmmap::MapView& view) {
    const auto maskWidth = static_cast<float>(country_.sharp.texture.width);
    const auto maskHeight = static_cast<float>(country_.sharp.texture.height);
    const Camera2D maskCamera = viewCamera(view, maskWidth, maskHeight, kMaskScale);
    const Selection selection = select(view, maskWidth / kMaskScale / 2, maskHeight / kMaskScale / 2);
    renderMask(country_, selection, maskCamera, false, kCountryHaloRadiusPx * uiScale() * kMaskScale);
    const bool hasAlerts = !alertRegionIds_.empty();
    if (hasAlerts) {
        renderMask(alerts_, selection, maskCamera, true, kAlertGlowRadiusPx * uiScale() * kMaskScale);
    }

    const Camera2D camera = viewCamera(view, static_cast<float>(cache_.texture.width),
                                       static_cast<float>(cache_.texture.height), kSupersampling);
    renderedTilesVersion_ = tiles_.version();
    const auto basemap = tilesToDraw();
    BeginTextureMode(cache_);
    ClearBackground(kLandBase);
    BeginMode2D(camera);
    drawBasemap(basemap, camera, kSupersampling, view.zoom());
    EndMode2D();
    drawOutsideDimming();
    drawGlow(country_, kNoGlow, kCountryHalo);
    BeginMode2D(camera);
    drawFills(selection, true, kAlertFill);
    EndMode2D();
    if (hasAlerts) drawGlow(alerts_, kAlertInnerGlow, kAlertOuterGlow);
    BeginMode2D(camera);
    drawBorders(selection, camera, kSupersampling * uiScale());
    EndMode2D();
    drawLabels(camera, kSupersampling * uiScale(), view);
    EndTextureMode();
}

std::vector<std::shared_ptr<const alarmmap::BasemapTile>> MapLayer::tilesToDraw() const {
    std::vector<std::shared_ptr<const alarmmap::BasemapTile>> result;
    std::set<alarmmap::TileId> chosen;
    for (const alarmmap::TileId& wanted : wantedTiles_) {
        alarmmap::TileId id = wanted;
        for (int level = 0; level <= kAncestorLevels && id.z >= 0; ++level) {
            if (auto tile = tiles_.get(id)) {
                if (chosen.insert(id).second) result.push_back(std::move(tile));
                break;
            }
            id = {id.z - 1, id.x / 2, id.y / 2};
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a->id.z < b->id.z; });
    return result;
}

void MapLayer::drawBasemap(const std::vector<std::shared_ptr<const alarmmap::BasemapTile>>& tiles,
                           const Camera2D& camera, float scale, double viewZoom) const {
    for (size_t fill = 0; fill < std::size(kFillColors); ++fill) {
        for (const auto& tile : tiles) {
            const alarmmap::FillMesh& mesh = tile->fills[fill];
            drawTriangles(mesh.vertices, mesh.indices, kFillColors[fill]);
        }
    }
    // Lines widen a little as the view zooms in.
    const float widthScale = static_cast<float>(std::clamp(1 + 0.25 * std::log2(viewZoom), 1.0, 2.2));
    std::vector<Vector2> points;
    for (size_t line = 0; line < std::size(kLineStyles); ++line) {
        const LineStyle& style = kLineStyles[line];
        for (const auto& tile : tiles) {
            if (tile->id.z < style.minZoom) continue;
            for (const auto& polyline : tile->lines[line]) {
                points.clear();
                for (const MapPoint& point : polyline) points.push_back(toVector(point));
                DrawSplineLinear(points.data(), static_cast<int>(points.size()),
                                 style.widthPx * widthScale * scale / camera.zoom, style.color);
            }
        }
    }
}

void MapLayer::drawOutsideDimming() const {
    BeginShaderMode(dimShader_);
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    drawTargetStretched(country_.sharp, static_cast<float>(cache_.texture.width),
                        static_cast<float>(cache_.texture.height));
    EndBlendMode();
    EndShaderMode();
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
