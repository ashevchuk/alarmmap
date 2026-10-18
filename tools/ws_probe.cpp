// Records the map's live channels as JSON lines for format analysis and
// long-running connection tests.
//
// Usage: ws_probe [duration_seconds] [output_path]

#include "live_feed.hpp"
#include "site.hpp"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

namespace {

constexpr int kDefaultDurationSeconds = 120;
constexpr const char* kDefaultOutputPath = ".notes/ws-probe/messages.jsonl";

long long unixMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string clockTime() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char text[16];
    std::strftime(text, sizeof(text), "%H:%M:%S", std::localtime(&now));
    return text;
}

int run(int durationSeconds, const std::filesystem::path& outputPath) {
    std::filesystem::create_directories(outputPath.parent_path());
    std::ofstream output(outputPath);
    if (!output) throw std::runtime_error("cannot open " + outputPath.string());

    std::map<std::string, int> publications;
    std::map<std::string, int> events;
    alarmmap::LiveFeed feed(
        {{alarmmap::kTargetsChannel, ""}, {alarmmap::kAlertsChannel, ""}},
        [&](const std::string& channel, const json& payload) {
            ++publications[channel];
            output << json{{"receivedAt", unixMillis()}, {"channel", channel}, {"payload", payload}}
                          .dump()
                   << '\n';
            output.flush();
            std::cout << '[' << clockTime() << "] push " << channel << " bytes="
                      << payload.dump().size() << '\n';
        },
        [&](const std::string& event, const json& details) {
            ++events[event];
            output << json{{"receivedAt", unixMillis()}, {"event", event}, {"details", details}}
                          .dump()
                   << '\n';
            output.flush();
            std::cout << '[' << clockTime() << "] " << event << ' ' << details.dump() << '\n';
        });
    feed.run(std::chrono::steady_clock::now() + std::chrono::seconds(durationSeconds));

    std::cout << "\nsummary:";
    for (const auto& [event, count] : events) std::cout << ' ' << event << '=' << count;
    std::cout << '\n';
    for (const auto& [channel, count] : publications) {
        std::cout << "  " << channel << ": publications=" << count << '\n';
    }
    std::cout << "log: " << outputPath << '\n';
    return events["connected"] > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const int durationSeconds = argc > 1 ? std::stoi(argv[1]) : kDefaultDurationSeconds;
    const std::filesystem::path outputPath = argc > 2 ? argv[2] : kDefaultOutputPath;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int exitCode = 1;
    try {
        exitCode = run(durationSeconds, outputPath);
    } catch (const std::exception& error) {
        std::cerr << "ws_probe: " << error.what() << '\n';
    }
    curl_global_cleanup();
    return exitCode;
}
