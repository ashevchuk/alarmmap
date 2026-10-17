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
};

inline constexpr TargetKind kAllTargetKinds[] = {
    TargetKind::Plane,         TargetKind::Drone,          TargetKind::JetDrone,
    TargetKind::CruiseMissile, TargetKind::BallisticMissile, TargetKind::GuidedBomb,
    TargetKind::Helicopter,    TargetKind::Ground,
};

// Kind of a target by the icon name the site assigns to it; unknown names
// are treated as planes, the site's own fallback icon.
TargetKind targetKind(std::string_view iconName);

}  // namespace alarmmap
