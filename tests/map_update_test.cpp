// Usage: map_update_test <tests/data directory>
//
// map_update.json is a recorded response of the site's HTTP snapshot
// (api/v2/data/mapUpdate), the only alerts source the app polls.

#include "map_update.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <string>

using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool condition, const std::string& what,
           std::source_location location = std::source_location::current()) {
    if (condition) return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
}

json readJson(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    return json::parse(input);
}

void testRecordedSnapshot(const std::filesystem::path& dataDirectory) {
    const json snapshot = readJson(dataDirectory / "map_update.json");
    const alarmmap::AlertUpdate update = alarmmap::readAlertUpdate(snapshot);
    check(update.skippedRegions == 0, "every region entry of the snapshot is readable");
    check(update.actionIndex == snapshot.at("actionIndex").at("lastActionIndex").get<std::int64_t>(),
          "the action index is the one the snapshot carries");
    check(update.alerts.any.size() == snapshot.at("alerts").size(), "every listed region has an alert");
    check(update.alerts.any.contains("9999"), "Crimea, the region with the oldest alert, is present");
    check(!update.alerts.air.empty() && update.alerts.air.size() <= update.alerts.any.size(),
          "air alerts are a non-empty subset of all alerts");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: map_update_test <data directory>\n";
        return 2;
    }
    try {
        testRecordedSnapshot(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
