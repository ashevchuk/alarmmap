#pragma once

#include "cities.hpp"
#include "gaussian_blur.hpp"
#include "map_view.hpp"
#include "tile_store.hpp"
#include "region_geometry.hpp"

#include <raylib.h>

#include <map>
#include <set>
#include <string>
#include <vector>

// Camera that shows `view` in the middle of a target of the given size,
// magnified `scale` times for supersampled targets.
Camera2D viewCamera(const alarmmap::MapView& view, float targetWidth, float targetHeight,
                    float scale = 1);
// Camera for the window the view describes.
Camera2D viewCamera(const alarmmap::MapView& view);

// Region fills, borders, glows, oblast names and cities, rendered into a cached,
// supersampled texture that extends past the window edges. While the view
// moves, the old cache is drawn shifted and scaled, and it is re-rendered
// once the view settles.
class MapLayer {
public:
    // Requires an initialised window (an OpenGL context). `coarse` shapes are
    // simplified with `coarseTolerance` map units and drawn while that stays
    // under a pixel; closer in, `detailed` ones are drawn. Keeps references to
    // the shapes, `cities` and `font`, which must outlive the layer.
    MapLayer(const std::vector<alarmmap::RegionShape>& coarse, double coarseTolerance,
             const std::vector<alarmmap::RegionShape>& detailed,
             const std::vector<alarmmap::City>& cities, alarmmap::TileStore& tiles, const Font& font);
    ~MapLayer();
    MapLayer(const MapLayer&) = delete;
    MapLayer& operator=(const MapLayer&) = delete;

    // Regions to highlight; nullptr when alerts are not known yet.
    void setAlertRegions(const std::set<std::string>* regionIds);
    // Re-renders the cache when needed. Must be called outside
    // BeginDrawing/BeginTextureMode, because it switches render targets.
    void prepare(const alarmmap::MapView& view, double now);
    // Draws the cached map as seen in `view`.
    void draw(const alarmmap::MapView& view) const;
    // Fills regions over the drawn map with the highlight colour at the given
    // opacity; meant for a few regions that change every frame.
    void drawHighlights(const std::map<std::string, float>& opacityByRegion,
                        const alarmmap::MapView& view) const;
    // Darkens the window edges.
    void drawVignette(float width, float height) const;

private:
    struct DrawableRegion {
        const alarmmap::RegionShape* shape;
        // Outlines with the first point repeated at the end.
        std::vector<std::vector<Vector2>> closedOutlines;
    };

    // Regions of one detail level that intersect an area of the map.
    struct Selection {
        const std::vector<DrawableRegion>* regions;
        alarmmap::MapPoint min;
        alarmmap::MapPoint max;
        bool showHromadaBorders;

        bool covers(const DrawableRegion& region) const;
    };

    struct Label {
        std::string text;
        Vector2 position;
    };

    // A sharp mask and its blurred copy; together they give edge glows.
    struct GlowMasks {
        RenderTexture2D sharp{};
        RenderTexture2D blurred{};
    };

    void resize(int width, int height);
    void render(const alarmmap::MapView& view);
    static std::vector<DrawableRegion> makeDrawable(const std::vector<alarmmap::RegionShape>& shapes);
    // Regions to draw for `view` within the given distances from its centre.
    Selection select(const alarmmap::MapView& view, double halfWidthPx, double halfHeightPx) const;
    void renderMask(const GlowMasks& masks, const Selection& selection, const Camera2D& camera,
                    bool alertsOnly, float blurRadius);
    void drawFills(const Selection& selection, bool alertsOnly, Color color) const;
    void drawBorders(const Selection& selection, const Camera2D& camera, float scale) const;
    void drawGlow(const GlowMasks& masks, Vector4 inner, Vector4 outer) const;
    // Oblast names, then the cities the zoom calls for whose labels fit
    // without overlapping anything placed before.
    void drawLabels(const Camera2D& camera, float scale, double viewZoom) const;
    // Basemap tiles to draw for the tiles wanted now: each loaded tile, or
    // the nearest loaded ancestor of a missing one; lower zooms first.
    std::vector<std::shared_ptr<const alarmmap::BasemapTile>> tilesToDraw() const;
    void drawBasemap(const std::vector<std::shared_ptr<const alarmmap::BasemapTile>>& tiles,
                     const Camera2D& camera, float scale, double viewZoom) const;
    void drawOutsideDimming() const;

    std::vector<DrawableRegion> coarse_;
    std::vector<DrawableRegion> detailed_;
    double coarseTolerance_;
    const std::vector<alarmmap::City>& cities_;
    alarmmap::TileStore& tiles_;
    std::vector<alarmmap::TileId> wantedTiles_;
    std::uint64_t renderedTilesVersion_ = 0;
    Shader dimShader_{};
    const Font& font_;
    std::vector<Label> labels_;
    std::set<std::string> alertRegionIds_;
    GaussianBlur blur_;
    Shader glowShader_{};
    int sharpMaskLocation_ = -1;
    int innerGlowLocation_ = -1;
    int outerGlowLocation_ = -1;
    Texture2D vignette_{};
    RenderTexture2D cache_{};
    GlowMasks country_;
    GlowMasks alerts_;
    // Window size the targets were made for.
    int width_ = 0;
    int height_ = 0;
    bool dirty_ = true;
    alarmmap::MapView renderedView_;
    alarmmap::MapView pendingView_;
    double pendingSince_ = 0;
    double renderedAt_ = 0;
};
