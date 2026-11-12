#include "alert_poll.hpp"

#include "http.hpp"

#include <algorithm>
#include <stdexcept>

namespace alarmmap {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

// The site caches the snapshot for 10 s, so asking more often changes nothing.
constexpr auto kPollInterval = std::chrono::seconds(10);
constexpr auto kInitialBackoff = std::chrono::seconds(5);
constexpr auto kMaxBackoff = std::chrono::seconds(60);

}  // namespace

AlertPoller::AlertPoller(std::string channel, std::string url, PublicationHandler onPublication,
                         EventHandler onEvent)
    : channel_(std::move(channel)),
      url_(std::move(url)),
      onPublication_(std::move(onPublication)),
      onEvent_(std::move(onEvent)) {}

void AlertPoller::stop() {
    {
        std::lock_guard lock(stopMutex_);
        stopRequested_ = true;
    }
    stopSignal_.notify_all();
}

bool AlertPoller::shouldStop(Clock::time_point end) const {
    return stopRequested_ || Clock::now() >= end;
}

void AlertPoller::waitFor(Clock::duration delay) {
    std::unique_lock lock(stopMutex_);
    stopSignal_.wait_for(lock, delay, [this] { return stopRequested_.load(); });
}

void AlertPoller::emit(const std::string& event, json details) {
    if (onEvent_) onEvent_(event, details);
}

void AlertPoller::run(std::optional<Clock::time_point> deadline) {
    const Clock::time_point end = deadline.value_or(Clock::time_point::max());
    Clock::duration backoff = kInitialBackoff;
    bool connected = false;
    emit("session_start");
    while (!shouldStop(end)) {
        Clock::duration wait = kPollInterval;
        try {
            const json payload = json::parse(httpGet(url_));
            if (!connected) {
                connected = true;
                emit("connected");
            }
            backoff = kInitialBackoff;
            // A payload the handler rejects is a format problem, not a lost
            // connection: the next poll brings a fresh one.
            try {
                onPublication_(channel_, payload);
            } catch (const std::exception& error) {
                emit("handler_error", {{"channel", channel_}, {"error", error.what()}});
            }
        } catch (const std::exception& error) {
            connected = false;
            wait = std::min(backoff, end - Clock::now());
            emit("session_error",
                 {{"error", error.what()},
                  {"retryInSeconds", std::chrono::duration_cast<std::chrono::seconds>(wait).count()}});
            backoff = std::min<Clock::duration>(backoff * 2, kMaxBackoff);
        }
        waitFor(std::min(wait, end - Clock::now()));
    }
}

}  // namespace alarmmap
