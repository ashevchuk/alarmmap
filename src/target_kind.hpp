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
    // Not identified by the source, or identified with an icon this app does
    // not know.
    Unknown,
};

inline constexpr TargetKind kAllTargetKinds[] = {
    TargetKind::Plane,         TargetKind::Drone,          TargetKind::JetDrone,
    TargetKind::CruiseMissile, TargetKind::BallisticMissile, TargetKind::GuidedBomb,
    TargetKind::Helicopter,    TargetKind::Ground,         TargetKind::Unknown,
};

// Like the site, a target counts as identified only with a detected type and
// an icon name of the form ic_[a-z0-9_]+.
bool isIdentified(std::string_view iconName, std::string_view detectedType);

// Kind of a target by the icon name and detected type the site assigns to
// it. Unidentified targets and icon names without a match are Unknown.
TargetKind targetKind(std::string_view iconName, std::string_view detectedType);

}  // namespace alarmmap
