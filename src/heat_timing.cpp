#include "heat_timing.hpp"

#include <algorithm>
#include <cmath>

namespace alarmmap {

namespace {

constexpr double kMinimumFadeSpan = 0.001;
constexpr double kDefaultBaseStrength = 0.45;
constexpr double kBaseStrengthPerHit = 0.55;
constexpr double kHitsForFullStrength = 10;

// The fields the timing model reads, common to cells and their samples.
struct Timing {
    double age = 0;
    int hits = 1;
    std::optional<double> baseStrength;
    double minimumStrength = 0;
    std::optional<double> retentionAge;
    std::optional<double> fadeDelay;
    std::optional<double> fadeDuration;
    std::optional<double> appearDelay;
    std::optional<double> appearDuration;
};

Timing timingOf(const HeatCell& cell) {
    Timing timing;
    timing.age = cell.age;
    timing.hits = cell.hits;
    timing.baseStrength = cell.baseStrength;
    timing.minimumStrength = cell.minimumStrength;
    timing.retentionAge = cell.retentionAge;
    return timing;
}

Timing timingOf(const HeatSample& sample) {
    return {sample.age,          1,
            sample.baseStrength, sample.minimumStrength,
            sample.retentionAge, sample.fadeDelay,
            sample.fadeDuration, sample.appearDelay,
            sample.appearDuration};
}

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

double fade(double age, double fog, double delay) {
    return clamp01(1 - std::max(0.0, age - delay) / std::max(kMinimumFadeSpan, fog - delay));
}

bool hasRetention(const Timing& timing) {
    return timing.retentionAge && std::isfinite(*timing.retentionAge) &&
           timing.minimumStrength > 0;
}

bool hasScheduledFade(const Timing& timing) {
    return timing.fadeDelay && timing.fadeDuration && *timing.fadeDelay >= 0 &&
           *timing.fadeDuration > 0;
}

bool hasScheduledAppearance(const Timing& timing) {
    return timing.appearDelay && timing.appearDuration && *timing.appearDelay >= 0 &&
           *timing.appearDuration > 0;
}

bool isAlive(const Timing& timing, double fog) {
    if (hasScheduledFade(timing)) return timing.age < *timing.fadeDelay + *timing.fadeDuration;
    return timing.age < fog || (hasRetention(timing) && *timing.retentionAge < fog);
}

double strength(const Timing& timing, double fog, double delay) {
    const int hits = std::max(1, timing.hits);
    const double base =
        timing.baseStrength
            ? clamp01(*timing.baseStrength)
            : kDefaultBaseStrength +
                  kBaseStrengthPerHit * std::min(1.0, (hits - 1) / kHitsForFullStrength);
    const double appearance =
        hasScheduledAppearance(timing)
            ? clamp01((timing.age - *timing.appearDelay) / *timing.appearDuration)
            : 1;
    if (hasScheduledFade(timing)) {
        return appearance * base *
               fade(timing.age, *timing.fadeDelay + *timing.fadeDuration, *timing.fadeDelay);
    }
    // A previous cell keeps a floor of intensity, and the floor itself fades
    // on the retention clock once the target has left it.
    const double minimum =
        hasRetention(timing) ? clamp01(timing.minimumStrength) * fade(*timing.retentionAge, fog, delay)
                             : 0;
    return appearance * base * std::max(fade(timing.age, fog, delay), minimum);
}

}  // namespace

HeatCell agedHeatCell(const HeatCell& cell, double elapsedSeconds) {
    HeatCell aged = cell;
    aged.age = std::max(0.0, cell.age) + elapsedSeconds;
    if (hasRetention(timingOf(cell))) aged.retentionAge = std::max(0.0, *cell.retentionAge) + elapsedSeconds;
    if (aged.samples) {
        for (HeatSample& sample : *aged.samples) {
            const bool retained = hasRetention(timingOf(sample));
            sample.age = std::max(0.0, sample.age) + elapsedSeconds;
            if (retained) sample.retentionAge = std::max(0.0, *sample.retentionAge) + elapsedSeconds;
        }
    }
    return aged;
}

bool isHeatCellAlive(const HeatCell& cell, double fogSeconds) {
    if (cell.samples) {
        return std::any_of(cell.samples->begin(), cell.samples->end(),
                           [&](const HeatSample& sample) { return isAlive(timingOf(sample), fogSeconds); });
    }
    return isAlive(timingOf(cell), fogSeconds);
}

double heatStrength(const HeatCell& cell, double fogSeconds, double fadeDelaySeconds) {
    if (cell.samples) {
        double strongest = 0;
        for (const HeatSample& sample : *cell.samples) {
            strongest = std::max(strongest, strength(timingOf(sample), fogSeconds, fadeDelaySeconds));
        }
        return strongest;
    }
    return strength(timingOf(cell), fogSeconds, fadeDelaySeconds);
}

}  // namespace alarmmap
