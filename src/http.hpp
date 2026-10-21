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

// Requests that share one cookie store, as a browser tab does.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Both return the response body; they throw HttpError on a status other
    // than 200 and std::runtime_error when the request fails.
    std::string get(const std::string& url);
    std::string postJson(const std::string& url, const std::string& body,
                         const std::string& bearerToken);

private:
    std::string perform(const std::string& url, const std::string* jsonBody,
                        const std::string& bearerToken);

    void* curl_;
};

// A single request without cookies kept.
std::string httpGet(const std::string& url);

}  // namespace alarmmap
