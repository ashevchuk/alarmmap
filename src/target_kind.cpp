#include "target_kind.hpp"

#include <utility>

namespace alarmmap {

namespace {

// First matching prefix wins, so longer prefixes come before shorter ones.
constexpr std::pair<std::string_view, TargetKind> kKindByIconPrefix[] = {
    {"ic_shahed_jet", TargetKind::JetDrone},
    {"ic_shahed", TargetKind::Drone},
    {"ic_drone", TargetKind::Drone},
    {"ic_fixed_wing_loitering", TargetKind::Drone},
    {"ic_fixed_wing_very_small", TargetKind::Drone},
    {"ic_fixed_wing_small", TargetKind::Drone},
    {"ic_missile", TargetKind::CruiseMissile},
    {"ic_fixed_wing_lacm", TargetKind::CruiseMissile},
    {"ic_fixed_wing_lr_tasm", TargetKind::CruiseMissile},
    {"ic_ballistic", TargetKind::BallisticMissile},
    {"ic_airbomb", TargetKind::GuidedBomb},
    {"ic_rotary_wing", TargetKind::Helicopter},
    {"ic_ground", TargetKind::Ground},
};

}  // namespace

TargetKind targetKind(std::string_view iconName) {
    for (const auto& [prefix, kind] : kKindByIconPrefix) {
        if (iconName.starts_with(prefix)) return kind;
    }
    return TargetKind::Plane;
}

}  // namespace alarmmap
