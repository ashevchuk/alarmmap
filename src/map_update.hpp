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

struct AlertUpdate {
    RegionAlerts alerts;
    // Orders updates: one with a lower index is older than the state shown.
    // Absent when the payload carries no index the site would read.
    std::optional<std::int64_t> actionIndex;
    // Entries of the alerts array that could not be read and were left out.
    int skippedRegions = 0;
};

// Reads an `updateMap` payload, optionally still inside Centrifugo's
// {data: ...} envelopes or sent as a JSON string, the way the site does
// (mapCore.js ApplyMapUpdate), the action index taken from
// actionIndex.lastActionIndex, lastActionIndex or lastUpdate. It degrades
// where the site rejects: a missing index or an unreadable region entry
// still yields the alerts that could be read. Throws std::runtime_error only
// without an alerts array.
AlertUpdate readAlertUpdate(const nlohmann::json& mapUpdate);

// The alerts of readAlertUpdate alone.
RegionAlerts activeRegionAlerts(const nlohmann::json& mapUpdate);

}  // namespace alarmmap
