// The two buttons (roles in board.h): debounced short/long presses.
#pragma once

#include <stdint.h>

namespace input {

enum class Key : uint8_t { None, NextShort, NextLong, NextRepeat, MenuShort, MenuLong };

// Configures the pins (also releases them from deep-sleep wake duty).
void begin(uint16_t longPressMs);
void setLongPress(uint16_t ms);
void setRepeat(bool enable);  // NEXT held: repeat after the long press (lists)

Key poll();  // non-blocking; at most one key per call

// The press that woke the chip (NEXT or MENU pin), classified as short or long.
// Counts from boot, which is roughly when the button went down.
Key wakePress(int pin, uint32_t timeoutMs = 3000);

bool anyPressed();
void waitAllReleased(uint32_t timeoutMs = 5000);
uint32_t lastActivityMs();

}  // namespace input
