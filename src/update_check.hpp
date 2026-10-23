#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace alarmmap {

inline constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/ashevchuk/alarmmap/releases/latest";

// The page to send the user to for a release version such as "0.2.10".
std::string releasePageUrl(const std::string& version);

// The version of a GitHub "latest release" response when it is higher than
// `current`, without a leading "v". Throws std::runtime_error when either
// version is not dotted integers, so a response in an unexpected shape is
// never mistaken for "no update".
std::optional<std::string> newerReleaseVersion(const nlohmann::json& latestRelease, std::string_view current);

}  // namespace alarmmap
