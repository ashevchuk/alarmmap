#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace alarmmap {

// Polls an HTTP snapshot and delivers every response as a publication on one
// channel. Reports its state with the same events as LiveFeed: session_start,
// connected, session_error (with retryInSeconds) and handler_error.
class AlertPoller {
public:
    using PublicationHandler =
        std::function<void(const std::string& channel, const nlohmann::json& payload)>;
    using EventHandler =
        std::function<void(const std::string& event, const nlohmann::json& details)>;

    AlertPoller(std::string channel, std::string url, PublicationHandler onPublication,
                EventHandler onEvent);

    // Blocks until stop() is called or the deadline passes. Handlers are
    // called on this thread.
    void run(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    // Safe to call from any thread; run() returns within the request in flight.
    void stop();

private:
    void emit(const std::string& event, nlohmann::json details = nlohmann::json::object());
    bool shouldStop(std::chrono::steady_clock::time_point end) const;
    void waitFor(std::chrono::steady_clock::duration delay);

    std::string channel_;
    std::string url_;
    PublicationHandler onPublication_;
    EventHandler onEvent_;
    std::atomic<bool> stopRequested_ = false;
    std::mutex stopMutex_;
    std::condition_variable stopSignal_;
};

}  // namespace alarmmap
