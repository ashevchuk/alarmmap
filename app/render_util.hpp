#pragma once

#include <raylib.h>

// Compiles a fragment shader over raylib's default vertex shader; throws
// std::runtime_error when compilation fails.
Shader loadFragmentShader(const char* source, const char* name);

// Render target with bilinear filtering and clamped edges, as blurs need.
RenderTexture2D loadLinearTarget(int width, int height);

// Render textures are stored bottom-up; drawing with this source rectangle
// shows them upright.
Rectangle flippedSource(const Texture2D& texture);

// Draws a render texture stretched over the given screen rectangle.
void drawTargetStretched(const RenderTexture2D& target, float width, float height);
