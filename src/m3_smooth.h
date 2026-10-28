// Smooth motion: on a display of 100 Hz or more, the engine presents once per
// refresh (120, 144, 165, 240 Hz...). Each present is the newest frame with
// each pixel moved back by the part of its layer's or sprite's motion still to
// come at that moment, drawn at twice the resolution so half-pixel steps show.
#pragma once

namespace m3smooth {

// 0 off, 1 on displays of 100 Hz or more, 2 always (for displays that misreport).
void enable(int level);  // installs the engine hooks; call before run_game()

}  // namespace m3smooth
