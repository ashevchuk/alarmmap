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

}  // namespace alarmmap
