#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace alarmmap {

struct RegionAlerts {
    // Regions with at least one active alert of any type.
    std::set<std::string> any;
    // Regions with an active air alert; the site shows targets only there.
    std::set<std::string> air;
    // Unix milliseconds of the most recent alert start per region, where the
    // payload carries a readable time.
    std::map<std::string, std::int64_t> startedAtMs;

    bool operator==(const RegionAlerts&) const = default;
};

// Parses "YYYY-MM-DDTHH:MM:SS[.fraction]Z" into Unix milliseconds.
std::optional<std::int64_t> parseUtcTimestampMs(std::string_view text);

// Reads an `updateMap` payload, optionally still inside Centrifugo's
// {data: ...} envelopes. Throws std::runtime_error without an alerts array.
RegionAlerts activeRegionAlerts(const nlohmann::json& mapUpdate);

}  // namespace alarmmap
