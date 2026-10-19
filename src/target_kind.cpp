#include "target_kind.hpp"

#include <algorithm>
#include <cctype>
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
    {"ic_lancet", TargetKind::Drone},
    {"ic_banderol", TargetKind::CruiseMissile},
    {"ic_missile", TargetKind::CruiseMissile},
    {"ic_fixed_wing_lacm", TargetKind::CruiseMissile},
    {"ic_fixed_wing_lr_tasm", TargetKind::CruiseMissile},
    {"ic_ballistic", TargetKind::BallisticMissile},
    {"ic_airbomb", TargetKind::GuidedBomb},
    {"ic_rotary_wing", TargetKind::Helicopter},
    {"ic_ground", TargetKind::Ground},
    {"ic_plane", TargetKind::Plane},
    {"ic_fixed_wing", TargetKind::Plane},
    {"ic_asterix_plane", TargetKind::Plane},
};

constexpr std::string_view kIconPrefix = "ic_";

bool isNonBlank(std::string_view text) {
    return std::any_of(text.begin(), text.end(),
                       [](char symbol) { return !std::isspace(static_cast<unsigned char>(symbol)); });
}

}  // namespace

bool isIdentified(std::string_view iconName, std::string_view detectedType) {
    const auto isIconNameCharacter = [](char symbol) {
        return (symbol >= 'a' && symbol <= 'z') || (symbol >= '0' && symbol <= '9') || symbol == '_';
    };
    return isNonBlank(detectedType) && iconName.size() > kIconPrefix.size() &&
           iconName.starts_with(kIconPrefix) &&
           std::all_of(iconName.begin(), iconName.end(), isIconNameCharacter);
}

TargetKind targetKind(std::string_view iconName, std::string_view detectedType) {
    if (!isIdentified(iconName, detectedType)) return TargetKind::Unknown;
    for (const auto& [prefix, kind] : kKindByIconPrefix) {
        if (iconName.starts_with(prefix)) return kind;
    }
    return TargetKind::Unknown;
}

}  // namespace alarmmap
