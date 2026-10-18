// Checks that map.ukrainealarm.com still serves what the app understands.
//
// Usage: api_check [duration_seconds] [failures_file]
//
// Connects the way the app does and runs every publication and snapshot
// through the app's own parsers for the given time, then downloads the
// region geometry, checks that tools/geometry can still prepare it and
// compares it with the copy in data/. Payloads that fail to parse are saved
// to failures_file (default api-check-failures.jsonl) for analysis.
//
// Exit status: 0 when everything parses, 1 when the site changed in a way
// the app does not understand, 2 when only data/regions-standard-v1.geojson
// is out of date.

#include "http.hpp"
#include "live_feed.hpp"
#include "map_update.hpp"
#include "region_geometry.hpp"
#include "region_store.hpp"
#include "site.hpp"
#include "target_snapshot.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <exception>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr int kDefaultDurationSeconds = 90;
constexpr const char* kDefaultFailuresFile = "api-check-failures.jsonl";
constexpr std::size_t kExamplesShown = 10;
constexpr std::size_t kPayloadShownBytes = 300;

struct ChannelResult {
    int parsed = 0;
    int skipped = 0;
    int failed = 0;
};

class FeedCheck {
public:
    explicit FeedCheck(std::string failuresFile) : failuresFile_(std::move(failuresFile)) {}

    void onPublication(const std::string& channel, const json& payload) {
        ChannelResult& result = channels_[channel];
        try {
            if (channel == alarmmap::kTargetsChannel) {
                if (reader_.read(payload)) {
                    ++result.parsed;
                } else {
                    ++result.skipped;  // an older or repeated snapshot, as the app drops it
                }
            } else if (channel == alarmmap::kAlertsChannel) {
                alarmmap::activeRegionAlerts(payload);
                ++result.parsed;
            } else {
                throw std::runtime_error("publication on an unexpected channel");
            }
        } catch (const std::exception& error) {
            ++result.failed;
            const std::string text = payload.dump();
            std::cout << "FAILED " << channel << ": " << error.what() << "\n  payload: "
                      << text.substr(0, kPayloadShownBytes) << (text.size() > kPayloadShownBytes ? "..." : "")
                      << '\n';
            std::ofstream failures(failuresFile_, std::ios::app);
            failures << json{{"channel", channel}, {"error", error.what()}, {"payload", payload}}.dump() << '\n';
            if (!failures) std::cerr << "api_check: cannot write " << failuresFile_ << '\n';
            savedFailures_ = true;
        }
    }

    void onEvent(const std::string& event, const json& details) {
        ++events_[event];
        if (event != "connected" && event != "session_start") {
            std::cout << "event " << event << ' ' << details.dump() << '\n';
        }
    }

    // Prints the result; returns whether the feed works as the app expects.
    bool report() const {
        bool ok = true;
        const auto count = [](const auto& map, const std::string& key) {
            const auto found = map.find(key);
            return found == map.end() ? 0 : found->second;
        };
        if (count(events_, "connected") == 0) {
            std::cout << "feed: never connected\n";
            ok = false;
        }
        for (const char* event : {"snapshot_error", "handler_error"}) {
            if (count(events_, event) > 0) {
                std::cout << "feed: " << event << " x" << count(events_, event) << '\n';
                ok = false;
            }
        }
        for (const char* channel : {alarmmap::kTargetsChannel, alarmmap::kAlertsChannel}) {
            const auto found = channels_.find(channel);
            const ChannelResult result = found == channels_.end() ? ChannelResult{} : found->second;
            std::cout << "channel " << channel << ": parsed " << result.parsed << ", skipped " << result.skipped
                      << ", failed " << result.failed << '\n';
            // Every channel delivers at least its REST snapshot right after subscribing.
            if (result.parsed == 0 || result.failed > 0) ok = false;
        }
        if (savedFailures_) std::cout << "failed payloads saved to " << failuresFile_ << '\n';
        return ok;
    }

private:
    std::string failuresFile_;
    alarmmap::TargetSnapshotReader reader_;
    std::map<std::string, ChannelResult> channels_;
    std::map<std::string, int> events_;
    bool savedFailures_ = false;
};

struct RegionInfo {
    std::string name;
    int rank = 0;
    json geometry;
};

std::map<std::string, RegionInfo> regionsById(const json& collection) {
    std::map<std::string, RegionInfo> regions;
    for (const json& feature : collection.at("features")) {
        const json& properties = feature.at("properties");
        regions[properties.at("regionId").get<std::string>()] = {properties.value("name", ""),
                                                                  properties.at("rank").get<int>(),
                                                                  feature.at("geometry")};
    }
    return regions;
}

void printExamples(const char* title, const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    std::cout << title << ": " << lines.size() << '\n';
    for (std::size_t i = 0; i < lines.size() && i < kExamplesShown; ++i) std::cout << "  " << lines[i] << '\n';
    if (lines.size() > kExamplesShown) std::cout << "  ...\n";
}

// Returns 0 when the live geometry matches data/, 2 when it differs, 1 when
// tools/geometry could not prepare it.
int checkGeometry() {
    json live;
    try {
        live = json::parse(alarmmap::httpGet(alarmmap::kRegionsUrl));
        json prepared = live;
        std::ifstream crimeaFile(std::string(ALARMMAP_DATA_DIR) + "/crimea.geojson");
        alarmmap::overrideRegionGeometry(prepared, json::parse(crimeaFile));
        const alarmmap::PreparedRegions regions{alarmmap::buildRegionShapes(prepared, alarmmap::kCoarseTolerance),
                                                alarmmap::buildRegionShapes(prepared, alarmmap::kDetailedTolerance)};
        alarmmap::readPreparedRegions(alarmmap::writePreparedRegions(regions));
        std::cout << "geometry: " << regions.coarse.size() << " regions prepare as before\n";
    } catch (const std::exception& error) {
        std::cout << "geometry: FAILED: " << error.what() << '\n';
        return 1;
    }

    std::ifstream bundledFile(std::string(ALARMMAP_DATA_DIR) + "/regions-standard-v1.geojson");
    const auto bundled = regionsById(json::parse(bundledFile));
    const auto current = regionsById(live);
    std::vector<std::string> added, removed, renamed, reranked, reshaped;
    for (const auto& [id, region] : current) {
        const auto old = bundled.find(id);
        if (old == bundled.end()) {
            added.push_back(id + " " + region.name);
            continue;
        }
        if (old->second.name != region.name) renamed.push_back(id + " " + old->second.name + " -> " + region.name);
        if (old->second.rank != region.rank) reranked.push_back(id + " " + region.name);
        if (old->second.geometry != region.geometry) reshaped.push_back(id + " " + region.name);
    }
    for (const auto& [id, region] : bundled) {
        if (!current.contains(id)) removed.push_back(id + " " + region.name);
    }
    if (added.empty() && removed.empty() && renamed.empty() && reranked.empty() && reshaped.empty()) {
        std::cout << "geometry: data/regions-standard-v1.geojson is current\n";
        return 0;
    }
    printExamples("regions added on the site", added);
    printExamples("regions removed from the site", removed);
    printExamples("regions renamed", renamed);
    printExamples("regions with a new rank", reranked);
    printExamples("regions with new outlines", reshaped);
    std::cout << "geometry: data/ is out of date; refresh it with\n"
                 "  cmake -DOUTPUT=data/regions-standard-v1.geojson -P tools/geometry/download.cmake\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 3) {
        std::cerr << "usage: api_check [duration_seconds] [failures_file]\n";
        return 1;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int status = 1;
    try {
        const int duration = argc > 1 ? std::stoi(argv[1]) : kDefaultDurationSeconds;
        FeedCheck feed(argc > 2 ? argv[2] : kDefaultFailuresFile);
        alarmmap::LiveFeed live(
            {{alarmmap::kTargetsChannel, alarmmap::kTargetsSnapshotUrl},
             {alarmmap::kAlertsChannel, alarmmap::kAlertsSnapshotUrl}},
            [&feed](const std::string& channel, const json& payload) { feed.onPublication(channel, payload); },
            [&feed](const std::string& event, const json& details) { feed.onEvent(event, details); });
        std::cout << "listening to the live feed for " << duration << " s\n";
        live.run(std::chrono::steady_clock::now() + std::chrono::seconds(duration));
        const bool feedOk = feed.report();
        const int geometry = checkGeometry();
        status = !feedOk || geometry == 1 ? 1 : geometry;
        std::cout << (status == 0   ? "OK: the site serves what the app understands\n"
                      : status == 2 ? "OK, but the bundled region geometry is out of date\n"
                                    : "BROKEN: the site changed in a way the app does not understand\n");
    } catch (const std::exception& error) {
        std::cerr << "api_check: " << error.what() << '\n';
    }
    curl_global_cleanup();
    return status;
}
