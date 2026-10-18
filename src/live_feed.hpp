#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace alarmmap {

struct FeedCredentials;

struct FeedChannel {
    std::string name;
    // Channel history is closed to anonymous clients, so the current state is
    // fetched from this URL after every subscribe. Empty: no initial state.
    std::string snapshotUrl;
};

// Live connection to the map's Centrifugo server. Takes the connection token
// from the map page, renews it by reconnecting shortly before it expires and
// reconnects with backoff after any failure.
class LiveFeed {
public:
    // Payload of a publication, or of the snapshot fetched for the channel.
    using PublicationHandler =
        std::function<void(const std::string& channel, const nlohmann::json& payload)>;
    // Lifecycle notices: session_start, connected, token_renewal,
    // session_error, snapshot_error, handler_error. Details never contain
    // the token. After a handler_error on a publication the channel's
    // snapshot is fetched again.
    using EventHandler =
        std::function<void(const std::string& event, const nlohmann::json& details)>;

    LiveFeed(std::vector<FeedChannel> channels, PublicationHandler onPublication,
             EventHandler onEvent);

    // Blocks, running sessions until stop() is called or the deadline passes.
    // Handlers are called on this thread.
    void run(std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt);

    // Safe to call from any thread; run() returns within about half a second
    // unless it is inside an HTTP request.
    void stop();

private:
    enum class SessionEnd { Stopped, Renewal };

    SessionEnd runSession(const FeedCredentials& credentials,
                          std::chrono::steady_clock::time_point end);
    // Returns whether the handler accepted the payload.
    bool deliver(const std::string& channel, const nlohmann::json& payload);
    void deliverSnapshot(const FeedChannel& channel);
    void emit(const std::string& event, nlohmann::json details = nlohmann::json::object());
    bool shouldStop(std::chrono::steady_clock::time_point end) const;
    void waitBeforeRetry(std::chrono::steady_clock::duration delay);

    std::vector<FeedChannel> channels_;
    PublicationHandler onPublication_;
    EventHandler onEvent_;
    // Whether the current session got as far as a connect reply.
    bool sessionConnected_ = false;
    std::atomic<bool> stopRequested_ = false;
    std::mutex stopMutex_;
    std::condition_variable stopSignal_;
};

}  // namespace alarmmap
