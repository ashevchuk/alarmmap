#include "http.hpp"

#include <curl/curl.h>

#include <memory>
#include <stdexcept>

namespace alarmmap {

namespace {

constexpr long kConnectTimeoutSeconds = 15;
// Abort transfers that stall below this rate, instead of capping total time,
// so a large download on a slow link still completes.
constexpr long kLowSpeedBytesPerSecond = 1024;
constexpr long kLowSpeedSeconds = 20;

struct CurlDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};

size_t appendToString(char* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(data, size * count);
    return size * count;
}

}  // namespace

std::string httpGet(const std::string& url) {
    std::unique_ptr<CURL, CurlDeleter> curl(curl_easy_init());
    if (!curl) throw std::runtime_error("curl_easy_init failed");

    std::string body;
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, kBrowserUserAgent);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
    curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
    const CURLcode code = curl_easy_perform(curl.get());
    if (code != CURLE_OK) {
        throw std::runtime_error("GET " + url + ": " + curl_easy_strerror(code));
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        throw std::runtime_error("GET " + url + " returned HTTP " + std::to_string(status));
    }
    return body;
}

}  // namespace alarmmap
