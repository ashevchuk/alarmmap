#pragma once

// What the app takes from map.ukrainealarm.com, in one place, so that
// tools/api_check checks exactly what the app uses. The live feed's own
// page and WebSocket addresses live in live_feed.cpp.

namespace alarmmap {

// The targets channel is private and its Centrifugo name is given by the map
// page, so this is only the name the app uses for it.
inline constexpr const char* kTargetsChannel = "targets";
inline constexpr const char* kAlertsChannel = "updateMap";
inline constexpr const char* kAlertsSnapshotUrl = "https://map.ukrainealarm.com/api/v2/data/mapUpdate";
// Also in tools/geometry/download.cmake, which refreshes data/ from it.
inline constexpr const char* kRegionsUrl = "https://map.ukrainealarm.com/data/real-map/regions-standard-v1.geojson";

}  // namespace alarmmap
