#include "map_update.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
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

// The site marks a region yellow when its first air alert has no red level
// (mapCore.js SetHighlight).
bool isYellowAirAlert(const json& active) {
    const auto first = std::find_if(active.begin(), active.end(), isAirAlert);
    if (first == active.end()) return false;
    const json levels = first->value("activeAlertLevels", json::array());
    if (!levels.is_array()) return true;
    return std::none_of(levels.begin(), levels.end(), [](const json& level) {
        const json& name = level.is_object() && level.contains("alertLevel") ? level["alertLevel"] : json();
        return name.is_string() && trimmedUpper(name.get<std::string>()) == "RED";
    });
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

namespace {

// The payload inside its envelopes, decoded when it came as a JSON string.
json unwrapMapUpdate(const json& mapUpdate) {
    json payload = mapUpdate;
    for (int depth = 0; depth < kMaxEnvelopeDepth; ++depth) {
        if (payload.is_string()) {
            try {
                payload = json::parse(payload.get<std::string>());
            } catch (const json::parse_error&) {
                throw std::runtime_error("updateMap payload is a string but not JSON");
            }
            continue;
        }
        if (payload.is_object() && !payload.contains("alerts") && payload.contains("data")) {
            payload = json(payload.at("data"));
            continue;
        }
        break;
    }
    if (!payload.is_object() || !payload.contains("alerts") || !payload.at("alerts").is_array()) {
        throw std::runtime_error("updateMap payload has no alerts array");
    }
    return payload;
}

std::optional<std::int64_t> actionIndexOf(const json& payload) {
    const json* index = nullptr;
    if (payload.contains("actionIndex") && payload["actionIndex"].is_object() &&
        payload["actionIndex"].contains("lastActionIndex") && !payload["actionIndex"]["lastActionIndex"].is_null()) {
        index = &payload["actionIndex"]["lastActionIndex"];
    } else if (payload.contains("lastActionIndex") && !payload["lastActionIndex"].is_null()) {
        index = &payload["lastActionIndex"];
    } else if (payload.contains("lastUpdate") && !payload["lastUpdate"].is_null()) {
        index = &payload["lastUpdate"];
    }
    if (!index) return std::nullopt;
    if (index->is_number_integer()) return index->get<std::int64_t>();
    if (index->is_number_float()) {
        const double value = index->get<double>();
        if (!std::isfinite(value)) return std::nullopt;
        return static_cast<std::int64_t>(value);
    }
    if (index->is_string()) {
        const std::string& text = index->get_ref<const std::string&>();
        std::int64_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error == std::errc() && end == text.data() + text.size() && !text.empty()) return value;
    }
    return std::nullopt;
}

// Reads the regions of an unwrapped payload. A region entry that cannot be
// read is left out and counted, so one odd entry does not hide every alert.
RegionAlerts readRegions(const json& payload, int& skipped) {
    RegionAlerts alerts;
    for (const json& region : payload.at("alerts")) {
        try {
            const json& active = region.value("activeAlerts", json::array());
            if (!active.is_array() || active.empty()) continue;
            const std::string id = region.at("regionId").get<std::string>();
            const bool air = std::any_of(active.begin(), active.end(), isAirAlert);
            alerts.any.insert(id);
            if (air) alerts.air.insert(id);
            if (isYellowAirAlert(active)) alerts.yellow.insert(id);
            for (const json& alert : active) {
                const json& started = alert.is_object() && alert.contains("lastUpdate") ? alert["lastUpdate"] : json();
                if (!started.is_string()) continue;
                // An unreadable time only costs the region its "fresh alert" pulse.
                const auto startedAt = parseUtcTimestampMs(started.get<std::string>());
                if (!startedAt) continue;
                auto [entry, inserted] = alerts.startedAtMs.emplace(id, *startedAt);
                if (!inserted) entry->second = std::max(entry->second, *startedAt);
            }
        } catch (const std::exception&) {
            ++skipped;
        }
    }
    return alerts;
}

}  // namespace

AlertUpdate readAlertUpdate(const json& mapUpdate) {
    const json payload = unwrapMapUpdate(mapUpdate);
    AlertUpdate update;
    update.actionIndex = actionIndexOf(payload);
    update.alerts = readRegions(payload, update.skippedRegions);
    return update;
}

RegionAlerts activeRegionAlerts(const json& mapUpdate) { return readAlertUpdate(mapUpdate).alerts; }

}  // namespace alarmmap
