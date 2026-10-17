#pragma once

#include "projection.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace alarmmap {

enum class CityKind { Capital, Regional, Town };

struct City {
    std::string name;
    CityKind kind = CityKind::Town;
    double population = 0;
    // Web-map zoom level from which Natural Earth suggests showing the place.
    double minZoom = 0;
    MapPoint position;
};

// Reads data/cities.geojson (see tools/extract_cities.py). Throws
// std::runtime_error on malformed features.
std::vector<City> loadCities(const nlohmann::json& collection);

// Web-map zoom level equivalent to a view zoom, to compare with City::minZoom.
// The whole country in a window corresponds to about web zoom 5.2, and
// every doubling of the view zoom adds one level.
double webZoomForView(double viewZoom);

}  // namespace alarmmap
