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
// tracks fade in; tracks missing from a snapshot stay a while, then fade out.
class TargetMotion {
public:
    static constexpr double kFadeInSeconds = 3;
    // A missing target is held as on the site: fully visible for the delay,
    // then fading until the fog time of the heat trail.
    static constexpr double kMissingHoldSeconds = 10;
    static constexpr double kMissingFadeSeconds = 20;

    struct Marker {
        Target target;
        MapPoint position;
        double opacity = 1;
        // Still on its way to the target's current cell.
        bool gliding = false;
        // Missing from the latest snapshot.
        bool fadingOut = false;
    };

    // Times are seconds on any monotonic clock.
    void update(const std::vector<Target>& targets, double now);
    // Markers with opacity above zero, in the order of the latest snapshot
    // followed by fading ones.
    std::vector<Marker> markersAt(double now) const;
    // Tracks still drawn, including those fading out.
    const std::vector<std::string>& trackIds() const { return order_; }

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
    };

    std::unordered_map<std::string, Track> tracks_;
    // Track IDs in drawing order.
    std::vector<std::string> order_;
};

}  // namespace alarmmap
