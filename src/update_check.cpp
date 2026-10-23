#include "update_check.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace alarmmap {

namespace {

constexpr std::string_view kReleasePagePrefix = "https://github.com/ashevchuk/alarmmap/releases/tag/v";
constexpr size_t kMaxComponentDigits = 9;

std::vector<std::uint64_t> parseVersion(std::string_view text) {
    if (text.starts_with('v')) text.remove_prefix(1);
    std::vector<std::uint64_t> components;
    while (true) {
        const size_t end = text.find('.');
        const std::string_view part = text.substr(0, end);
        std::uint64_t value = 0;
        const auto [parsedEnd, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > kMaxComponentDigits || error != std::errc() ||
            parsedEnd != part.data() + part.size()) {
            throw std::runtime_error("update check: unrecognized version \"" + std::string(text) + "\"");
        }
        components.push_back(value);
        if (end == std::string_view::npos) return components;
        text.remove_prefix(end + 1);
    }
}

}  // namespace

std::string releasePageUrl(const std::string& version) { return std::string(kReleasePagePrefix) + version; }

std::optional<std::string> newerReleaseVersion(const nlohmann::json& latestRelease, std::string_view current) {
    const auto tag = latestRelease.find("tag_name");
    if (tag == latestRelease.end() || !tag->is_string()) {
        throw std::runtime_error("update check: the response has no tag_name");
    }
    const std::string& tagName = tag->get_ref<const std::string&>();
    std::vector<std::uint64_t> latest = parseVersion(tagName);
    std::vector<std::uint64_t> installed = parseVersion(current);
    const size_t length = std::max(latest.size(), installed.size());
    latest.resize(length);
    installed.resize(length);
    if (latest <= installed) return std::nullopt;
    return tagName.starts_with('v') ? tagName.substr(1) : tagName;
}

}  // namespace alarmmap
