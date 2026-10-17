#include "gaussian_blur.hpp"

#include "render_util.hpp"

#include <rlgl.h>

#include <algorithm>
#include <cmath>

namespace {

// Five taps per pass give nine effective samples thanks to linear filtering.
constexpr const char* kBlurShader = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 direction;
out vec4 finalColor;

void main() {
    float density = texture(texture0, fragTexCoord).a * 0.2270270270;
    density += texture(texture0, fragTexCoord + direction * 1.3846153846).a * 0.3162162162;
    density += texture(texture0, fragTexCoord - direction * 1.3846153846).a * 0.3162162162;
    density += texture(texture0, fragTexCoord + direction * 3.2307692308).a * 0.0702702703;
    density += texture(texture0, fragTexCoord - direction * 3.2307692308).a * 0.0702702703;
    finalColor = vec4(density);
}
)";

constexpr float kKernelOuterOffset = 3.2307692308f;
constexpr float kRemainingVarianceThreshold = 0.0001f;

}  // namespace

GaussianBlur::GaussianBlur()
    : shader_(loadFragmentShader(kBlurShader, "blur")),
      directionLocation_(GetShaderLocation(shader_, "direction")) {}

GaussianBlur::~GaussianBlur() {
    if (width_ > 0) UnloadRenderTexture(scratch_);
    UnloadShader(shader_);
}

void GaussianBlur::pass(const RenderTexture2D& source, const RenderTexture2D& destination,
                        Vector2 direction) const {
    BeginTextureMode(destination);
    ClearBackground(BLANK);
    BeginShaderMode(shader_);
    SetShaderValue(shader_, directionLocation_, &direction, SHADER_UNIFORM_VEC2);
    // Overwrite instead of blending: the pass output replaces the target.
    rlSetBlendFactors(RL_ONE, RL_ZERO, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM);
    DrawTextureRec(source.texture, flippedSource(source.texture), {0, 0}, WHITE);
    EndBlendMode();
    EndShaderMode();
    EndTextureMode();
}

void GaussianBlur::apply(const RenderTexture2D& target, float radius) {
    const int width = target.texture.width;
    const int height = target.texture.height;
    if (width != width_ || height != height_) {
        if (width_ > 0) UnloadRenderTexture(scratch_);
        scratch_ = loadLinearTarget(width, height);
        width_ = width;
        height_ = height;
    }
    // Start from neighbouring texels and double the step each round: every
    // pass reads an already smoothed mask, so no banding appears between
    // samples. The sum of squared steps keeps the overall blur width.
    float remainingVariance = std::pow(radius / kKernelOuterOffset, 2.0f);
    float maximumStep = 1;
    while (remainingVariance > kRemainingVarianceThreshold) {
        const float step = std::min(maximumStep, std::sqrt(remainingVariance));
        pass(target, scratch_, {step / width, 0});
        pass(scratch_, target, {0, step / height});
        remainingVariance -= step * step;
        maximumStep *= 2;
    }
}
