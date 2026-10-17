#pragma once

#include <string>

namespace alarmmap {

// The site's front door rejects requests that do not look like a browser.
inline constexpr const char* kBrowserUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0 Safari/537.36";

// Returns the response body; throws std::runtime_error unless HTTP 200.
std::string httpGet(const std::string& url);

}  // namespace alarmmap
