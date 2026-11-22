// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/oot_dualsense.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numbers>
#include <thread>

#include "common/dualsense_effects.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core::OotDualSense {
namespace {

// Offsets from main (OoT 1.1.1, build E0F3CCFA), from Pulse of Time's Oot.cs
constexpr u64 Mod0 = 0x3C4705C;      // the module's "MOD0" tag: this build
constexpr u64 VtLinkObj = 0x4B25E18; // Link's actor vtable
constexpr u64 VtParam = 0x4B63438;   // an int parameter's vtable
constexpr u64 CameraPtr = 0x4E98068;
constexpr std::array<std::pair<u64, u64>, 5> LinkChains{{
    {0x4E371D0, 0x480},
    {0x4E94858, 0x2E0},
    {0x4E935B8, 0xEF8},
    {0x4E94860, 0x328},
    {0x4E94368, 0xD70},
}};

using Common::DualSenseEffects::Block;

// Trigger effects (11 bytes), as Pulse of Time's DualSense.cs encodes them (after Nielk1's research)
std::array<u8, 11> Off() {
    std::array<u8, 11> e{};
    e[0] = 0x05;
    return e;
}
std::array<u8, 11> Feedback(int start, int strength) {
    std::array<u8, 11> e{};
    e[0] = 0x21;
    u16 zones = 0;
    u32 forces = 0;
    const int f = std::clamp(strength - 1, 0, 7);
    for (int z = std::clamp(start, 0, 9); z < 10; ++z) {
        zones |= static_cast<u16>(1u << z);
        forces |= static_cast<u32>(f) << (3 * z);
    }
    e[1] = static_cast<u8>(zones);
    e[2] = static_cast<u8>(zones >> 8);
    std::memcpy(&e[3], &forces, 4);
    return e;
}
std::array<u8, 11> Bow(int start, int end, int strength, int snap) {
    std::array<u8, 11> e{};
    e[0] = 0x22;
    start = std::clamp(start, 0, 8);
    end = std::clamp(end, start + 1, 8);
    const u16 zones = static_cast<u16>((1u << start) | (1u << end));
    const u16 forces =
        static_cast<u16>(std::clamp(strength - 1, 0, 7) | (std::clamp(snap - 1, 0, 7) << 3));
    e[1] = static_cast<u8>(zones);
    e[2] = static_cast<u8>(zones >> 8);
    e[3] = static_cast<u8>(forces);
    e[4] = static_cast<u8>(forces >> 8);
    return e;
}

struct Reader {
    Core::Memory::Memory& memory; // (taken each cycle: the process's memory is set up after main loads)
    u64 main;

    u64 P(u64 va) const {
        if (va == 0 || !memory.IsValidVirtualAddressRange(va, 8)) {
            return 0;
        }
        return memory.Read64(va);
    }
    bool ReadBytes(u64 va, void* out, size_t n) const {
        return va != 0 && memory.IsValidVirtualAddressRange(va, n) && memory.ReadBlock(va, out, n);
    }
    bool IsThisBuild() const {
        char tag[4]{};
        return ReadBytes(main + Mod0, tag, 4) && std::memcmp(tag, "MOD0", 4) == 0;
    }
    u64 Link() const {
        for (const auto& [global, member] : LinkChains) {
            const u64 holder = P(main + global);
            const u64 actor = holder ? P(holder + member) : 0;
            if (actor && P(actor) == main + VtLinkObj) {
                return actor;
            }
        }
        return 0;
    }
    s32 Param(u64 p) const {
        if (!p || P(p) != main + VtParam) {
            return -1;
        }
        s32 v = -1;
        return ReadBytes(p + 8, &v, 4) ? v : -1;
    }
    float FovDegrees() const {
        const u64 cam = P(P(main + CameraPtr) + 0x68);
        float rad = 0.0f;
        return cam && ReadBytes(cam + 0x130, &rad, 4) ? rad * 180.0f / std::numbers::pi_v<float>
                                                      : 0.0f;
    }
};

std::atomic<bool> g_stop{false};
std::thread g_thread;
std::mutex g_mutex;

void Run(Core::System& system, u64 main_base) {
    Common::SetCurrentThreadName("OotDualSense");
    bool checked = false;
    std::chrono::steady_clock::time_point aim_since{};
    bool was_aiming = false;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        // Main is loaded before the process runs: nothing is read until the system is on and not stopping
        if (!system.IsPoweredOn() || system.IsShuttingDown() || !system.ApplicationProcess()) {
            continue;
        }
        Reader r{system.ApplicationMemory(), main_base};
        if (!checked) {
            if (!r.IsThisBuild()) {
                continue; // (the module may not be readable yet; another build never matches)
            }
            checked = true;
            LOG_INFO(Input, "OoT DualSense: triggers and lights from the game (Pulse of Time's mapping)");
        }
        Block b{};
        const u64 link = r.Link();
        s32 life = -1, max_life = -1;
        if (link) {
            const u64 state = r.P(link + 8);
            const u64 pars = state ? r.P(state + 0x6A0) : 0;
            if (pars) {
                life = r.Param(r.P(pars + 0x1C10));
                max_life = r.Param(r.P(pars + 0x1C18));
            }
        }
        const bool in_game = link != 0 && max_life > 0 && life >= 0;
        static s32 logged_life = -2, logged_max = -2;
        if (in_game && (life != logged_life || max_life != logged_max)) {
            logged_life = life;
            logged_max = max_life;
            LOG_INFO(Input, "OoT DualSense: life {} of {}", life, max_life);
        }
        b[0] = 0x04 | 0x08; // both trigger effects present
        if (!in_game) {
            const auto off = Off();
            std::copy(off.begin(), off.end(), b.begin() + 10);
            std::copy(off.begin(), off.end(), b.begin() + 21);
            Common::DualSenseEffects::Publish(b);
            was_aiming = false;
            continue;
        }

        // R2: the bow's draw while aiming (the camera narrows to 40 degrees), held 120 ms
        const auto now = std::chrono::steady_clock::now();
        const bool aiming_now = std::abs(r.FovDegrees() - 40.0f) < 0.6f;
        if (aiming_now && !was_aiming) {
            aim_since = now;
        }
        was_aiming = aiming_now;
        const bool aiming = aiming_now && now - aim_since >= std::chrono::milliseconds(120);
        const auto right = aiming ? Bow(0, 7, 8, 8) : Off();
        const auto left = Off();
        std::copy(right.begin(), right.end(), b.begin() + 10);
        std::copy(left.begin(), left.end(), b.begin() + 21);

        // Lights: player LEDs show the hearts left (of 5), the lightbar is green, a red pulse when low
        // (life counts quarter hearts: 1 heart or less as the game's own beep, or a quarter of the
        // hearts with many), the mute LED pulses at half a heart or less
        b[1] = 0x01 | 0x04 | 0x10;
        const float frac = std::clamp(static_cast<float>(life) / static_cast<float>(max_life), 0.0f, 1.0f);
        const int lit = life > 0 ? std::clamp(static_cast<int>(std::ceil(frac * 5.0f)), 1, 5) : 0;
        b[42] = 0; // player LEDs bright
        b[43] = static_cast<u8>((1u << lit) - 1);
        const bool low = life <= 4 || life * 4 <= max_life;
        b[8] = life <= 2 ? 2 : 0;
        if (low) {
            const float t = std::chrono::duration<float>(now.time_since_epoch()).count();
            const float pulse = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(t * 2.0f * std::numbers::pi_v<float>));
            b[44] = static_cast<u8>(255 * pulse);
            b[45] = 0;
            b[46] = 0;
        } else {
            b[44] = 0;
            b[45] = 170;
            b[46] = 40;
        }
        Common::DualSenseEffects::Publish(b);
    }
    // Triggers free and lights left as they are when the game stops
    Block b{};
    b[0] = 0x04 | 0x08;
    const auto off = Off();
    std::copy(off.begin(), off.end(), b.begin() + 10);
    std::copy(off.begin(), off.end(), b.begin() + 21);
    Common::DualSenseEffects::Publish(b);
}

} // namespace

void Start(Core::System& system, u64 main_base) {
    const char* const e = std::getenv("FRITCHII_OOT_DUALSENSE");
    if (e && e[0] == '0') {
        return;
    }
    Stop();
    std::scoped_lock lock{g_mutex};
    g_stop = false;
    g_thread = std::thread([&system, main_base] { Run(system, main_base); });
}

void Stop() {
    std::scoped_lock lock{g_mutex};
    g_stop = true;
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

} // namespace Core::OotDualSense
