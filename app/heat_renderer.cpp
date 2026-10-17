#include "heat_renderer.hpp"

#include "render_util.hpp"

#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace {

// Opacity curve of the site's satellite heat layer with a yellow palette and
// higher opacity, so the glow stands out over dark red alert regions. Output
// is premultiplied by alpha.
constexpr const char* kCompositeShader = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
out vec4 finalColor;

vec3 palette(float value) {
    vec3 cold = vec3(1.0, 240.0 / 255.0, 150.0 / 255.0);
    vec3 warm = vec3(1.0, 215.0 / 255.0, 80.0 / 255.0);
    vec3 hot = vec3(1.0, 180.0 / 255.0, 40.0 / 255.0);
    vec3 core = vec3(1.0, 140.0 / 255.0, 20.0 / 255.0);
    if (value < 0.35) return mix(cold, warm, value / 0.35);
    if (value < 0.65) return mix(warm, hot, (value - 0.35) / 0.30);
    return mix(hot, core, (value - 0.65) / 0.35);
}

void main() {
    float density = clamp(texture(texture0, fragTexCoord).a, 0.0, 1.0);
    if (density < 0.002) discard;
    float intensity = min(1.0, density * 1.9);
    float alpha = 0.75 * pow(intensity, 0.65) * min(1.0, intensity / 0.2);
    finalColor = vec4(palette(density) * alpha, alpha);
}
)";

constexpr float kBlurRadiusPerCellPx = 0.75f;
constexpr float kMinimumBlurRadiusPx = 6;
constexpr float kMaximumBlurRadiusPx = 70;

}  // namespace

HeatRenderer::HeatRenderer() : composite_(loadFragmentShader(kCompositeShader, "heat composite")) {}

HeatRenderer::~HeatRenderer() {
    if (width_ > 0) UnloadRenderTexture(mask_);
    UnloadShader(composite_);
}

bool HeatRenderer::isStale(double now, std::uint64_t dataVersion, const Camera2D& camera, int width,
                           int height) const {
    const bool cameraMoved = camera.zoom != preparedCamera_.zoom ||
                             camera.target.x != preparedCamera_.target.x ||
                             camera.target.y != preparedCamera_.target.y ||
                             camera.offset.x != preparedCamera_.offset.x ||
                             camera.offset.y != preparedCamera_.offset.y;
    return !prepared_ || cameraMoved || dataVersion != preparedVersion_ || width != preparedWidth_ ||
           height != preparedHeight_ || now - preparedAt_ >= kRefreshIntervalSeconds;
}

void HeatRenderer::prepare(const std::vector<alarmmap::HeatQuad>& quads, const Camera2D& camera,
                           int width, int height, double now, std::uint64_t dataVersion) {
    prepared_ = true;
    preparedAt_ = now;
    preparedVersion_ = dataVersion;
    preparedWidth_ = width;
    preparedHeight_ = height;
    preparedCamera_ = camera;
    hasHeat_ = !quads.empty() && width > 0 && height > 0;
    if (!hasHeat_) return;
    if (width != width_ || height != height_) {
        if (width_ > 0) UnloadRenderTexture(mask_);
        mask_ = loadLinearTarget(width, height);
        width_ = width;
        height_ = height;
    }

    BeginTextureMode(mask_);
    ClearBackground(BLANK);
    BeginMode2D(camera);
    // Maximum, not sum: a shared edge of neighbouring cells must not get
    // brighter because the same pixel is drawn twice.
    rlSetBlendFactors(RL_ONE, RL_ONE, RL_MAX);
    BeginBlendMode(BLEND_CUSTOM);
    double cellPixels = 0;
    for (const alarmmap::HeatQuad& quad : quads) {
        const auto& rect = quad.rect;
        const Rectangle rectangle = {static_cast<float>(rect.left), static_cast<float>(rect.top),
                                     static_cast<float>(rect.right - rect.left),
                                     static_cast<float>(rect.bottom - rect.top)};
        const auto alpha = static_cast<unsigned char>(std::lround(quad.strength * 255));
        DrawRectangleRec(rectangle, {255, 255, 255, alpha});
        cellPixels += std::min(rectangle.width, rectangle.height) * camera.zoom;
    }
    EndBlendMode();
    EndMode2D();
    EndTextureMode();

    const float blurRadius =
        std::clamp(static_cast<float>(cellPixels / quads.size()) * kBlurRadiusPerCellPx,
                   kMinimumBlurRadiusPx, kMaximumBlurRadiusPx);
    blur_.apply(mask_, blurRadius);
}

void HeatRenderer::composite() const {
    if (!hasHeat_) return;
    BeginShaderMode(composite_);
    BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
    drawTargetStretched(mask_, static_cast<float>(width_), static_cast<float>(height_));
    EndBlendMode();
    EndShaderMode();
}
