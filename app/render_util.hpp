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

// Multiplier for target markers and their effects. They grow slower than
// text: at the full UI scale they would cover too much of a phone's map.
float markerScale();

// Runs `draw` with the drawing scaled by `scale` around `pivot`, so sizes
// and offsets given in unscaled pixels come out scaled.
template <typename Draw>
void drawScaledAround(Vector2 pivot, float scale, Draw draw) {
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
