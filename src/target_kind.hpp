#pragma once

#include <string_view>

namespace alarmmap {

enum class TargetKind {
    Plane,
    Drone,
    JetDrone,
    CruiseMissile,
    BallisticMissile,
    GuidedBomb,
    Helicopter,
    Ground,
    // Not identified by the source; the site draws these as Shahed drones.
    Unknown,
};

inline constexpr TargetKind kAllTargetKinds[] = {
    TargetKind::Plane,         TargetKind::Drone,          TargetKind::JetDrone,
    TargetKind::CruiseMissile, TargetKind::BallisticMissile, TargetKind::GuidedBomb,
    TargetKind::Helicopter,    TargetKind::Ground,         TargetKind::Unknown,
};

// Kind of a target by the icon name and detected type the site assigns to
// it. Like the site, a target counts as identified only with a detected type
// and an icon name of the form ic_[a-z0-9_]+. Identified names without a
// match are drones, drawn by the site with its Shahed icon.
TargetKind targetKind(std::string_view iconName, std::string_view detectedType);

}  // namespace alarmmap
