#include "live_feed.hpp"

#include "base64url.hpp"
#include "http.hpp"

#include <curl/curl.h>
#include <poll.h>

#include <algorithm>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace alarmmap {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kPageUrl = "https://map.ukrainealarm.com/";
constexpr const char* kOriginHeader = "Origin: https://map.ukrainealarm.com";
constexpr long kConnectTimeoutSeconds = 15;
constexpr auto kPingInterval = std::chrono::seconds(25);
constexpr auto kReceiveSlice = std::chrono::milliseconds(500);
constexpr auto kRenewalMargin = std::chrono::minutes(5);
constexpr auto kInitialBackoff = std::chrono::seconds(5);
// The server closes healthy connections every few minutes, so losing an
// established session is routine and is retried almost at once.
constexpr auto kReconnectAfterDrop = std::chrono::seconds(1);
constexpr auto kMaxBackoff = std::chrono::seconds(60);

// Numeric method ids of the legacy Centrifuge JSON protocol used by
// centrifuge-js 2.x, which is what the site itself runs.
enum class Method : int { Connect = 0, Subscribe = 1, Ping = 7 };

struct CurlDeleter {
    void operator()(CURL* handle) const { curl_easy_cleanup(handle); }
};
struct HeaderListDeleter {
    void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

void checkCurl(CURLcode code, const char* what) {
    if (code != CURLE_OK) {
        throw std::runtime_error(std::string(what) + ": " + curl_easy_strerror(code));
    }
}

class WebSocket {
public:
    explicit WebSocket(const std::string& url) : curl_(curl_easy_init()) {
        if (!curl_) throw std::runtime_error("curl_easy_init failed");
        headers_.reset(curl_slist_append(nullptr, kOriginHeader));
        curl_easy_setopt(curl_.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_.get(), CURLOPT_USERAGENT, kBrowserUserAgent);
        curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER, headers_.get());
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        // 2 = perform only the WebSocket upgrade, then hand the socket to us.
        curl_easy_setopt(curl_.get(), CURLOPT_CONNECT_ONLY, 2L);
        checkCurl(curl_easy_perform(curl_.get()), "websocket upgrade");
        checkCurl(curl_easy_getinfo(curl_.get(), CURLINFO_ACTIVESOCKET, &socket_),
                  "active socket");
    }

    void sendText(const std::string& message) {
        size_t offset = 0;
        while (offset < message.size()) {
            size_t sent = 0;
            const CURLcode code = curl_ws_send(curl_.get(), message.data() + offset,
                                               message.size() - offset, &sent, 0, CURLWS_TEXT);
            if (code == CURLE_AGAIN) {
                waitFor(POLLOUT, kReceiveSlice);
                continue;
            }
            checkCurl(code, "websocket send");
            offset += sent;
        }
    }

    // Returns one complete text message, or nothing if none arrived in time.
    std::optional<std::string> receive(std::chrono::milliseconds timeout) {
        const auto deadline = Clock::now() + timeout;
        char buffer[64 * 1024];
        for (;;) {
            size_t received = 0;
            const curl_ws_frame* frame = nullptr;
            const CURLcode code =
                curl_ws_recv(curl_.get(), buffer, sizeof(buffer), &received, &frame);
            if (code == CURLE_AGAIN) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                if (left.count() <= 0 || !waitFor(POLLIN, left)) return std::nullopt;
                continue;
            }
            if (code == CURLE_GOT_NOTHING) throw std::runtime_error("server closed connection");
            checkCurl(code, "websocket receive");

            if (frame->flags & CURLWS_CLOSE) throw std::runtime_error("server sent close frame");
            if (frame->flags & (CURLWS_PING | CURLWS_PONG)) continue;  // libcurl answers pings

            pending_.append(buffer, received);
            const bool lastFragment = !(frame->flags & CURLWS_CONT);
            if (frame->bytesleft == 0 && lastFragment) {
                std::string message;
                message.swap(pending_);
                return message;
            }
        }
    }

private:
    bool waitFor(short events, std::chrono::milliseconds timeout) {
        pollfd descriptor{static_cast<int>(socket_), events, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(timeout.count()));
        if (ready < 0) throw std::runtime_error("poll failed");
        return ready > 0;
    }

    std::unique_ptr<CURL, CurlDeleter> curl_;
    std::unique_ptr<curl_slist, HeaderListDeleter> headers_;
    curl_socket_t socket_ = CURL_SOCKET_BAD;
    std::string pending_;
};

std::string extractHiddenInput(const std::string& html, const std::string& id) {
    const std::regex pattern("<input[^>]*id=\"" + id + "\"[^>]*value=\"([^\"]+)\"");
    std::smatch match;
    if (!std::regex_search(html, match, pattern)) {
        throw std::runtime_error("hidden input '" + id + "' not found in map page");
    }
    return match[1].str();
}

std::chrono::system_clock::time_point jwtExpiry(const std::string& token) {
    if (std::count(token.begin(), token.end(), '.') != 2) {
        throw std::runtime_error("centrifugo-token is not a JWT");
    }
    const size_t payloadStart = token.find('.') + 1;
    const size_t payloadEnd = token.find('.', payloadStart);
    const json claims = json::parse(
        decodeBase64Url(std::string_view(token).substr(payloadStart, payloadEnd - payloadStart)),
        nullptr, false);
    if (claims.is_discarded() || !claims.contains("exp") || !claims["exp"].is_number()) {
        throw std::runtime_error("centrifugo-token has no numeric exp claim");
    }
    return std::chrono::system_clock::time_point(
        std::chrono::seconds(claims["exp"].get<long long>()));
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

struct FeedCredentials {
    std::string token;
    std::string url;
    std::chrono::system_clock::time_point expiresAt;
};

namespace {

FeedCredentials fetchCredentials() {
    const std::string html = httpGet(kPageUrl);
    FeedCredentials credentials{extractHiddenInput(html, "centrifugo-token"),
                            extractHiddenInput(html, "centrifugo-url"), {}};
    if (credentials.url.rfind("wss://", 0) != 0) {
        throw std::runtime_error("centrifugo-url is not wss://");
    }
    credentials.expiresAt = jwtExpiry(credentials.token);
    return credentials;
}

}  // namespace

LiveFeed::LiveFeed(std::vector<FeedChannel> channels, PublicationHandler onPublication,
                   EventHandler onEvent)
    : channels_(std::move(channels)),
      onPublication_(std::move(onPublication)),
      onEvent_(std::move(onEvent)) {}

void LiveFeed::stop() {
    {
        std::lock_guard lock(stopMutex_);
        stopRequested_ = true;
    }
    stopSignal_.notify_all();
}

bool LiveFeed::shouldStop(Clock::time_point end) const {
    return stopRequested_ || Clock::now() >= end;
}

void LiveFeed::waitBeforeRetry(Clock::duration delay) {
    std::unique_lock lock(stopMutex_);
    stopSignal_.wait_for(lock, delay, [this] { return stopRequested_.load(); });
}

void LiveFeed::emit(const std::string& event, json details) {
    if (onEvent_) onEvent_(event, details);
}

void LiveFeed::deliver(const std::string& channel, const json& payload) {
    try {
        onPublication_(channel, payload);
    } catch (const std::exception& error) {
        emit("handler_error", {{"channel", channel}, {"error", error.what()}});
    }
}

void LiveFeed::deliverSnapshot(const FeedChannel& channel) {
    if (channel.snapshotUrl.empty()) return;
    // Missing initial state is not worth dropping the live session for: the
    // next publication on the channel brings the state anyway.
    try {
        deliver(channel.name, json::parse(httpGet(channel.snapshotUrl)));
    } catch (const std::exception& error) {
        emit("snapshot_error", {{"channel", channel.name}, {"error", error.what()}});
    }
}

void LiveFeed::run(std::optional<Clock::time_point> deadline) {
    const Clock::time_point end = deadline.value_or(Clock::time_point::max());
    Clock::duration backoff = kInitialBackoff;
    while (!shouldStop(end)) {
        sessionConnected_ = false;
        try {
            const FeedCredentials credentials = fetchCredentials();
            const auto lifetime = std::chrono::duration_cast<std::chrono::seconds>(
                credentials.expiresAt - std::chrono::system_clock::now());
            emit("session_start", {{"url", credentials.url},
                                   {"tokenLifetimeSeconds", lifetime.count()}});
            if (runSession(credentials, end) == SessionEnd::Renewal) emit("token_renewal");
            backoff = kInitialBackoff;
        } catch (const std::exception& error) {
            if (sessionConnected_) backoff = kReconnectAfterDrop;
            const Clock::duration wait = std::min(backoff, end - Clock::now());
            emit("session_error",
                 {{"error", error.what()},
                  {"retryInSeconds",
                   std::chrono::duration_cast<std::chrono::seconds>(wait).count()}});
            waitBeforeRetry(wait);
            backoff = std::min<Clock::duration>(backoff * 2, kMaxBackoff);
        }
    }
}

LiveFeed::SessionEnd LiveFeed::runSession(const FeedCredentials& credentials, Clock::time_point end) {
    const auto renewAt =
        Clock::now() + (credentials.expiresAt - kRenewalMargin - std::chrono::system_clock::now());
    if (renewAt <= Clock::now()) throw std::runtime_error("token expires too soon to use");
    const Clock::time_point sessionEnd = std::min(end, renewAt);

    WebSocket socket(credentials.url);
    int nextId = 1;
    // Request id -> channel index, or -1 for connect and ping.
    std::map<int, int> pending;
    auto send = [&](Method method, json params, int channelIndex) {
        json command = {{"id", nextId}, {"method", static_cast<int>(method)}};
        if (!params.is_null()) command["params"] = std::move(params);
        socket.sendText(command.dump());
        pending[nextId++] = channelIndex;
    };

    send(Method::Connect, {{"token", credentials.token}}, -1);
    bool connected = false;
    auto lastPing = Clock::now();

    while (!shouldStop(sessionEnd)) {
        if (Clock::now() - lastPing >= kPingInterval) {
            send(Method::Ping, nullptr, -1);
            lastPing = Clock::now();
        }
        const auto frame = socket.receive(kReceiveSlice);
        if (!frame) continue;

        // The legacy protocol may batch several messages into one frame.
        for (const std::string& line : splitLines(*frame)) {
            const json message = json::parse(line, nullptr, false);
            if (!message.is_object()) throw std::runtime_error("unparsable server message");
            const json& result = message.contains("result") ? message["result"] : json::object();

            if (!message.contains("id")) {
                if (result.contains("channel") && result.contains("data")) {
                    const json& envelope = result["data"];
                    deliver(result["channel"].get<std::string>(),
                            envelope.contains("data") ? envelope["data"] : envelope);
                }
                continue;
            }

            const int id = message["id"].get<int>();
            const auto request = pending.find(id);
            if (request == pending.end()) continue;
            const int channelIndex = request->second;
            pending.erase(request);
            if (message.contains("error")) {
                throw std::runtime_error("server rejected request: " + message["error"].dump());
            }
            if (channelIndex >= 0) {
                deliverSnapshot(channels_[static_cast<size_t>(channelIndex)]);
            } else if (!connected) {
                connected = true;
                sessionConnected_ = true;
                emit("connected");
                for (size_t i = 0; i < channels_.size(); ++i) {
                    send(Method::Subscribe, {{"channel", channels_[i].name}}, static_cast<int>(i));
                }
            }
        }
    }
    if (!connected && !stopRequested_) {
        throw std::runtime_error("no connect reply before session end");
    }
    return stopRequested_ || Clock::now() >= end ? SessionEnd::Stopped : SessionEnd::Renewal;
}

}  // namespace alarmmap
