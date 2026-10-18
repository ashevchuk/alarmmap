#include "map_view.hpp"

#include <algorithm>

namespace alarmmap {

void MapView::setViewport(double width, double height) {
    width_ = std::max(1.0, width);
    height_ = std::max(1.0, height);
    clamp();
}

double MapView::pixelsPerUnit() const {
    const double fit = std::min((width_ - 2 * kMarginPx) / kMapWidth, (height_ - 2 * kMarginPx) / kMapHeight);
    return std::max(0.01, fit) * zoom_;
}

MapPoint MapView::screenToMap(MapPoint screen) const {
    const double scale = pixelsPerUnit();
    return {center_.x + (screen.x - width_ / 2) / scale, center_.y + (screen.y - height_ / 2) / scale};
}

MapPoint MapView::mapToScreen(MapPoint map) const {
    const double scale = pixelsPerUnit();
    return {width_ / 2 + (map.x - center_.x) * scale, height_ / 2 + (map.y - center_.y) * scale};
}

void MapView::zoomAt(MapPoint screenAnchor, double factor) {
    const MapPoint anchor = screenToMap(screenAnchor);
    zoom_ = std::clamp(zoom_ * factor, kMinZoom, kMaxZoom);
    const double scale = pixelsPerUnit();
    center_ = {anchor.x - (screenAnchor.x - width_ / 2) / scale,
               anchor.y - (screenAnchor.y - height_ / 2) / scale};
    clamp();
}

void MapView::panByPixels(double dx, double dy) {
    const double scale = pixelsPerUnit();
    center_ = {center_.x - dx / scale, center_.y - dy / scale};
    clamp();
}

void MapView::reset() {
    zoom_ = 1;
    center_ = {kMapWidth / 2, kMapHeight / 2};
}

void MapView::clamp() {
    zoom_ = std::clamp(zoom_, kMinZoom, kMaxZoom);
    // The centre's freedom grows from none at the minimum zoom to the whole
    // map plane at kFreePanZoom, so zooming out glides back to the middle.
    const double freedom = std::clamp((zoom_ - kMinZoom) / (kFreePanZoom - kMinZoom), 0.0, 1.0);
    const double reachX = freedom * kMapWidth / 2;
    const double reachY = freedom * kMapHeight / 2;
    center_.x = std::clamp(center_.x, kMapWidth / 2 - reachX, kMapWidth / 2 + reachX);
    center_.y = std::clamp(center_.y, kMapHeight / 2 - reachY, kMapHeight / 2 + reachY);
}

}  // namespace alarmmap
