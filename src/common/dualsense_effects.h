// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "common/common_types.h"

/// A DualSense effects block (SDL_SendGamepadEffect's DS5EffectsState_t: R2 effect at byte 10, L2 at
/// 21, mic LED at 8, player LED brightness / mask at 42 / 43, lightbar RGB at 44-46, enable bits at
/// 0 and 1) a game feature publishes and the SDL driver sends to PS5 pads when it changes.
namespace Common::DualSenseEffects {

using Block = std::array<u8, 47>;

struct Channel {
    std::mutex mutex;
    Block block{};
    std::atomic<u32> serial{0};
};

inline Channel& Get() {
    static Channel channel;
    return channel;
}

/// A new block (sent once to every PS5 pad)
inline void Publish(const Block& block) {
    Channel& c = Get();
    std::scoped_lock lock{c.mutex};
    if (c.block == block && c.serial.load() != 0) {
        return;
    }
    c.block = block;
    c.serial.fetch_add(1);
}

/// The block if its serial is not `seen` (then `seen` takes it)
inline bool Take(u32& seen, Block& out) {
    Channel& c = Get();
    const u32 now = c.serial.load();
    if (now == seen) {
        return false;
    }
    std::scoped_lock lock{c.mutex};
    out = c.block;
    seen = c.serial.load();
    return true;
}

/// The PS5 pad's own input as the SDL driver last saw it (Pulse of Time reads its input report): buttons in
/// its encoding (1 square, 2 cross, 4 circle, 8 triangle, 0x10 L1, 0x20 R1, 0x40 L3, 0x80 R3), triggers and
/// stick 0..255, D-pad 0 up clockwise to 7, 8 none, touchpad 0..1919 x 0..1079.
struct Input {
    std::atomic<bool> valid{false};
    std::atomic<int> buttons{0}, l2{0}, r2{0}, lx{128}, ly{128}, dpad{8}, touch_x{0}, touch_y{0};
    std::atomic<bool> touch_down{false}, touch_click{false};
};
inline Input& GetInput() {
    static Input input;
    return input;
}

/// The overall haptic strength the touchpad sets (Pulse of Time's "master"), read by the voice-coil haptics
inline std::atomic<float>& MasterGain() {
    static std::atomic<float> gain{1.0f};
    return gain;
}

} // namespace Common::DualSenseEffects
