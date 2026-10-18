#include "render_util.hpp"

#include <rlgl.h>

#ifdef __ANDROID__
#include <android/configuration.h>
#include <android_native_app_glue.h>
#endif

#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

#ifdef __ANDROID__
// OpenGL ES 3.0 accepts the desktop GLSL 3.30 sources under its own version
// line, with the float precision stated explicitly.
std::string targetGlsl(const char* source) {
    constexpr std::string_view kDesktopVersion = "#version 330";
    std::string glsl = source;
    if (!glsl.starts_with(kDesktopVersion)) throw std::runtime_error("shader without #version 330");
    glsl.replace(0, kDesktopVersion.size(), "#version 300 es\nprecision highp float;");
    return glsl;
}
#else
std::string targetGlsl(const char* source) { return source; }
#endif

}  // namespace

Shader loadFragmentShader(const char* source, const char* name) {
    const Shader shader = LoadShaderFromMemory(nullptr, targetGlsl(source).c_str());
    // raylib falls back to its default shader when compilation fails.
    if (shader.id == rlGetShaderIdDefault()) {
        throw std::runtime_error(std::string("cannot compile ") + name + " shader");
    }
    return shader;
}

RenderTexture2D loadLinearTarget(int width, int height) {
    const RenderTexture2D target = LoadRenderTexture(width, height);
    if (!IsRenderTextureValid(target)) throw std::runtime_error("cannot create render target");
    SetTextureFilter(target.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(target.texture, TEXTURE_WRAP_CLAMP);
    return target;
}

Rectangle flippedSource(const Texture2D& texture) {
    return {0, 0, static_cast<float>(texture.width), -static_cast<float>(texture.height)};
}

void drawTargetStretched(const RenderTexture2D& target, float width, float height) {
    DrawTexturePro(target.texture, flippedSource(target.texture), {0, 0, width, height}, {0, 0}, 0,
                   WHITE);
}

#ifdef __ANDROID__
float uiScale() {
    // Android's density-independent pixel is 1/160 inch.
    static const float scale = [] {
        const int32_t density = AConfiguration_getDensity(GetAndroidApp()->config);
        return density > 0 ? density / 160.0f : 1.0f;
    }();
    return scale;
}
#else
float uiScale() { return 1; }
#endif

float markerScale() { return std::sqrt(uiScale()); }
