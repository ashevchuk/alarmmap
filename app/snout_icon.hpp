#pragma once

#include "target_kind.hpp"

#include <raylib.h>

// Top-down silhouette of a target kind whose nose is a pig snout. Rendered
// once into a texture and drawn rotated to the target's heading.
class SnoutIcon {
public:
    // Requires an initialised window (an OpenGL context).
    explicit SnoutIcon(alarmmap::TargetKind kind);
    ~SnoutIcon();
    SnoutIcon(const SnoutIcon&) = delete;
    SnoutIcon& operator=(const SnoutIcon&) = delete;

    // Heading is a compass bearing: 0 points up (north), clockwise.
    void draw(Vector2 screenCenter, float sizePx, float headingDegrees, Color tint = WHITE) const;

private:
    RenderTexture2D texture_{};
};
