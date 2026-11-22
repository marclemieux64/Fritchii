// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Core {
class System;
}

/// Ocarina of Time (1.1.1) on a DualSense: adaptive triggers and lights from the game's state, after
/// Pulse of Time (github.com/Balemuni/pulse-of-time, GPL-3.0-or-later), whose offsets and trigger
/// effect encodings these are. A thread reads the state ~30 times a second and publishes an effects
/// block (common/dualsense_effects.h) the SDL driver sends to PS5 pads, over USB or Bluetooth.
/// FRITCHII_OOT_DUALSENSE=0 turns it off.
namespace Core::OotDualSense {

/// main_base: where OoT's main module starts (nso.cpp); another build is recognised and left alone.
void Start(Core::System& system, u64 main_base);

/// Stops the thread before guest memory goes away (System::ShutdownMainProcess).
void Stop();

} // namespace Core::OotDualSense
