// Touch screen (GT911, M5 boards). Coordinates are page pixels (kPageW x kPageH,
// portrait). On boards without touch every call reports nothing.
#pragma once

#include <stdint.h>

namespace touch {

bool begin();  // finds the controller (I2C 0x5D or 0x14); false if there is none

// The first finger, if one is down. Only talks to the controller when its INT
// line says there is news (or a finger was down), so it is cheap to poll.
bool read(int &x, int &y);

// INT asserted: a report is waiting.
bool pending();
int interruptPin();  // the light-sleep wake source; -1 without touch

// Settings -> Flip screen: coordinates turned 180 degrees with the image.
void setFlipped(bool flipped);

}  // namespace touch
