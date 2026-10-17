#pragma once

#include "projection.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace alarmmap {

enum class CityKind { Capital, Regional, Town, Village };

struct City {
    std::string name;
    CityKind kind = CityKind::Town;
    double population = 0;
    // Web map tile zoom (see tileZoomForScale) from which the place is shown.
    double minZoom = 0;
    MapPoint position;
};

// Reads data/cities.geojson (see tools/extract_cities.py). Throws
// std::runtime_error on malformed features.
std::vector<City> loadCities(const nlohmann::json& collection);

}  // namespace alarmmap
