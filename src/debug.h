#pragma once

#include "config.h"

#include <cstdint>

bool MGS4Debug_Install(uintptr_t moduleBase, uint8_t* textBegin, uintptr_t textSize,
    const DebugConfig& config);
