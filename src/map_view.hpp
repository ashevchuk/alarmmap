#pragma once

#include "projection.hpp"

namespace alarmmap {

// Pan and zoom state of the map. Zoom 1 fits the whole map plane into the
// viewport; the centre is a point on the map plane shown in the middle of
// the viewport. Screen coordinates are pixels from the top-left corner.
class MapView {
public:
    static constexpr double kMinZoom = 1;
    static constexpr double kMaxZoom = 48;
    // From this zoom on the centre may reach any point of the map plane.
    static constexpr double kFreePanZoom = 2;
    static constexpr double kMarginPx = 24;

    void setViewport(double width, double height);
    double width() const { return width_; }
    double height() const { return height_; }

    double zoom() const { return zoom_; }
    MapPoint center() const { return center_; }
    // Screen pixels per map-plane unit.
    double pixelsPerUnit() const;

    MapPoint screenToMap(MapPoint screen) const;
    MapPoint mapToScreen(MapPoint map) const;

    // Zooms by `factor`, keeping the map point under `screenAnchor` in place.
    void zoomAt(MapPoint screenAnchor, double factor);
    void panByPixels(double dx, double dy);
    void reset();

    bool operator==(const MapView&) const = default;

private:
    // Keeps the zoom in range and the centre over the map plane, so the
    // country can never be dragged out of sight.
    void clamp();

    double width_ = 1;
    double height_ = 1;
    double zoom_ = 1;
    MapPoint center_{kMapWidth / 2, kMapHeight / 2};
};

}  // namespace alarmmap
