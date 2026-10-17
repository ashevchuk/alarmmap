#include "app_icon.hpp"

#include "render_util.hpp"

#include <rlgl.h>

#include <vector>

namespace {

constexpr int kCanvas = 256;
constexpr int kIconSizes[] = {256, 128, 64, 48, 32, 16};
constexpr float kTileInset = 8;
constexpr float kTileRoundness = 0.24f;
constexpr int kRoundSegments = 24;
constexpr float kGlowRadius = 112;
constexpr float kDroneSize = 150;
// Tilted as if in flight, nose up and to the right.
constexpr float kDroneHeadingDegrees = 35;

constexpr Color kTile = {18, 21, 27, 255};
constexpr Color kGlowCenter = {210, 40, 48, 170};
constexpr Color kGlowEdge = {210, 40, 48, 0};
constexpr Color kRim = {255, 80, 70, 230};

}  // namespace

void setWindowIcon(const SnoutIcon& drone) {
    const RenderTexture2D canvas = loadLinearTarget(kCanvas, kCanvas);
    BeginTextureMode(canvas);
    ClearBackground(BLANK);
    // Plain alpha blending would thin out the alpha channel wherever
    // translucent layers overlap; combine alpha the "over" way instead.
    rlSetBlendFactorsSeparate(RL_SRC_ALPHA, RL_ONE_MINUS_SRC_ALPHA, RL_ONE, RL_ONE_MINUS_SRC_ALPHA,
                              RL_FUNC_ADD, RL_FUNC_ADD);
    BeginBlendMode(BLEND_CUSTOM_SEPARATE);
    const Rectangle tile = {kTileInset, kTileInset, kCanvas - 2 * kTileInset, kCanvas - 2 * kTileInset};
    DrawRectangleRounded(tile, kTileRoundness, kRoundSegments, kTile);
    DrawCircleGradient({kCanvas / 2.0f, kCanvas / 2.0f}, kGlowRadius, kGlowCenter, kGlowEdge);
    DrawRectangleRoundedLinesEx(tile, kTileRoundness, kRoundSegments, 5, kRim);
    drone.draw({kCanvas / 2.0f, kCanvas / 2.0f + 4}, kDroneSize, kDroneHeadingDegrees);
    EndBlendMode();
    EndTextureMode();

    Image full = LoadImageFromTexture(canvas.texture);
    UnloadRenderTexture(canvas);
    ImageFlipVertical(&full);  // render textures are stored bottom-up
    ImageFormat(&full, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    std::vector<Image> images;
    for (const int size : kIconSizes) {
        Image image = ImageCopy(full);
        if (size != kCanvas) ImageResize(&image, size, size);
        images.push_back(image);
    }
    SetWindowIcons(images.data(), static_cast<int>(images.size()));
    for (const Image& image : images) UnloadImage(image);
    UnloadImage(full);
}
