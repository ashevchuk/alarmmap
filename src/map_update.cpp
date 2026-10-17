#include "map_update.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace alarmmap {

using json = nlohmann::json;

namespace {

constexpr int kMaxEnvelopeDepth = 4;
// Numeric alert type the API uses for air alerts besides the string "AIR".
constexpr int kAirAlertTypeCode = 1;

std::string trimmedUpper(std::string text) {
    const auto isSpace = [](unsigned char symbol) { return std::isspace(symbol) != 0; };
    text.erase(text.begin(), std::find_if_not(text.begin(), text.end(), isSpace));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), isSpace).base(), text.end());
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char symbol) { return static_cast<char>(std::toupper(symbol)); });
    return text;
}

bool isAirAlert(const json& alert) {
    const json& type = alert.contains("type") ? alert["type"] : json();
    if (type.is_number()) return type.get<double>() == kAirAlertTypeCode;
    return type.is_string() && trimmedUpper(type.get<std::string>()) == "AIR";
}

std::optional<int> readNumber(std::string_view text, size_t offset, size_t length) {
    int value = 0;
    const char* first = text.data() + offset;
    const auto [end, error] = std::from_chars(first, first + length, value);
    if (error != std::errc() || end != first + length) return std::nullopt;
    return value;
}

}  // namespace

std::optional<std::int64_t> parseUtcTimestampMs(std::string_view text) {
    // Fixed layout up to the seconds: YYYY-MM-DDTHH:MM:SS
    constexpr size_t kSecondsEnd = 19;
    if (text.size() < kSecondsEnd + 1 || text.back() != 'Z' || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    const auto year = readNumber(text, 0, 4), month = readNumber(text, 5, 2), day = readNumber(text, 8, 2);
    const auto hour = readNumber(text, 11, 2), minute = readNumber(text, 14, 2),
               second = readNumber(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second) return std::nullopt;

    int milliseconds = 0;
    const std::string_view fraction = text.substr(kSecondsEnd, text.size() - kSecondsEnd - 1);
    if (!fraction.empty()) {
        if (fraction.front() != '.' || fraction.size() < 2) return std::nullopt;
        for (size_t i = 1; i < fraction.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(fraction[i]))) return std::nullopt;
            if (i <= 3) milliseconds = milliseconds * 10 + (fraction[i] - '0');
        }
        for (size_t i = fraction.size(); i <= 3; ++i) milliseconds *= 10;
    }

    const std::chrono::year_month_day date{std::chrono::year(*year), std::chrono::month(*month),
                                           std::chrono::day(*day)};
    if (!date.ok() || *hour > 23 || *minute > 59 || *second > 60) return std::nullopt;
    const auto time = std::chrono::sys_days(date) + std::chrono::hours(*hour) +
                      std::chrono::minutes(*minute) + std::chrono::seconds(*second) +
                      std::chrono::milliseconds(milliseconds);
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

RegionAlerts activeRegionAlerts(const json& mapUpdate) {
    const json* payload = &mapUpdate;
    for (int depth = 0; depth < kMaxEnvelopeDepth && payload->is_object() &&
                        !payload->contains("alerts") && payload->contains("data");
         ++depth) {
        payload = &payload->at("data");
    }
    if (!payload->is_object() || !payload->contains("alerts") || !payload->at("alerts").is_array()) {
        throw std::runtime_error("updateMap payload has no alerts array");
    }

    RegionAlerts alerts;
    for (const json& region : payload->at("alerts")) {
        const json& active = region.value("activeAlerts", json::array());
        if (!active.is_array() || active.empty()) continue;
        const std::string id = region.at("regionId").get<std::string>();
        alerts.any.insert(id);
        if (std::any_of(active.begin(), active.end(), isAirAlert)) alerts.air.insert(id);
        for (const json& alert : active) {
            const json& started = alert.contains("lastUpdate") ? alert["lastUpdate"] : json();
            if (!started.is_string()) continue;
            // An unreadable time only costs the region its "fresh alert" pulse.
            const auto startedAt = parseUtcTimestampMs(started.get<std::string>());
            if (!startedAt) continue;
            auto [entry, inserted] = alerts.startedAtMs.emplace(id, *startedAt);
            if (!inserted) entry->second = std::max(entry->second, *startedAt);
        }
    }
    return alerts;
}

}  // namespace alarmmap
