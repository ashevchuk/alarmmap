#pragma once

#include "gaussian_blur.hpp"
#include "heat_cells.hpp"

#include <raylib.h>

#include <cstdint>
#include <vector>

// Heat trail rendering of map.ukrainealarm.com: cells are merged into one
// mask with MAX blending, blurred, then coloured by density, so neighbouring
// cells form one smooth patch without square seams.
class HeatRenderer {
public:
    // Requires an initialised window (an OpenGL context).
    HeatRenderer();
    ~HeatRenderer();
    HeatRenderer(const HeatRenderer&) = delete;
    HeatRenderer& operator=(const HeatRenderer&) = delete;

    // The heat fades slowly, so its mask is rebuilt at most this often unless
    // the data or the target size changed.
    static constexpr double kRefreshIntervalSeconds = 0.1;

    // Whether prepare() would produce a different picture than the last one.
    bool isStale(double now, std::uint64_t dataVersion, int width, int height) const;
    // Renders and blurs the mask offscreen. Must be called outside
    // BeginDrawing/BeginTextureMode, because it switches render targets.
    void prepare(const std::vector<alarmmap::HeatQuad>& quads, const Camera2D& camera, int width,
                 int height, double now, std::uint64_t dataVersion);
    // Draws the prepared heat over the current render target.
    void composite() const;

private:
    GaussianBlur blur_;
    Shader composite_{};
    RenderTexture2D mask_{};
    int width_ = 0;
    int height_ = 0;
    bool hasHeat_ = false;
    bool prepared_ = false;
    double preparedAt_ = 0;
    std::uint64_t preparedVersion_ = 0;
    int preparedWidth_ = 0;
    int preparedHeight_ = 0;
};
