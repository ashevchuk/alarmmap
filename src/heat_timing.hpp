#pragma once

#include "target_snapshot.hpp"

namespace alarmmap {

// Heat intensity model of map.ukrainealarm.com (detailTargetHeatTiming.js).
// Times are seconds. A cell burns at full strength for fadeDelay seconds,
// then fades linearly until fog seconds; server-scheduled samples override
// that schedule.
inline constexpr double kHeatFadeDelaySeconds = 10;

// The cell as it will be `elapsedSeconds` after the snapshot was taken.
HeatCell agedHeatCell(const HeatCell& cell, double elapsedSeconds);
bool isHeatCellAlive(const HeatCell& cell, double fogSeconds);
double heatStrength(const HeatCell& cell, double fogSeconds,
                    double fadeDelaySeconds = kHeatFadeDelaySeconds);

}  // namespace alarmmap
