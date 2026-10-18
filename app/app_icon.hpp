#pragma once

#include "snout_icon.hpp"

// Sets the window icon: the pig-snout drone over a red alert glow on a dark
// rounded tile, in several sizes for the taskbar and window switchers.
// Requires an initialised window.
void setWindowIcon(const SnoutIcon& drone);

// The same icon as a 256 px RGBA image; the caller unloads it.
Image renderAppIcon(const SnoutIcon& drone);
