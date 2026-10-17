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
    // Level of labelZoomForView() from which the place is shown.
    double minZoom = 0;
    MapPoint position;
};

// Reads data/cities.geojson (see tools/extract_cities.py). Throws
// std::runtime_error on malformed features.
std::vector<City> loadCities(const nlohmann::json& collection);

// Label density level of a view zoom, to compare with City::minZoom: 5.2
// for the whole country, one more per doubling of the view zoom. It is the
// scale the place thresholds were tuned on, not a web-map tile zoom (that
// would be about 6.8 for the whole country in a 1600-pixel window).
double labelZoomForView(double viewZoom);

}  // namespace alarmmap
