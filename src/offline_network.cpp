// Network layer for builds without libcurl: every request fails and the live
// feed stays idle, so only data given as files is shown.

#include "http.hpp"
#include "live_feed.hpp"

#include <stdexcept>
#include <utility>

namespace alarmmap {

std::string httpGet(const std::string& url) {
    throw std::runtime_error("no network access in this build: " + url);
}

LiveFeed::LiveFeed(std::vector<FeedChannel> channels, PublicationHandler onPublication,
                   EventHandler onEvent)
    : channels_(std::move(channels)),
      onPublication_(std::move(onPublication)),
      onEvent_(std::move(onEvent)) {}

void LiveFeed::run(std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::unique_lock lock(stopMutex_);
    const auto stopped = [this] { return stopRequested_.load(); };
    if (deadline) {
        stopSignal_.wait_until(lock, *deadline, stopped);
    } else {
        stopSignal_.wait(lock, stopped);
    }
}

void LiveFeed::stop() {
    {
        std::lock_guard lock(stopMutex_);
        stopRequested_ = true;
    }
    stopSignal_.notify_all();
}

}  // namespace alarmmap
