#pragma once

#include "basemap.hpp"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace alarmmap {

// Fetches, caches and prepares basemap tiles on background threads.
// Tile data is immutable per data version, so tiles are cached on disk under
// the version taken from the TileJSON; the TileJSON itself is refreshed
// daily and its last copy is used while the network is down.
class TileStore {
public:
    using Fetch = std::function<std::string(const std::string& url)>;

    struct Options {
        std::string tileJsonUrl;
        std::filesystem::path cacheDirectory;
        // Throws on failure; defaults to an HTTP GET.
        Fetch fetch;
        size_t memoryTiles = 400;
        int workers = 4;
    };

    explicit TileStore(Options options);
    ~TileStore();
    TileStore(const TileStore&) = delete;
    TileStore& operator=(const TileStore&) = delete;

    // Replaces the set of wanted tiles, most important first.
    void want(const std::vector<TileId>& ids);
    // A prepared tile, or nullptr while it is not available.
    std::shared_ptr<const BasemapTile> get(const TileId& id) const;
    // Grows whenever a tile becomes available.
    std::uint64_t version() const;
    // Why a wanted tile is missing, for the status line; empty when every
    // wanted tile either loaded or is still on its way.
    std::string lastError() const;

private:
    struct Entry {
        std::shared_ptr<const BasemapTile> tile;
        std::uint64_t lastWanted = 0;
    };

    void work();
    std::optional<TileId> nextJob();
    std::string tileTemplate();
    // From the disk cache, or from the network when the cached copy is
    // missing or unreadable; only data that decodes is written to disk.
    BasemapTile loadTile(const std::string& urlTemplate, const TileId& id);
    void store(const TileId& id, std::shared_ptr<const BasemapTile> tile);
    void fail(const TileId& id, const std::string& error);

    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::vector<TileId> wanted_;
    std::uint64_t wantGeneration_ = 0;
    std::map<TileId, Entry> tiles_;
    std::set<TileId> inProgress_;
    struct Failure {
        std::chrono::steady_clock::time_point at;
        std::string message;
    };
    // Tiles that failed; each is retried only after a pause.
    std::map<TileId, Failure> failures_;
    std::uint64_t version_ = 0;
    std::optional<std::string> tileTemplate_;
    std::mutex templateMutex_;
    std::vector<std::thread> threads_;
};

}  // namespace alarmmap
