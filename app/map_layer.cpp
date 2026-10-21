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
// During a gesture the cache is stretched up to this factor either way
// before it is re-rendered; beyond it the map turns visibly blurry.
constexpr double kMaxCacheMagnification = 2;
// Newly arrived basemap tiles are drawn in batches at most this often.
constexpr double kTileRedrawSeconds = 0.3;
constexpr int kMaxTileZoom = 14;
// Uploaded tile geometry kept beyond the tiles on screen, and how many cache
// renders an unused tile's geometry survives once that many are kept.
constexpr size_t kMaxTileGeometries = 256;
constexpr std::uint64_t kTileGeometryKeepRenders = 8;
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
constexpr float kAlertGlowRadiusPx = 7;
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
// Street names appear from this tile zoom; before it they would only clutter.
constexpr double kStreetNamesMinTileZoom = 12;
constexpr float kStreetFontPx = 13;
constexpr float kStreetSpacingPx = 0.8f;
constexpr float kStreetOutlinePx = kLabelOutlinePx;
constexpr float kStreetPaddingPx = 2;
constexpr float kStreetMinGlyphShare = 0.3f;
// A name must be this much shorter than the stretch of road it sits on, and
// may turn this much between neighbouring letters.
constexpr float kStreetFitMargin = 1.15f;
constexpr float kStreetMaxTurnRadians = 0.5f;
// Positions tried along the road, as shares of its spare length.
constexpr float kStreetOffsetShares[] = {0.5f, 0.25f, 0.75f, 0.1f, 0.9f};
constexpr int kVignetteSize = 512;
constexpr float kVignetteClearShare = 0.35f;

constexpr Color kLandBase = {34, 38, 46, 255};
constexpr Color kAlertFill = {150, 30, 40, 105};
constexpr Color kYellowAlertFill = {190, 140, 10, 70};
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
constexpr Color kStreetLabel = {226, 231, 242, 255};
constexpr Color kCityDot = {236, 240, 248, 255};
constexpr Color kCityDotOutline = {10, 12, 16, 255};
constexpr Color kMaskColor = WHITE;
constexpr Vector4 kNoGlow = {0, 0, 0, 0};
constexpr Vector4 kCountryHalo = {80 / 255.0f, 120 / 255.0f, 190 / 255.0f, 0.45f};
constexpr Vector4 kAlertInnerGlow = {1.0f, 80 / 255.0f, 70 / 255.0f, 0.9f};
constexpr Vector4 kAlertOuterGlow = {1.0f, 60 / 255.0f, 50 / 255.0f, 0.4f};
constexpr Vector4 kYellowInnerGlow = {1.0f, 200 / 255.0f, 40 / 255.0f, 0.55f};
constexpr Vector4 kYellowOuterGlow = {1.0f, 180 / 255.0f, 30 / 255.0f, 0.22f};

Vector2 toVector(const MapPoint& point) {
    return {static_cast<float>(point.x), static_cast<float>(point.y)};
}

// Web map zoom of the view in density-independent pixels, as web maps
// count it: a dense phone screen gets the detail a desktop shows at the same
// physical size, not three times more.
double tileZoomOf(const alarmmap::MapView& view) {
    return alarmmap::tileZoomForScale(view.pixelsPerUnit() / uiScale());
}

int tileZoomFor(const alarmmap::MapView& view) {
    return std::clamp(static_cast<int>(std::floor(tileZoomOf(view))), 0, kMaxTileZoom);
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

MapLayer::RegionLevel MapLayer::makeLevel(const std::vector<RegionShape>& shapes) {
    alarmmap::MeshBuilder fills;
    std::array<alarmmap::MeshBuilder, kHromadaRank + 1> borders;
    for (const RegionShape& shape : shapes) {
        fills.beginItem();
        fills.addTriangles(shape.vertices, shape.indices);
        if (shape.rank < kOblastRank || shape.rank > kHromadaRank) continue;
        for (const auto& ring : shape.outlines) borders[shape.rank].addPolyline(ring, true);
    }
    RegionLevel level{&shapes, GpuGeometry(fills), {}};
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) level.borders[rank] = GpuGeometry(borders[rank]);
    return level;
}

bool MapLayer::Selection::covers(const RegionShape& shape) const {
    return shape.boundsMax.x >= min.x && shape.boundsMin.x <= max.x && shape.boundsMax.y >= min.y &&
           shape.boundsMin.y <= max.y;
}

MapLayer::MapLayer(const std::vector<RegionShape>& coarse, double coarseTolerance,
                   const std::vector<RegionShape>& detailed, const std::vector<alarmmap::City>& cities,
                   alarmmap::TileStore& tiles, const Font& font)
    : coarse_(makeLevel(coarse)),
      detailed_(makeLevel(detailed)),
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
             {cache_, country_.sharp, country_.blurred, redAlerts_.sharp, redAlerts_.blurred,
              yellowAlerts_.sharp, yellowAlerts_.blurred}) {
            UnloadRenderTexture(target);
        }
    }
    UnloadTexture(vignette_);
    UnloadShader(glowShader_);
    UnloadShader(dimShader_);
}

void MapLayer::setAlertRegions(const std::set<std::string>* regionIds, const std::set<std::string>* yellow) {
    std::set<std::string> nextYellow;
    std::set<std::string> nextRed;
    if (regionIds) {
        for (const std::string& id : *regionIds) {
            (yellow && yellow->contains(id) ? nextYellow : nextRed).insert(id);
        }
    }
    if (nextRed == redRegionIds_ && nextYellow == yellowRegionIds_) return;
    redRegionIds_ = std::move(nextRed);
    yellowRegionIds_ = std::move(nextYellow);
    dirty_ = true;
}

void MapLayer::resize(int width, int height) {
    if (width_ > 0) {
        for (const RenderTexture2D& target :
             {cache_, country_.sharp, country_.blurred, redAlerts_.sharp, redAlerts_.blurred,
              yellowAlerts_.sharp, yellowAlerts_.blurred}) {
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
    redAlerts_ = {loadLinearTarget(maskWidth, maskHeight), loadLinearTarget(maskWidth, maskHeight)};
    yellowAlerts_ = {loadLinearTarget(maskWidth, maskHeight), loadLinearTarget(maskWidth, maskHeight)};
    width_ = width;
    height_ = height;
    dirty_ = true;
}

void MapLayer::drawFills(const Selection& selection, const std::set<std::string>* regions, Color color) const {
    const std::vector<RegionShape>& shapes = *selection.level->shapes;
    geometry_.begin(color);
    // Coarser regions first, so a district alert stays visible over its oblast.
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (size_t i = 0; i < shapes.size(); ++i) {
            const RegionShape& shape = shapes[i];
            if (shape.rank != rank || !selection.covers(shape)) continue;
            if (regions ? regions->contains(shape.id) : rank == kOblastRank) {
                geometry_.draw(selection.level->fills, i);
            }
        }
    }
    geometry_.end();
}

void MapLayer::drawAlertFills(const Selection& selection) const {
    const std::vector<RegionShape>& shapes = *selection.level->shapes;
    // Coarser regions first, so a district alert stays visible over its oblast.
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (size_t i = 0; i < shapes.size(); ++i) {
            const RegionShape& shape = shapes[i];
            if (shape.rank != rank || !selection.covers(shape)) continue;
            const bool yellow = yellowRegionIds_.contains(shape.id);
            if (!yellow && !redRegionIds_.contains(shape.id)) continue;
            geometry_.begin(yellow ? kYellowAlertFill : kAlertFill);
            geometry_.draw(selection.level->fills, i);
            geometry_.end();
        }
    }
}

void MapLayer::renderMask(const GlowMasks& masks, const Selection& selection, const Camera2D& camera,
                          const std::set<std::string>* regions, float blurRadius) {
    BeginTextureMode(masks.sharp);
    ClearBackground(BLANK);
    BeginMode2D(camera);
    drawFills(selection, regions, kMaskColor);
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
        geometry_.begin(color, widthPx * scale / camera.zoom);
        geometry_.drawAll(selection.level->borders[rank]);
        geometry_.end();
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

void MapLayer::drawLabels(const Camera2D& camera, float scale, const alarmmap::MapView& view,
                          const std::vector<std::shared_ptr<const alarmmap::BasemapTile>>& tiles) const {
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

    const double tileZoom = tileZoomOf(view);
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
    if (tileZoom >= kStreetNamesMinTileZoom) drawStreetNames(camera, scale, tiles, placed);
}

void MapLayer::drawStreetNames(const Camera2D& camera, float scale,
                               const std::vector<std::shared_ptr<const alarmmap::BasemapTile>>& tiles,
                               std::vector<Rectangle>& placed) const {
    const float size = kStreetFontPx * scale;
    const float spacing = kStreetSpacingPx * scale;
    const float outline = kStreetOutlinePx * scale;
    const float padding = kStreetPaddingPx * scale;
    const auto screenWidth = static_cast<float>(cache_.texture.width);
    const auto screenHeight = static_cast<float>(cache_.texture.height);

    // Finest tiles first, so a coarser tile only fills what they left free;
    // within a tile the more important roads come first.
    std::vector<const alarmmap::StreetName*> streets;
    for (auto tile = tiles.rbegin(); tile != tiles.rend(); ++tile) {
        const size_t first = streets.size();
        for (const alarmmap::StreetName& street : (*tile)->streets) streets.push_back(&street);
        std::stable_sort(streets.begin() + first, streets.end(),
                         [](const auto* a, const auto* b) { return a->kind < b->kind; });
    }

    struct Glyph {
        std::string text;
        float advance;
    };
    for (const alarmmap::StreetName* street : streets) {
        std::vector<Vector2> path;
        for (const MapPoint& point : street->points) path.push_back(GetWorldToScreen2D(toVector(point), camera));
        const bool onScreen = std::any_of(path.begin(), path.end(), [&](const Vector2& point) {
            return point.x >= 0 && point.y >= 0 && point.x <= screenWidth && point.y <= screenHeight;
        });
        if (!onScreen) continue;
        if (path.back().x < path.front().x) std::reverse(path.begin(), path.end());

        std::vector<float> reach(path.size(), 0);
        for (size_t i = 1; i < path.size(); ++i) {
            reach[i] = reach[i - 1] + std::hypot(path[i].x - path[i - 1].x, path[i].y - path[i - 1].y);
        }
        const float pathLength = reach.back();
        // Letters are at least this wide, which spares measuring names that cannot fit anyway.
        if (static_cast<float>(street->text.size() / 2) * kStreetMinGlyphShare * size > pathLength) continue;

        std::vector<Glyph> glyphs;
        float textLength = -spacing;
        const std::string& text = street->text;
        for (size_t i = 0; i < text.size();) {
            int length = 0;
            GetCodepointNext(text.c_str() + i, &length);
            std::string glyph = text.substr(i, static_cast<size_t>(length));
            const float advance = MeasureTextEx(font_, glyph.c_str(), size, 0).x;
            glyphs.push_back({std::move(glyph), advance});
            textLength += advance + spacing;
            i += static_cast<size_t>(length);
        }
        if (glyphs.empty() || textLength * kStreetFitMargin > pathLength) continue;

        const auto pointAt = [&](float distance) {
            const size_t next = static_cast<size_t>(
                std::upper_bound(reach.begin(), reach.end(), distance) - reach.begin());
            const size_t segment = std::clamp<size_t>(next, 1, path.size() - 1);
            const float span = reach[segment] - reach[segment - 1];
            const float along = span > 0 ? std::clamp((distance - reach[segment - 1]) / span, 0.0f, 1.0f) : 0;
            return Vector2{path[segment - 1].x + (path[segment].x - path[segment - 1].x) * along,
                           path[segment - 1].y + (path[segment].y - path[segment - 1].y) * along};
        };

        struct Placement {
            Vector2 center;
            float angle;
        };
        for (const float share : kStreetOffsetShares) {
            float distance = (pathLength - textLength) * share;
            std::vector<Placement> placements;
            std::vector<Rectangle> areas;
            bool fits = true;
            for (const Glyph& glyph : glyphs) {
                const float middle = distance + glyph.advance / 2;
                const float reachAhead = std::min(middle + glyph.advance / 2, pathLength);
                const float reachBehind = std::max(middle - glyph.advance / 2, 0.0f);
                const Vector2 behind = pointAt(reachBehind), ahead = pointAt(reachAhead);
                const float angle = std::atan2(ahead.y - behind.y, ahead.x - behind.x);
                const Vector2 center = pointAt(middle);
                distance += glyph.advance + spacing;

                if (!placements.empty()) {
                    float turn = std::abs(angle - placements.back().angle);
                    if (turn > PI) turn = 2 * PI - turn;
                    fits = turn <= kStreetMaxTurnRadians;
                }
                const float halfAlong = glyph.advance / 2 + padding, halfAcross = size * 0.4f + padding;
                const float halfWidth = std::abs(std::cos(angle)) * halfAlong + std::abs(std::sin(angle)) * halfAcross;
                const float halfHeight = std::abs(std::sin(angle)) * halfAlong + std::abs(std::cos(angle)) * halfAcross;
                const Rectangle area = {center.x - halfWidth, center.y - halfHeight, 2 * halfWidth, 2 * halfHeight};
                fits = fits && area.x >= 0 && area.y >= 0 && area.x + area.width <= screenWidth &&
                       area.y + area.height <= screenHeight &&
                       std::none_of(placed.begin(), placed.end(), [&](const Rectangle& other) {
                           return CheckCollisionRecs(area, other);
                       });
                if (!fits) break;
                placements.push_back({center, angle});
                areas.push_back(area);
            }
            if (!fits) continue;

            for (size_t i = 0; i < glyphs.size(); ++i) {
                const Vector2 origin = {glyphs[i].advance / 2, size / 2};
                const float degrees = placements[i].angle * RAD2DEG;
                const char* glyph = glyphs[i].text.c_str();
                for (int direction = 0; direction < kLabelOutlineDirections; ++direction) {
                    const float angle = 2 * PI * direction / kLabelOutlineDirections;
                    DrawTextPro(font_, glyph,
                                {placements[i].center.x + outline * std::cos(angle),
                                 placements[i].center.y + outline * std::sin(angle)},
                                origin, degrees, size, 0, kLabelOutline);
                }
                DrawTextPro(font_, glyph, placements[i].center, origin, degrees, size, 0, kStreetLabel);
            }
            placed.insert(placed.end(), areas.begin(), areas.end());
            break;
        }
    }
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
    if (!dirty_ || (gestureActive && rendered_ && cacheServes(view))) return;
    render(view);
    rendered_ = true;
    renderedView_ = view;
    renderedAt_ = now;
    dirty_ = false;
}

bool MapLayer::cacheServes(const alarmmap::MapView& view) const {
    const double magnification = view.pixelsPerUnit() / renderedView_.pixelsPerUnit();
    if (magnification > kMaxCacheMagnification || magnification < 1 / kMaxCacheMagnification) return false;
    const double cachedHalfWidth = cache_.texture.width / kSupersampling / 2 / renderedView_.pixelsPerUnit();
    const double cachedHalfHeight = cache_.texture.height / kSupersampling / 2 / renderedView_.pixelsPerUnit();
    const double shownHalfWidth = view.width() / 2 / view.pixelsPerUnit();
    const double shownHalfHeight = view.height() / 2 / view.pixelsPerUnit();
    return std::abs(view.center().x - renderedView_.center().x) + shownHalfWidth <= cachedHalfWidth &&
           std::abs(view.center().y - renderedView_.center().y) + shownHalfHeight <= cachedHalfHeight;
}

void MapLayer::render(const alarmmap::MapView& view) {
    const auto maskWidth = static_cast<float>(country_.sharp.texture.width);
    const auto maskHeight = static_cast<float>(country_.sharp.texture.height);
    const Camera2D maskCamera = viewCamera(view, maskWidth, maskHeight, kMaskScale);
    const Selection selection = select(view, maskWidth / kMaskScale / 2, maskHeight / kMaskScale / 2);
    renderMask(country_, selection, maskCamera, nullptr, kCountryHaloRadiusPx * uiScale() * kMaskScale);
    const bool hasRed = !redRegionIds_.empty();
    const bool hasYellow = !yellowRegionIds_.empty();
    const float alertGlowRadius = kAlertGlowRadiusPx * uiScale() * kMaskScale;
    if (hasRed) renderMask(redAlerts_, selection, maskCamera, &redRegionIds_, alertGlowRadius);
    if (hasYellow) renderMask(yellowAlerts_, selection, maskCamera, &yellowRegionIds_, alertGlowRadius);

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
    drawAlertFills(selection);
    EndMode2D();
    if (hasRed) drawGlow(redAlerts_, kAlertInnerGlow, kAlertOuterGlow);
    if (hasYellow) drawGlow(yellowAlerts_, kYellowInnerGlow, kYellowOuterGlow);
    BeginMode2D(camera);
    drawBorders(selection, camera, kSupersampling * uiScale());
    EndMode2D();
    drawLabels(camera, kSupersampling * uiScale(), view, basemap);
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

const MapLayer::TileGeometry& MapLayer::tileGeometry(const alarmmap::BasemapTile& tile) {
    const auto found = tileGeometry_.find(tile.id);
    if (found != tileGeometry_.end()) {
        found->second.lastUsed = renderCount_;
        return found->second;
    }
    alarmmap::MeshBuilder fills;
    for (const alarmmap::FillMesh& mesh : tile.fills) {
        fills.beginItem();
        fills.addTriangles(mesh.vertices, mesh.indices);
    }
    alarmmap::MeshBuilder lines;
    for (const auto& polylines : tile.lines) {
        lines.beginItem();
        for (const auto& polyline : polylines) lines.addPolyline(polyline, false);
    }
    return tileGeometry_.emplace(tile.id, TileGeometry{GpuGeometry(fills), GpuGeometry(lines), renderCount_})
        .first->second;
}

void MapLayer::drawBasemap(const std::vector<std::shared_ptr<const alarmmap::BasemapTile>>& tiles,
                           const Camera2D& camera, float scale, double viewZoom) {
    ++renderCount_;
    std::vector<const TileGeometry*> geometry;
    for (const auto& tile : tiles) geometry.push_back(&tileGeometry(*tile));
    for (size_t fill = 0; fill < std::size(kFillColors); ++fill) {
        geometry_.begin(kFillColors[fill]);
        for (const TileGeometry* tile : geometry) geometry_.draw(tile->fills, fill);
        geometry_.end();
    }
    // Lines widen a little as the view zooms in.
    const float widthScale = static_cast<float>(std::clamp(1 + 0.25 * std::log2(viewZoom), 1.0, 2.2));
    for (size_t line = 0; line < std::size(kLineStyles); ++line) {
        const LineStyle& style = kLineStyles[line];
        geometry_.begin(style.color, style.widthPx * widthScale * scale / camera.zoom);
        for (size_t i = 0; i < tiles.size(); ++i) {
            if (tiles[i]->id.z >= style.minZoom) geometry_.draw(geometry[i]->lines, line);
        }
        geometry_.end();
    }
    // Geometry of tiles not drawn lately goes once there is a lot of it.
    for (auto it = tileGeometry_.begin(); it != tileGeometry_.end() && tileGeometry_.size() > kMaxTileGeometries;) {
        it = renderCount_ - it->second.lastUsed > kTileGeometryKeepRenders ? tileGeometry_.erase(it) : std::next(it);
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
    const std::vector<RegionShape>& shapes = *selection.level->shapes;
    BeginMode2D(viewCamera(view));
    for (int rank = kOblastRank; rank <= kHromadaRank; ++rank) {
        for (size_t i = 0; i < shapes.size(); ++i) {
            if (shapes[i].rank != rank || !selection.covers(shapes[i])) continue;
            const auto opacity = opacityByRegion.find(shapes[i].id);
            if (opacity == opacityByRegion.end()) continue;
            geometry_.begin(Fade(kHighlight, opacity->second));
            geometry_.draw(selection.level->fills, i);
            geometry_.end();
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
