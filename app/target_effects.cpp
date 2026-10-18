#include "target_effects.hpp"

#include "render_util.hpp"
#include "target_motion.hpp"

#include <algorithm>
#include <cmath>

namespace {

using alarmmap::TargetMotion;

constexpr float kExplosionRadiusPx = 44;
constexpr float kTeleportRadiusPx = 40;
constexpr float kBeamHeightPx = 150;
constexpr Color kTeleportColor = {120, 225, 255, 255};
constexpr float kDashPx = 8;
constexpr float kGapPx = 6;
constexpr Color kPathColor = {255, 232, 170, 255};

float clamp01(double value) { return static_cast<float>(std::clamp(value, 0.0, 1.0)); }
float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
float easeIn(float t) { return t * t * t; }
float easeOutBack(float t) {
    constexpr float overshoot = 1.7f;
    const float u = t - 1;
    return 1 + (overshoot + 1) * u * u * u + overshoot * u * u;
}
// Rises and falls back to zero over q in 0..1.
float bump(float q) { return std::sin(PI * std::clamp(q, 0.0f, 1.0f)); }

// Deterministic value in 0..1 for the seed and index.
float random01(std::uint32_t seed, std::uint32_t index) {
    std::uint32_t x = seed ^ (index * 0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<float>(x) / 4294967295.0f;
}

Color withAlpha(Color color, float alpha) {
    color.a = static_cast<unsigned char>(255 * std::clamp(alpha, 0.0f, 1.0f));
    return color;
}

Vector2 offset(Vector2 center, float angle, float distance) {
    return {center.x + std::cos(angle) * distance, center.y + std::sin(angle) * distance};
}

void drawRingAt(Vector2 center, float radius, float thickness, Color color) {
    if (radius <= 0 || color.a == 0) return;
    DrawRing(center, std::max(0.0f, radius - thickness), radius, 0, 360, 64, color);
}

}  // namespace

std::uint32_t effectSeed(std::string_view trackId) {
    std::uint32_t hash = 2166136261u;
    for (const char c : trackId) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    return hash;
}

void drawExplosion(Vector2 center, double age, std::uint32_t seed) {
    const auto t = static_cast<float>(age);
    if (t < 0 || t >= TargetMotion::kExplosionSeconds) return;
    const float progress = t / static_cast<float>(TargetMotion::kExplosionSeconds);
    const float r = kExplosionRadiusPx;

    // Smoke darkens the map, so it goes on with normal blending.
    const float smokeIn = clamp01((t - 0.15) / 0.25);
    for (std::uint32_t i = 0; i < 5; ++i) {
        const float angle = 2 * PI * random01(seed, i);
        const Vector2 puff = offset(center, angle, r * 0.55f * easeOut(progress) * random01(seed, i + 10));
        const float radius = r * (0.35f + 0.55f * easeOut(progress)) * (0.8f + 0.4f * random01(seed, i + 20));
        const Color smoke = {30, 26, 24, 255};
        DrawCircleGradient(puff, radius, withAlpha(smoke, 0.5f * smokeIn * (1 - progress)), withAlpha(smoke, 0));
    }

    BeginBlendMode(BLEND_ADDITIVE);
    const float heat = clamp01(1 - t / 1.1);
    const float fireAlpha = clamp01(t / 0.05) * std::pow(1 - progress, 1.3f);
    for (std::uint32_t i = 0; i < 6; ++i) {
        const float angle = 2 * PI * random01(seed, i + 30);
        const float spread = r * 0.35f * easeOut(clamp01(t / 0.6)) * random01(seed, i + 40);
        const float radius = r * (0.4f + 0.55f * easeOut(clamp01(t / 0.45))) * (0.8f + 0.4f * random01(seed, i + 50));
        const Color inner = {255, static_cast<unsigned char>(80 + 150 * heat),
                             static_cast<unsigned char>(20 + 80 * heat), 255};
        DrawCircleGradient(offset(center, angle, spread), radius, withAlpha(inner, 0.9f * fireAlpha),
                           withAlpha({220, 40, 0, 255}, 0));
    }

    if (t < 0.4f) {
        const float radius = r * (0.25f + 0.45f * easeOut(clamp01(t / 0.12)));
        DrawCircleGradient(center, radius, withAlpha({255, 255, 235, 255}, 1 - t / 0.4f),
                           withAlpha({255, 200, 120, 255}, 0));
    }

    if (const float q = t / 0.75f; q < 1) {
        drawRingAt(center, r * 0.4f + r * 1.8f * easeOut(q), 1 + 3 * (1 - q),
                   withAlpha({255, 220, 170, 255}, 0.8f * (1 - q) * (1 - q)));
    }

    constexpr std::uint32_t kSparks = 20;
    constexpr float kSparkDrag = 0.3f;
    constexpr float kSparkGravityPx = 30;
    for (std::uint32_t i = 0; i < kSparks; ++i) {
        const float life = 0.7f + 0.7f * random01(seed, i + 60);
        if (t >= life) continue;
        const float angle = 2 * PI * (i + random01(seed, i + 70)) / kSparks;
        const float speed = r * (2.0f + 1.6f * random01(seed, i + 80));
        const auto at = [&](float time) {
            const float distance = speed * kSparkDrag * (1 - std::exp(-time / kSparkDrag));
            Vector2 point = offset(center, angle, distance);
            point.y += kSparkGravityPx * time * time;
            return point;
        };
        const float fade = 1 - t / life;
        const Color spark = {255, static_cast<unsigned char>(200 - 120 * (t / life)), 80, 255};
        DrawLineEx(at(std::max(0.0f, t - 0.06f)), at(t), 2, withAlpha(spark, fade));
    }
    EndBlendMode();
}

void drawScorch(Vector2 center, double age, double endsAt, std::uint32_t seed) {
    const double fadeFrom = TargetMotion::kExplosionSeconds;
    const float alpha = clamp01((age - 0.3) / 0.5) * (1 - clamp01((age - fadeFrom) / (endsAt - fadeFrom)));
    if (alpha <= 0) return;
    const auto t = static_cast<float>(age);
    const float phase = 2 * PI * random01(seed, 0);
    const float flicker = 0.8f + 0.2f * std::sin(t * 9 + phase) * std::sin(t * 3.7f + 2 * phase);

    const Color char_ = {20, 14, 12, 255};
    DrawCircleGradient(center, 14, withAlpha(char_, 0.7f * alpha), withAlpha(char_, 0));

    BeginBlendMode(BLEND_ADDITIVE);
    DrawCircleGradient(center, 16, withAlpha({255, 90, 30, 255}, 0.6f * alpha * flicker),
                       withAlpha({255, 40, 0, 255}, 0));
    drawRingAt(center, 11, 1.2f, withAlpha({255, 140, 60, 255}, 0.3f * alpha));
    for (std::uint32_t i = 0; i < 5; ++i) {
        const float angle = 2 * PI * random01(seed, i + 100);
        const float glow = 0.6f + 0.4f * std::sin(t * (4 + 3 * random01(seed, i + 110)) + 6 * random01(seed, i + 120));
        DrawCircleV(offset(center, angle, 5 + 5 * random01(seed, i + 130)), 1.4f,
                    withAlpha({255, 170, 90, 255}, alpha * glow));
    }
    DrawCircleV(center, 2.5f, withAlpha({255, 210, 140, 255}, 0.9f * alpha * flicker));
    EndBlendMode();
}

void drawTeleport(Vector2 center, double age, std::uint32_t seed) {
    const auto t = static_cast<float>(age);
    if (t < 0 || t >= kTeleportSeconds) return;
    const float r = kTeleportRadiusPx;
    BeginBlendMode(BLEND_ADDITIVE);

    if (const float q = t / 0.9f; q < 1) {
        const float envelope = bump(q);
        for (const auto& [width, color] : {std::pair{3 + 16 * (1 - q), kTeleportColor},
                                           std::pair{1 + 5 * (1 - q), Color{235, 252, 255, 255}}}) {
            const Color lit = withAlpha(color, 0.8f * envelope);
            const Color clear = withAlpha(color, 0);
            // Colours go counter-clockwise from the top left corner.
            DrawRectangleGradientEx({center.x - width / 2, center.y - kBeamHeightPx, width, kBeamHeightPx},
                                    clear, lit, lit, clear);
            DrawRectangleGradientEx({center.x - width / 2, center.y, width, kBeamHeightPx}, lit, clear, clear,
                                    lit);
        }
    }

    for (int ring = 0; ring < 3; ++ring) {
        const float q = (t - 0.12f * ring) / 0.6f;
        if (q <= 0 || q >= 1) continue;
        drawRingAt(center, 6 + r * 2 * (1 - easeIn(q)), 1.5f, withAlpha(kTeleportColor, 0.85f * bump(q)));
    }

    for (std::uint32_t i = 0; i < 16; ++i) {
        const float q = (t - 0.3f * random01(seed, i)) / 0.7f;
        if (q <= 0 || q >= 1) continue;
        const float angle = 2 * PI * random01(seed, i + 10) + 2.5f * q;
        DrawCircleV(offset(center, angle, r * 2.2f * (1 - easeIn(q))), 1.8f,
                    withAlpha({200, 245, 255, 255}, bump(q)));
    }

    if (const float q = (t - 0.6f) / 0.6f; q > 0) {
        DrawCircleGradient(center, r * (0.3f + 0.6f * easeOut(q)), withAlpha({230, 250, 255, 255}, 0.85f * (1 - q)),
                           withAlpha(kTeleportColor, 0));
        drawRingAt(center, r * 0.3f + r * 1.4f * easeOut(q), 2, withAlpha(kTeleportColor, 0.7f * (1 - q)));
    }
    EndBlendMode();
}

float teleportIconScale(double age) {
    const float q = clamp01((age - 0.5) / 0.45);
    return 0.2f + 0.8f * easeOutBack(q);
}

void drawDashedPath(const std::vector<Vector2>& points, float opacity) {
    if (points.size() < 2 || opacity <= 0) return;
    const float ui = uiScale();
    const float dash = kDashPx * ui;
    const float period = (kDashPx + kGapPx) * ui;
    for (const auto& [lineWidth, color] : {std::pair{4.5f, withAlpha(BLACK, 0.6f * opacity)},
                                           std::pair{2.0f, withAlpha(kPathColor, 0.95f * opacity)}}) {
        const float thickness = lineWidth * ui;
        // Distance into the current dash-and-gap period.
        float phase = 0;
        for (size_t i = 1; i < points.size(); ++i) {
            const Vector2 from = points[i - 1];
            const Vector2 to = points[i];
            const float length = std::hypot(to.x - from.x, to.y - from.y);
            if (length <= 0) continue;
            const auto pointAt = [&](float distance) {
                const float share = distance / length;
                return Vector2{from.x + (to.x - from.x) * share, from.y + (to.y - from.y) * share};
            };
            float walked = 0;
            while (walked < length) {
                const bool inDash = phase < dash;
                const float step = std::min(length - walked, (inDash ? dash : period) - phase);
                if (inDash) DrawLineEx(pointAt(walked), pointAt(walked + step), thickness, color);
                walked += step;
                phase = std::fmod(phase + step, period);
            }
        }
        DrawCircleV(points.front(), thickness + ui, color);
    }
}
