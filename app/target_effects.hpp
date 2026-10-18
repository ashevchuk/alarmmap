#pragma once

#include <raylib.h>

#include <cstdint>
#include <string_view>
#include <vector>

// Screen-space animations around target markers. Ages are seconds since the
// effect started; `seed` varies the random-looking details between targets.

inline constexpr double kTeleportSeconds = 1.2;

// Stable per-target seed for the effects.
std::uint32_t effectSeed(std::string_view trackId);

// Fireball, shock wave and sparks; drawn over the markers.
void drawExplosion(Vector2 center, double age, std::uint32_t seed);

inline constexpr double kMushroomSeconds = 4.2;

// Nuclear mushroom cloud rising from `ground`: flash, fireball, stem, a cap
// that burns down to smoke and a shock wave running along the ground.
void drawMushroom(Vector2 ground, double age, std::uint32_t seed);

// Smouldering mark left by an explosion; drawn under the markers. Fades out
// by `endsAt`, the age at which the mark is gone.
void drawScorch(Vector2 center, double age, double endsAt, std::uint32_t seed);

// Beam of light and converging rings that bring a new marker in.
void drawTeleport(Vector2 center, double age, std::uint32_t seed);

// Size factor of a marker's icon while it materialises.
float teleportIconScale(double age);

// Dashed polyline with a dark outline, the dash pattern running on across
// the vertices.
void drawDashedPath(const std::vector<Vector2>& points, float opacity);
