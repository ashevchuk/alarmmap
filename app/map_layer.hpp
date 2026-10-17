#pragma once

#include "gaussian_blur.hpp"
#include "map_view.hpp"
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

// Region fills, borders, glows and oblast names, rendered into a cached,
// supersampled texture that extends past the window edges. While the view
// moves, the old cache is drawn shifted and scaled, and it is re-rendered
// once the view settles.
class MapLayer {
public:
    // Requires an initialised window (an OpenGL context). Keeps references
    // to `shapes` and `font`, which must outlive the layer.
    MapLayer(const std::vector<alarmmap::RegionShape>& shapes, const Font& font);
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
    void renderMask(const GlowMasks& masks, const Camera2D& camera, bool alertsOnly, float blurRadius);
    void drawFills(bool alertsOnly, Color color) const;
    void drawBorders(const Camera2D& camera, float scale) const;
    void drawGlow(const GlowMasks& masks, Vector4 inner, Vector4 outer) const;
    void drawLabels(const Camera2D& camera, float scale) const;

    std::vector<DrawableRegion> regions_;
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
