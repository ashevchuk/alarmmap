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

struct HeaderListDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

size_t appendToString(char* data, size_t size, size_t count, void* userData) {
    static_cast<std::string*>(userData)->append(data, size * count);
    return size * count;
}

}  // namespace

HttpClient::HttpClient() : curl_(curl_easy_init()) {
    if (!curl_) throw std::runtime_error("curl_easy_init failed");
    // An empty file name turns on the in-memory cookie store.
    curl_easy_setopt(curl_, CURLOPT_COOKIEFILE, "");
}

HttpClient::~HttpClient() { curl_easy_cleanup(curl_); }

std::string HttpClient::get(const std::string& url) { return perform(url, nullptr, ""); }

std::string HttpClient::postJson(const std::string& url, const std::string& body,
                                 const std::string& bearerToken) {
    return perform(url, &body, bearerToken);
}

std::string HttpClient::perform(const std::string& url, const std::string* jsonBody,
                                const std::string& bearerToken) {
    std::unique_ptr<curl_slist, HeaderListDeleter> headers;
    if (jsonBody) headers.reset(curl_slist_append(headers.release(), "Content-Type: application/json"));
    if (!bearerToken.empty()) {
        headers.reset(curl_slist_append(headers.release(), ("Authorization: Bearer " + bearerToken).c_str()));
    }

    std::string body;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_USERAGENT, kBrowserUserAgent);
    curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_LIMIT, kLowSpeedBytesPerSecond);
    curl_easy_setopt(curl_, CURLOPT_LOW_SPEED_TIME, kLowSpeedSeconds);
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, appendToString);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &body);
    if (jsonBody) {
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonBody->c_str());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(jsonBody->size()));
    } else {
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
    }
    const CURLcode code = curl_easy_perform(curl_);
    const std::string what = std::string(jsonBody ? "POST " : "GET ") + url;
    if (code != CURLE_OK) throw std::runtime_error(what + ": " + curl_easy_strerror(code));

    long status = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) throw HttpError(what + " returned HTTP " + std::to_string(status), status);
    return body;
}

std::string httpGet(const std::string& url) { return HttpClient().get(url); }

}  // namespace alarmmap
