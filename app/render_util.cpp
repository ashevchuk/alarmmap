#include "render_util.hpp"

#include <rlgl.h>

#include <stdexcept>
#include <string>

Shader loadFragmentShader(const char* source, const char* name) {
    const Shader shader = LoadShaderFromMemory(nullptr, source);
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
