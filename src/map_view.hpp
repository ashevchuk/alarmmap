#pragma once

#include "projection.hpp"

namespace alarmmap {

// Pan and zoom state of the map. Zoom 1 fits the whole map plane into the
// viewport; the centre is a point on the map plane shown in the middle of
// the viewport. Screen coordinates are pixels from the top-left corner.
class MapView {
public:
    static constexpr double kMinZoom = 1;
    // Deepest zoom in 256-pixel tile zoom. map.ukrainealarm.com caps MapLibre
    // at zoom 13, and MapLibre zooms count 512-pixel tiles, one level lower.
    static constexpr double kMaxTileZoom = 14;
    // From this zoom on the centre may reach any point of the map plane.
    static constexpr double kFreePanZoom = 2;
    static constexpr double kMarginPx = 24;

    // pixelsPerDip: screen pixels per density-independent pixel, the unit
    // web map zoom levels are defined in.
    void setViewport(double width, double height, double pixelsPerDip = 1);
    double width() const { return width_; }
    double height() const { return height_; }

    double zoom() const { return zoom_; }
    // Zoom at which the view reaches kMaxTileZoom; depends on the viewport.
    double maxZoom() const;
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
    // Screen pixels per map-plane unit at zoom 1.
    double fitPixelsPerUnit() const;

    double width_ = 1;
    double height_ = 1;
    double pixelsPerDip_ = 1;
    double zoom_ = 1;
    MapPoint center_{kMapWidth / 2, kMapHeight / 2};
};

}  // namespace alarmmap
