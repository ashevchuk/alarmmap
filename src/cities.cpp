#include "cities.hpp"

#include <cmath>
#include <stdexcept>

namespace alarmmap {

using json = nlohmann::json;

namespace {

constexpr double kLabelZoomAtCountryView = 5.2;

CityKind kindOf(const std::string& kind) {
    if (kind == "capital") return CityKind::Capital;
    if (kind == "regional") return CityKind::Regional;
    if (kind == "town") return CityKind::Town;
    if (kind == "village") return CityKind::Village;
    throw std::runtime_error("unknown city kind '" + kind + "'");
}

}  // namespace

std::vector<City> loadCities(const json& collection) {
    std::vector<City> cities;
    for (const json& feature : collection.at("features")) {
        const json& properties = feature.at("properties");
        const json& coordinates = feature.at("geometry").at("coordinates");
        if (!coordinates.is_array() || coordinates.size() < 2) {
            throw std::runtime_error("city without coordinates");
        }
        cities.push_back({properties.at("name").get<std::string>(),
                          kindOf(properties.at("kind").get<std::string>()),
                          properties.at("population").get<double>(),
                          properties.at("minZoom").get<double>(),
                          lonLatToMap(coordinates[0].get<double>(), coordinates[1].get<double>())});
    }
    return cities;
}

double labelZoomForView(double viewZoom) { return kLabelZoomAtCountryView + std::log2(viewZoom); }

}  // namespace alarmmap
