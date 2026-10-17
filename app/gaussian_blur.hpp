#pragma once

#include <raylib.h>

// Separable Gaussian blur of the alpha channel of a render target, as the
// site's heat layer does it: several passes with a doubling step.
class GaussianBlur {
public:
    // Requires an initialised window (an OpenGL context).
    GaussianBlur();
    ~GaussianBlur();
    GaussianBlur(const GaussianBlur&) = delete;
    GaussianBlur& operator=(const GaussianBlur&) = delete;

    // Blurs `target` in place; the radius is in texels. Must be called
    // outside BeginDrawing/BeginTextureMode.
    void apply(const RenderTexture2D& target, float radius);

private:
    void pass(const RenderTexture2D& source, const RenderTexture2D& destination,
              Vector2 direction) const;

    Shader shader_{};
    int directionLocation_ = -1;
    RenderTexture2D scratch_{};
    int width_ = 0;
    int height_ = 0;
};
