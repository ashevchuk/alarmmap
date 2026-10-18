#pragma once

#include <stdexcept>
#include <string>

namespace alarmmap {

// The site's front door rejects requests that do not look like a browser.
inline constexpr const char* kBrowserUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0 Safari/537.36";

// A response other than 200, so callers can tell a refusal (4xx) from a
// server having a moment (5xx).
class HttpError : public std::runtime_error {
public:
    HttpError(const std::string& message, long status) : std::runtime_error(message), status_(status) {}
    long status() const { return status_; }

private:
    long status_;
};

// Returns the response body; throws HttpError on a status other than 200
// and std::runtime_error when the request fails.
std::string httpGet(const std::string& url);

}  // namespace alarmmap
