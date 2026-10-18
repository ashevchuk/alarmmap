// Usage: alarmmap_geometry SITE_REGIONS_GEOJSON CRIMEA_GEOJSON OUTPUT
//
// Applies the Crimea outline to the site's regions, simplifies and
// triangulates them at both detail levels and writes the prepared regions
// file.

#include "region_geometry.hpp"
#include "region_store.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

nlohmann::json readJson(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("cannot open ") + path);
    return nlohmann::json::parse(input);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: alarmmap_geometry SITE_REGIONS_GEOJSON CRIMEA_GEOJSON OUTPUT\n";
        return 2;
    }
    try {
        nlohmann::json regions = readJson(argv[1]);
        alarmmap::overrideRegionGeometry(regions, readJson(argv[2]));
        alarmmap::PreparedRegions prepared{alarmmap::buildRegionShapes(regions, alarmmap::kCoarseTolerance),
                                           alarmmap::buildRegionShapes(regions, alarmmap::kDetailedTolerance)};
        const std::string bytes = alarmmap::writePreparedRegions(prepared);
        // The app trusts this file, so it must read back here first.
        const alarmmap::PreparedRegions check = alarmmap::readPreparedRegions(bytes);
        if (check.coarse.size() != prepared.coarse.size() || check.detailed.size() != prepared.detailed.size()) {
            throw std::runtime_error("prepared regions do not read back");
        }
        const std::string partial = std::string(argv[3]) + ".partial";
        {
            std::ofstream output(partial, std::ios::binary);
            output << bytes;
            if (!output) throw std::runtime_error("cannot write " + partial);
        }
        if (std::rename(partial.c_str(), argv[3]) != 0) {
            throw std::runtime_error("cannot replace " + std::string(argv[3]));
        }
        std::cout << "regions=" << prepared.coarse.size() << " bytes=" << bytes.size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "alarmmap_geometry: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
