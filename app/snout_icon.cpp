#include "snout_icon.hpp"

#include <cmath>
#include <stdexcept>

using alarmmap::TargetKind;

namespace {

// Drawn large and shrunk with mipmaps, which smooths the edges. All shapes
// below use canvas coordinates with the nose pointing up.
constexpr int kCanvas = 256;
constexpr float kCenterX = kCanvas / 2.0f;
constexpr float kOutlinePx = 7;
constexpr int kOutlineDirections = 16;

constexpr Color kOutline = {28, 10, 18, 255};
constexpr Color kBody = {250, 250, 250, 255};
constexpr Color kDetail = {190, 190, 196, 255};
constexpr Color kSnout = {255, 255, 255, 255};
constexpr Color kNostril = {40, 30, 36, 255};
constexpr Color kFlameOuter = {255, 150, 40, 255};
constexpr Color kFlameInner = {255, 230, 120, 255};
constexpr Color kTrack = {120, 120, 128, 255};

Vector2 mirrored(Vector2 point) { return {2 * kCenterX - point.x, point.y}; }

// Quad on the left side and its mirror image on the right.
void drawMirroredQuad(Vector2 a, Vector2 b, Vector2 c, Vector2 d, Color color) {
    DrawTriangle(a, b, c, color);
    DrawTriangle(a, c, d, color);
    DrawTriangle(mirrored(a), mirrored(b), mirrored(c), color);
    DrawTriangle(mirrored(a), mirrored(c), mirrored(d), color);
}

void drawSnout(float centerY, float radius) {
    const int x = static_cast<int>(kCenterX);
    const int y = static_cast<int>(centerY);
    const float nostrilOffset = radius * 0.38f;
    DrawCircle(x, y, radius, kDetail);
    DrawCircle(x, y, radius * 0.83f, kSnout);
    DrawEllipse(static_cast<int>(kCenterX - nostrilOffset), y + 2, radius * 0.19f, radius * 0.35f,
                kNostril);
    DrawEllipse(static_cast<int>(kCenterX + nostrilOffset), y + 2, radius * 0.19f, radius * 0.35f,
                kNostril);
}

void drawFlame(float nozzleY, float halfWidth, float length) {
    DrawTriangle({kCenterX - halfWidth, nozzleY}, {kCenterX, nozzleY + length},
                 {kCenterX + halfWidth, nozzleY}, kFlameOuter);
    DrawTriangle({kCenterX - halfWidth * 0.55f, nozzleY}, {kCenterX, nozzleY + length * 0.6f},
                 {kCenterX + halfWidth * 0.55f, nozzleY}, kFlameInner);
}

void drawPlane() {
    drawMirroredQuad({128, 100}, {22, 170}, {38, 188}, {128, 152}, kBody);  // swept wings
    drawMirroredQuad({128, 196}, {82, 236}, {96, 244}, {128, 228}, kBody);  // tailplane
    DrawEllipse(128, 156, 22, 88, kBody);
    drawSnout(76, 48);
}

// Shahed-style delta wing with tip winglets.
void drawDeltaWing() {
    DrawTriangle({128, 72}, {18, 204}, {238, 204}, kBody);
    DrawRectangle(14, 168, 12, 44, kDetail);
    DrawRectangle(230, 168, 12, 44, kDetail);
    DrawEllipse(128, 150, 20, 82, kBody);
}

void drawDrone() {
    drawDeltaWing();
    DrawEllipse(128, 236, 40, 7, kDetail);  // pusher propeller disc
    drawSnout(74, 40);
}

void drawJetDrone() {
    drawFlame(220, 16, 34);
    drawDeltaWing();
    drawSnout(74, 40);
}

void drawCruiseMissile() {
    drawFlame(212, 12, 36);
    DrawRectangle(52, 120, 152, 16, kBody);                                  // short straight wings
    drawMirroredQuad({128, 176}, {94, 206}, {94, 216}, {128, 204}, kBody);  // tail fins
    DrawRectangle(112, 60, 32, 156, kBody);
    drawSnout(60, 32);
}

void drawBallisticMissile() {
    drawFlame(214, 22, 40);
    drawMirroredQuad({128, 160}, {80, 204}, {80, 222}, {128, 196}, kBody);  // stabilisers
    DrawRectangle(104, 62, 48, 154, kBody);
    drawSnout(60, 40);
}

// Glide bomb: fat body with long straight deployable wings and a cross tail.
void drawGuidedBomb() {
    DrawRectangle(20, 116, 216, 18, kBody);
    drawMirroredQuad({128, 192}, {84, 224}, {84, 240}, {128, 230}, kBody);
    DrawEllipse(128, 148, 36, 84, kBody);
    DrawRectangle(98, 120, 60, 10, kDetail);  // wing hinge
    drawSnout(76, 42);
}

void drawHelicopter() {
    DrawRectangle(120, 160, 16, 76, kBody);   // tail boom
    DrawRectangle(98, 228, 60, 10, kDetail);  // tail rotor
    DrawEllipse(128, 140, 38, 54, kBody);
    drawSnout(80, 36);
    // Main rotor, drawn last so it reads as being on top.
    for (const float angle : {45.0f, -45.0f}) {
        DrawRectanglePro({128, 146, 236, 12}, {118, 6}, angle, kDetail);
    }
}

void drawGround() {
    DrawRectangle(62, 76, 22, 160, kTrack);
    DrawRectangle(172, 76, 22, 160, kTrack);
    DrawRectangle(80, 84, 96, 146, kBody);
    drawSnout(84, 42);
}

void drawSilhouette(TargetKind kind) {
    switch (kind) {
        case TargetKind::Plane: return drawPlane();
        case TargetKind::Drone: return drawDrone();
        case TargetKind::JetDrone: return drawJetDrone();
        case TargetKind::CruiseMissile: return drawCruiseMissile();
        case TargetKind::BallisticMissile: return drawBallisticMissile();
        case TargetKind::GuidedBomb: return drawGuidedBomb();
        case TargetKind::Helicopter: return drawHelicopter();
        case TargetKind::Ground: return drawGround();
        // The site draws unidentified targets with its Shahed icon.
        case TargetKind::Unknown: return drawDrone();
    }
    throw std::logic_error("unhandled target kind");
}

Rectangle flippedCanvas() {
    // Render textures are stored bottom-up; a negative height draws them upright.
    return {0, 0, static_cast<float>(kCanvas), -static_cast<float>(kCanvas)};
}

}  // namespace

SnoutIcon::SnoutIcon(TargetKind kind) {
    // The dark outline keeps the light icon readable over any background:
    // the silhouette is stamped in dark around itself, then drawn on top.
    const RenderTexture2D silhouette = LoadRenderTexture(kCanvas, kCanvas);
    texture_ = LoadRenderTexture(kCanvas, kCanvas);
    if (!IsRenderTextureValid(silhouette) || !IsRenderTextureValid(texture_)) {
        throw std::runtime_error("cannot create target icon texture");
    }
    BeginTextureMode(silhouette);
    ClearBackground(BLANK);
    drawSilhouette(kind);
    EndTextureMode();

    BeginTextureMode(texture_);
    ClearBackground(BLANK);
    for (int i = 0; i < kOutlineDirections; ++i) {
        const float angle = 2 * PI * i / kOutlineDirections;
        DrawTextureRec(silhouette.texture, flippedCanvas(),
                       {kOutlinePx * std::cos(angle), kOutlinePx * std::sin(angle)}, kOutline);
    }
    DrawTextureRec(silhouette.texture, flippedCanvas(), {0, 0}, WHITE);
    EndTextureMode();
    UnloadRenderTexture(silhouette);

    GenTextureMipmaps(&texture_.texture);
    SetTextureFilter(texture_.texture, TEXTURE_FILTER_TRILINEAR);
}

SnoutIcon::~SnoutIcon() { UnloadRenderTexture(texture_); }

void SnoutIcon::draw(Vector2 screenCenter, float sizePx, float headingDegrees, Color tint) const {
    DrawTexturePro(texture_.texture, flippedCanvas(), {screenCenter.x, screenCenter.y, sizePx, sizePx},
                   {sizePx / 2, sizePx / 2}, headingDegrees, tint);
}
