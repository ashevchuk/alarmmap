// Usage: update_check_test <tests/data directory>

#include "update_check.hpp"

#include <iostream>
#include <source_location>
#include <string>

using namespace alarmmap;
using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool condition, const std::string& what,
           std::source_location location = std::source_location::current()) {
    if (condition) return;
    ++failures;
    std::cerr << location.file_name() << ':' << location.line() << ": FAILED: " << what << '\n';
}

std::optional<std::string> newer(const std::string& tag, const std::string& current) {
    return newerReleaseVersion(json{{"tag_name", tag}}, current);
}

template <typename Action>
void checkThrows(Action action, const std::string& what,
                 std::source_location location = std::source_location::current()) {
    try {
        action();
    } catch (const std::runtime_error&) {
        return;
    }
    check(false, what + " (no exception)", location);
}

void testComparesNumerically() {
    check(newer("v0.2.10", "0.2.9") == "0.2.10", "0.2.10 is newer than 0.2.9, not older as text");
    check(newer("v1.0.0", "0.9.9") == "1.0.0", "a higher major version is newer");
    check(newer("v0.3", "0.2.9") == "0.3", "a shorter version is padded with zeros");
    check(!newer("v0.2.9", "0.2.9"), "the same version is not newer");
    check(!newer("v0.3", "0.3.0"), "trailing zeros do not make a version newer");
    check(!newer("v0.2.8", "0.2.9"), "an older release is not newer");
}

void testTagWithoutPrefix() {
    check(newer("0.2.10", "0.2.9") == "0.2.10", "a tag without v");
}

void testRejectsUnrecognizedShapes() {
    checkThrows([] { newerReleaseVersion(json::object(), "0.2.9"); }, "no tag_name");
    checkThrows([] { newerReleaseVersion(json{{"tag_name", 5}}, "0.2.9"); }, "tag_name not a string");
    checkThrows([] { newer("v0.3.0-rc1", "0.2.9"); }, "a pre-release suffix");
    checkThrows([] { newer("latest", "0.2.9"); }, "a tag that is not a version");
    checkThrows([] { newer("v", "0.2.9"); }, "an empty version");
    checkThrows([] { newer("v0..3", "0.2.9"); }, "an empty component");
    checkThrows([] { newer("v0.3", "dev"); }, "an unreadable installed version");
    checkThrows([] { newer("v0.3.99999999999999999999", "0.2.9"); }, "an oversized component");
}

void testReleasePageUrl() {
    check(releasePageUrl("0.2.10") == "https://github.com/ashevchuk/alarmmap/releases/tag/v0.2.10", "release page");
}

}  // namespace

int main() {
    testComparesNumerically();
    testTagWithoutPrefix();
    testRejectsUnrecognizedShapes();
    testReleasePageUrl();
    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
