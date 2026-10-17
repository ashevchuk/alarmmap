#include "projection.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace alarmmap {

namespace {

constexpr double kWest = 22.137059;
constexpr double kEast = 40.2275801;
constexpr double kSouth = 44.184598;
constexpr double kNorth = 52.3791473;

double toRadians(double degrees) { return degrees * std::numbers::pi / 180; }

double mercatorY(double latitude) {
    return std::log(std::tan(std::numbers::pi / 4 + toRadians(latitude) / 2));
}

struct Fit {
    double scale;
    double offsetX;
    double offsetY;
};

Fit computeFit() {
    const double projectedWidth = toRadians(kEast - kWest);
    const double projectedHeight = mercatorY(kNorth) - mercatorY(kSouth);
    const double scale = std::min(kMapWidth / projectedWidth, kMapHeight / projectedHeight);
    return {scale, (kMapWidth - projectedWidth * scale) / 2,
            (kMapHeight - projectedHeight * scale) / 2};
}

const Fit kFit = computeFit();

}  // namespace

MapPoint lonLatToMap(double longitude, double latitude) {
    return {kFit.offsetX + toRadians(longitude - kWest) * kFit.scale,
            kFit.offsetY + (mercatorY(kNorth) - mercatorY(latitude)) * kFit.scale};
}

MapPoint normalizedToMap(double x, double y) { return {x * kMapWidth, y * kMapHeight}; }

namespace {

constexpr double kTilePixels = 256;

double westRadians() { return toRadians(kWest); }

}  // namespace

MapPoint worldToMap(double x, double y) {
    const double longitudeRadians = x * 2 * std::numbers::pi - std::numbers::pi;
    const double mercator = std::numbers::pi * (1 - 2 * y);
    return {kFit.offsetX + (longitudeRadians - westRadians()) * kFit.scale,
            kFit.offsetY + (mercatorY(kNorth) - mercator) * kFit.scale};
}

MapPoint mapToWorld(MapPoint point) {
    const double longitudeRadians = (point.x - kFit.offsetX) / kFit.scale + westRadians();
    const double mercator = mercatorY(kNorth) - (point.y - kFit.offsetY) / kFit.scale;
    return {(longitudeRadians + std::numbers::pi) / (2 * std::numbers::pi),
            (1 - mercator / std::numbers::pi) / 2};
}

double tileZoomForScale(double pixelsPerMapUnit) {
    const double pixelsPerWorld = pixelsPerMapUnit * 2 * std::numbers::pi * kFit.scale;
    return std::log2(pixelsPerWorld / kTilePixels);
}

}  // namespace alarmmap
