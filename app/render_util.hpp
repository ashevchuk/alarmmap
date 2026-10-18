#pragma once

#include <raylib.h>
#include <rlgl.h>

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

// Multiplier for the pixel sizes of text, icons and lines: 1 on the desktop,
// the display density in density-independent pixels on Android.
float uiScale();

// Runs `draw` with the drawing scaled by uiScale() around `pivot`, so sizes
// and offsets given in unscaled pixels come out at the UI scale.
template <typename Draw>
void drawScaledAround(Vector2 pivot, Draw draw) {
    const float scale = uiScale();
    rlPushMatrix();
    rlTranslatef(pivot.x, pivot.y, 0);
    rlScalef(scale, scale, 1);
    rlTranslatef(-pivot.x, -pivot.y, 0);
    draw();
    rlPopMatrix();
}

#ifdef __ANDROID__
// Defined by raylib's Android platform layer without a declaration in raylib.h.
extern "C" struct android_app* GetAndroidApp(void);
#endif
