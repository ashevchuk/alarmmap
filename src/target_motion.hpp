#pragma once

#include "projection.hpp"
#include "target_snapshot.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace alarmmap {

// Glides target markers between snapshots instead of jumping. A new cell for
// a track starts a glide from wherever the marker is drawn at that moment; a
// repeated report of the same cell leaves the running glide alone. New
// tracks fade in; tracks missing from a snapshot stay a while, then vanish
// in an explosion. A track reported again after its explosion appears anew.
class TargetMotion {
public:
    // Matches the teleport animation that brings a new marker in.
    static constexpr double kFadeInSeconds = 1;
    // A missing target is held as on the site, then vanishes quickly while
    // its explosion plays.
    static constexpr double kMissingHoldSeconds = 10;
    static constexpr double kVanishSeconds = 0.4;
    static constexpr double kExplosionSeconds = 1.6;
    // The explosion leaves a scorch mark fading for this long.
    static constexpr double kScorchSeconds = 20;
    static constexpr double kAftermathSeconds = kExplosionSeconds + kScorchSeconds;
    // A path outlives its marker, so a track that returns after a reconnect
    // keeps the way it has come.
    static constexpr double kPathRetentionSeconds = 600;

    struct Marker {
        Target target;
        MapPoint position;
        double opacity = 1;
        // Still on its way to the target's current cell.
        bool gliding = false;
        // Missing from the latest snapshot.
        bool fadingOut = false;
        // Seconds since the track first appeared.
        double age = 0;
    };

    struct Explosion {
        Target target;
        MapPoint position;
        // Seconds since it started, below kAftermathSeconds: the blast, then
        // its scorch mark.
        double age = 0;
    };

    // Times are seconds on any monotonic clock.
    void update(const std::vector<Target>& targets, double now);
    // Markers with opacity above zero, in the order of the latest snapshot
    // followed by fading ones.
    std::vector<Marker> markersAt(double now) const;
    // Explosions of missing tracks and their scorch marks at the moment.
    std::vector<Explosion> explosionsAt(double now) const;
    // Tracks still drawn, including those fading out or exploding.
    const std::vector<std::string>& trackIds() const { return order_; }
    // Cells the track's marker has visited since it was first seen, oldest
    // first, ending at the marker's drawn position. Empty for unknown tracks.
    std::vector<MapPoint> pathAt(const std::string& trackId, double now) const;

private:
    struct Track {
        Target target;
        MapPoint from;
        MapPoint to;
        double startedAt = 0;
        double duration = 0;
        double appearedAt = 0;
        // Set while the track is missing from the latest snapshot.
        std::optional<double> missingSince;

        MapPoint positionAt(double now) const;
        double opacityAt(double now) const;
        // Seconds since the explosion started; none before the hold ends.
        std::optional<double> explosionAgeAt(double now) const;
    };

    struct Path {
        std::vector<MapPoint> points;
        double lastSeenAt = 0;
    };

    std::unordered_map<std::string, Track> tracks_;
    std::unordered_map<std::string, Path> paths_;
    // Track IDs in drawing order.
    std::vector<std::string> order_;
};

}  // namespace alarmmap
