#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace alarmmap {

// Grid cell in coordinates normalized to the map's Mercator bounding box (0..1).
struct Cell {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    std::string gridLevel;

    bool operator==(const Cell&) const = default;
};

struct Prediction {
    double fromX = 0;
    double fromY = 0;
    double toX = 0;
    double toY = 0;
    std::int64_t startedAtMs = 0;
    std::int64_t endsAtMs = 0;

    bool operator==(const Prediction&) const = default;
};

struct Target {
    Cell cell;
    std::string trackId;
    int type = 0;
    std::string formular;
    std::string detectedType;
    std::string iconName;
    double iconScale = 0;
    std::optional<double> headingDegrees;
    int currentCount = 0;
    int hits = 0;
    int movementDurationMs = 0;
    double markerOffsetX = 0;
    double markerOffsetY = 0;
    bool isFading = false;
    std::optional<std::int64_t> fadeStartedAtMs;
    std::optional<std::int64_t> fadeEndsAtMs;
    std::optional<Prediction> prediction;

    bool operator==(const Target&) const = default;
};

// Server-scheduled intensity of one observation inside a heat cell. Seconds.
struct HeatSample {
    double age = 0;
    std::optional<double> baseStrength;
    double minimumStrength = 0;
    std::optional<double> retentionAge;
    std::optional<double> fadeDelay;
    std::optional<double> fadeDuration;
    std::optional<double> appearDelay;
    std::optional<double> appearDuration;
    std::optional<double> visibleUntil;

    bool operator==(const HeatSample&) const = default;
};

// One cell of a heat trail. Ages are seconds since the observation.
struct HeatCell {
    Cell cell;
    double age = 0;
    int hits = 0;
    std::optional<double> baseStrength;
    double minimumStrength = 0;
    std::optional<double> retentionAge;
    std::optional<std::vector<HeatSample>> samples;
    std::optional<std::vector<Cell>> extrapolationHoldAnchors;
    bool predictedExtrapolation = false;

    bool operator==(const HeatCell&) const = default;
};

// Previous target positions the renderer animates from.
struct Movement {
    std::vector<Target> from;
    std::int64_t fromUpdatedAtMs = 0;
    std::int64_t startedAtMs = 0;

    bool operator==(const Movement&) const = default;
};

struct TargetSnapshot {
    std::vector<Target> targets;
    std::vector<HeatCell> trail;
    std::vector<HeatCell> extrapolationTrail;
    std::int64_t serverTimeMs = 0;
    std::int64_t updatedAtMs = 0;
    std::int64_t extrapolationUpdatedAtMs = 0;
    double fogSeconds = 0;
    double snapshotTtlSeconds = 0;
    std::optional<Movement> movement;
    // The site has stopped publishing targets: the snapshot is empty on
    // purpose, not because no targets are in the air.
    bool targetsPaused = false;
    // Entries (targets, heat cells, track IDs) and header fields that could
    // not be read: the entries were left out, the fields took defaults.
    int skippedEntries = 0;

    bool operator==(const TargetSnapshot&) const = default;
};

// Accepts the compact v2 packet pushed over Centrifugo or the expanded JSON
// returned by the REST endpoint, optionally still inside Centrifugo's
// {data: ...} envelopes. Degrades gracefully: an entry it cannot read is
// left out and counted in skippedEntries, and fields added after the known
// ones are ignored. Throws std::runtime_error only when the packet as a
// whole is unusable: an unknown format or version, or no server time.
TargetSnapshot decodeTargetSnapshot(const nlohmann::json& packet);

// Decodes packets and drops v2 snapshots that are duplicates or older than
// the last accepted one, judged by their epoch and revision.
class TargetSnapshotReader {
public:
    std::optional<TargetSnapshot> read(const nlohmann::json& packet);

private:
    std::optional<std::string> epoch_;
    std::string revision_ = "0";
    std::int64_t serverTimeMs_ = 0;
    std::deque<std::string> seenEpochs_;
};

}  // namespace alarmmap
