#include "tile_store.hpp"

#include "http.hpp"
#include "vector_tile.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace alarmmap {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

constexpr auto kTileJsonMaxAge = std::chrono::hours(24);
constexpr auto kRetryAfterFailure = std::chrono::seconds(30);
constexpr const char* kTileJsonFile = "tilejson.json";

std::optional<std::string> readFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// Writes through a temporary file so readers never see a partial file.
void writeFile(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    const fs::path partial = path.string() + ".partial";
    {
        std::ofstream output(partial, std::ios::binary);
        output << data;
        if (!output) throw std::runtime_error("cannot write " + partial.string());
    }
    fs::rename(partial, path);
}

std::string replaceAll(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

std::string tileUrl(const std::string& urlTemplate, const TileId& id) {
    return replaceAll(replaceAll(replaceAll(urlTemplate, "{z}", std::to_string(id.z)), "{x}",
                                 std::to_string(id.x)),
                      "{y}", std::to_string(id.y));
}

// Directory name for a tile data version: the template with everything but
// letters and digits replaced, so another version never mixes in.
std::string versionDirectory(const std::string& urlTemplate) {
    std::string name;
    for (const char symbol : urlTemplate.substr(0, urlTemplate.find("{z}"))) {
        name.push_back(std::isalnum(static_cast<unsigned char>(symbol)) ? symbol : '_');
    }
    return name;
}

std::string templateFromTileJson(const std::string& text) {
    const nlohmann::json tileJson = nlohmann::json::parse(text);
    const auto& tiles = tileJson.at("tiles");
    if (!tiles.is_array() || tiles.empty() || !tiles[0].is_string()) {
        throw std::runtime_error("TileJSON without tile URLs");
    }
    std::string urlTemplate = tiles[0].get<std::string>();
    for (const char* placeholder : {"{z}", "{x}", "{y}"}) {
        if (urlTemplate.find(placeholder) == std::string::npos) {
            throw std::runtime_error("tile URL without " + std::string(placeholder));
        }
    }
    return urlTemplate;
}

}  // namespace

TileStore::TileStore(Options options) : options_(std::move(options)) {
    if (!options_.fetch) options_.fetch = httpGet;
    for (int i = 0; i < options_.workers; ++i) threads_.emplace_back([this] { work(); });
}

TileStore::~TileStore() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& thread : threads_) thread.join();
}

void TileStore::want(const std::vector<TileId>& ids) {
    {
        std::lock_guard lock(mutex_);
        wanted_ = ids;
        ++wantGeneration_;
        for (const TileId& id : ids) {
            const auto entry = tiles_.find(id);
            if (entry != tiles_.end()) entry->second.lastWanted = wantGeneration_;
        }
    }
    wake_.notify_all();
}

std::shared_ptr<const BasemapTile> TileStore::get(const TileId& id) const {
    std::lock_guard lock(mutex_);
    const auto entry = tiles_.find(id);
    return entry == tiles_.end() ? nullptr : entry->second.tile;
}

std::uint64_t TileStore::version() const {
    std::lock_guard lock(mutex_);
    return version_;
}

std::string TileStore::lastError() const {
    std::lock_guard lock(mutex_);
    for (const TileId& id : wanted_) {
        const auto failure = failures_.find(id);
        if (failure != failures_.end()) return failure->second.message;
    }
    return {};
}

std::optional<TileId> TileStore::nextJob() {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (stopping_) return std::nullopt;
        const auto now = Clock::now();
        for (const TileId& id : wanted_) {
            if (tiles_.contains(id) || inProgress_.contains(id)) continue;
            const auto failed = failures_.find(id);
            if (failed != failures_.end() && now - failed->second.at < kRetryAfterFailure) continue;
            inProgress_.insert(id);
            return id;
        }
        // Nothing to do now; wake for new wishes or a pending retry.
        wake_.wait_for(lock, kRetryAfterFailure);
    }
}

std::string TileStore::tileTemplate() {
    std::lock_guard lock(templateMutex_);
    if (tileTemplate_) return *tileTemplate_;

    const fs::path cached = options_.cacheDirectory / kTileJsonFile;
    std::error_code error;
    const auto modified = fs::last_write_time(cached, error);
    const bool fresh = !error && fs::file_time_type::clock::now() - modified < kTileJsonMaxAge;
    if (fresh) {
        if (const auto text = readFile(cached)) {
            tileTemplate_ = templateFromTileJson(*text);
            return *tileTemplate_;
        }
    }
    try {
        const std::string text = options_.fetch(options_.tileJsonUrl);
        tileTemplate_ = templateFromTileJson(text);
        writeFile(cached, text);
    } catch (const std::exception&) {
        // A stale copy beats no basemap while the network is down.
        const auto text = readFile(cached);
        if (!text) throw;
        tileTemplate_ = templateFromTileJson(*text);
    }
    return *tileTemplate_;
}

BasemapTile TileStore::loadTile(const std::string& urlTemplate, const TileId& id) {
    const fs::path cached = options_.cacheDirectory / versionDirectory(urlTemplate) /
                            std::to_string(id.z) / std::to_string(id.x) / (std::to_string(id.y) + ".pbf");
    if (const auto data = readFile(cached)) {
        try {
            return buildBasemapTile(id, decodeVectorTile(*data));
        } catch (const std::runtime_error&) {
            fs::remove(cached);
        }
    }
    const std::string data = options_.fetch(tileUrl(urlTemplate, id));
    BasemapTile tile = buildBasemapTile(id, decodeVectorTile(data));
    writeFile(cached, data);
    return tile;
}

void TileStore::store(const TileId& id, std::shared_ptr<const BasemapTile> tile) {
    std::lock_guard lock(mutex_);
    inProgress_.erase(id);
    failures_.erase(id);
    tiles_[id] = {std::move(tile), wantGeneration_};
    ++version_;
    // Evict the tiles that went longest without being wanted.
    while (tiles_.size() > options_.memoryTiles) {
        const auto oldest = std::min_element(tiles_.begin(), tiles_.end(), [](const auto& a, const auto& b) {
            return a.second.lastWanted < b.second.lastWanted;
        });
        if (oldest->second.lastWanted == wantGeneration_) break;
        tiles_.erase(oldest);
    }
}

void TileStore::fail(const TileId& id, const std::string& error) {
    std::lock_guard lock(mutex_);
    inProgress_.erase(id);
    failures_[id] = {Clock::now(), error};
}

void TileStore::work() {
    while (const auto id = nextJob()) {
        try {
            store(*id, std::make_shared<const BasemapTile>(loadTile(tileTemplate(), *id)));
        } catch (const std::exception& error) {
            fail(*id, "tile " + toString(*id) + ": " + error.what());
        }
    }
}

}  // namespace alarmmap
