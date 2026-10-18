#include "target_motion.hpp"

#include <algorithm>
#include <unordered_set>

namespace alarmmap {

namespace {

constexpr double kDefaultGlideSeconds = 20;

MapPoint markerPosition(const Target& target) {
    return normalizedToMap(target.cell.x + target.markerOffsetX, target.cell.y + target.markerOffsetY);
}

double glideSeconds(const Target& target) {
    return target.movementDurationMs > 0 ? target.movementDurationMs / 1000.0 : kDefaultGlideSeconds;
}

}  // namespace

MapPoint TargetMotion::Track::positionAt(double now) const {
    const double progress = duration > 0 ? std::clamp((now - startedAt) / duration, 0.0, 1.0) : 1.0;
    return {from.x + (to.x - from.x) * progress, from.y + (to.y - from.y) * progress};
}

double TargetMotion::Track::opacityAt(double now) const {
    const double fadeIn = std::clamp((now - appearedAt) / kFadeInSeconds, 0.0, 1.0);
    if (!missingSince) return fadeIn;
    const double vanish =
        1 - std::clamp((now - *missingSince - kMissingHoldSeconds) / kVanishSeconds, 0.0, 1.0);
    return std::min(fadeIn, vanish);
}

std::optional<double> TargetMotion::Track::explosionAgeAt(double now) const {
    if (!missingSince) return std::nullopt;
    const double age = now - *missingSince - kMissingHoldSeconds;
    if (age < 0) return std::nullopt;
    return age;
}

void TargetMotion::update(const std::vector<Target>& targets, double now) {
    std::vector<std::string> order;
    std::unordered_set<std::string> present;
    for (const Target& target : targets) {
        if (!present.insert(target.trackId).second) continue;
        order.push_back(target.trackId);
        Path& path = paths_[target.trackId];
        path.lastSeenAt = now;
        const auto existing = tracks_.find(target.trackId);
        if (existing == tracks_.end() || existing->second.explosionAgeAt(now)) {
            const MapPoint position = markerPosition(target);
            tracks_[target.trackId] = {target, position, position, now, 0, now, std::nullopt};
            path.points.push_back(position);
            continue;
        }
        Track& track = existing->second;
        const bool sameCell = track.target.cell.x == target.cell.x && track.target.cell.y == target.cell.y;
        if (!sameCell) {
            track.from = track.positionAt(now);
            track.to = markerPosition(target);
            track.startedAt = now;
            track.duration = glideSeconds(target);
            path.points.push_back(track.to);
        }
        track.target = target;
        track.missingSince.reset();
    }

    // Tracks missing from this snapshot stay until their explosion is over.
    for (const std::string& trackId : order_) {
        if (present.contains(trackId)) continue;
        Track& track = tracks_.at(trackId);
        if (!track.missingSince) track.missingSince = now;
        if (track.explosionAgeAt(now).value_or(0) < kAftermathSeconds) order.push_back(trackId);
    }
    std::erase_if(tracks_, [&](const auto& entry) {
        return std::find(order.begin(), order.end(), entry.first) == order.end();
    });
    order_ = std::move(order);
    std::erase_if(paths_, [&](const auto& entry) {
        return now - entry.second.lastSeenAt > kPathRetentionSeconds;
    });
}

std::vector<MapPoint> TargetMotion::pathAt(const std::string& trackId, double now) const {
    const auto path = paths_.find(trackId);
    if (path == paths_.end()) return {};
    std::vector<MapPoint> points = path->second.points;
    // The last point is the cell the marker is gliding to; end where it is.
    if (const auto track = tracks_.find(trackId); track != tracks_.end()) {
        points.back() = track->second.positionAt(now);
    }
    return points;
}

std::vector<TargetMotion::Marker> TargetMotion::markersAt(double now) const {
    std::vector<Marker> markers;
    markers.reserve(order_.size());
    for (const std::string& trackId : order_) {
        const Track& track = tracks_.at(trackId);
        const double opacity = track.opacityAt(now);
        if (opacity > 0) {
            const MapPoint position = track.positionAt(now);
            markers.push_back({track.target, position, opacity, !(position == track.to),
                               track.missingSince.has_value(), now - track.appearedAt});
        }
    }
    return markers;
}

std::vector<TargetMotion::Explosion> TargetMotion::explosionsAt(double now) const {
    std::vector<Explosion> explosions;
    for (const std::string& trackId : order_) {
        const Track& track = tracks_.at(trackId);
        const std::optional<double> age = track.explosionAgeAt(now);
        if (age && *age < kAftermathSeconds) explosions.push_back({track.target, track.positionAt(now), *age});
    }
    return explosions;
}

}  // namespace alarmmap
