// Buttons and touch (roles in the board header): debounced short/long presses.
//
// Two-button boards: NEXT and MENU, short and long press each. A PREV button
// (M5Paper) sends NEXT long, so it pages back.
// One-button boards (kSingleButton): the gestures of the one button map onto the
// same keys, so the rest of the reader doesn't care:
//   tap = NEXT short, double tap = NEXT long, hold = MENU short, long hold = MENU long.
// Touch boards also report Key::Touch with a gesture (lastTouch()); what a tap
// means depends on the screen, so the app maps it (app.cpp, touchKey()).
#pragma once

#include <stdint.h>

#include "touch_gesture.h"

namespace input {

enum class Key : uint8_t { None, NextShort, NextLong, NextRepeat, MenuShort, MenuLong, Touch };

// Configures the pins (also releases them from deep-sleep wake duty).
void begin(uint16_t longPressMs);
void setLongPress(uint16_t ms);
void setRepeat(bool enable);  // NEXT held: repeat after the long press (lists; two buttons only)

Key poll();  // non-blocking; at most one key per call

// The press that woke the chip (NEXT or MENU pin), classified as short or long.
// Counts from boot, which is roughly when the button went down.
Key wakePress(int pin, uint32_t timeoutMs = 3000);

TouchEvent lastTouch();  // the gesture behind the last Key::Touch

bool anyPressed();  // a button or a finger
void waitAllReleased(uint32_t timeoutMs = 5000);
uint32_t lastActivityMs();
void markActivity();  // e.g. woken from light sleep

}  // namespace input
