// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A port of Pulse of Time's controller output (github.com/Balemuni/pulse-of-time, GPL-3.0-or-later, commit
// 6d2cf17): Oot.cs (the game's state), GameData.cs (save fields), Actions.cs (Link's action verbs) and the
// trigger and light parts of Daemon.cs, with their numbers; Combat.cs (enemies, bosses, the shield's brace),
// Scene.cs and Events.cs (places, songs, warps), and the trigger, light, LED and mute LED parts of Moments.cs.
// Quad.cs (trigger bursts) with every pulse those files send, and what reads the pad's own input (R2's pull,
// buttons, the stick, the touchpad: the bow's draw, dry fire, sword swings and the spin charge, ocarina notes,
// struggling, a hooked fish, the pause menu, Navi's compass, the touchpad's strength). Not ported: the haptic
// textures (Fritchii plays the game's own HD rumble) and the pulses timed by them (swim strokes, Epona's hooves).

#include "core/oot_dualsense.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/dualsense_effects.h"
#include "common/logging.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/memory.h"

namespace Core::OotDualSense {
namespace {

using Effect = std::array<u8, 11>;
using Block = Common::DualSenseEffects::Block;
using Clock = std::chrono::steady_clock;
constexpr double Pi = std::numbers::pi;

// ---------- Effects (DualSense.cs) ----------
Effect B(std::initializer_list<u8> p) {
    Effect e{};
    std::copy(p.begin(), p.end(), e.begin());
    return e;
}
Effect Off() {
    return B({0x05});
}
Effect Feedback(int start, int strength) {
    if (strength <= 0) {
        return Off();
    }
    u32 zones = 0, force = 0;
    for (int i = std::clamp(start, 0, 9); i < 10; i++) {
        zones |= 1u << i;
        force |= static_cast<u32>((strength - 1) & 7) << (3 * i);
    }
    return B({0x21, static_cast<u8>(zones), static_cast<u8>(zones >> 8), static_cast<u8>(force),
              static_cast<u8>(force >> 8), static_cast<u8>(force >> 16), static_cast<u8>(force >> 24)});
}
Effect Slope(int start, int end, int from, int to) {
    start = std::clamp(start, 0, 9);
    end = std::clamp(end, start, 9);
    u32 zones = 0, force = 0;
    for (int i = start; i <= end; i++) {
        const int s = end == start ? to : from + (to - from) * (i - start) / (end - start);
        if (s <= 0) {
            continue;
        }
        zones |= 1u << i;
        force |= static_cast<u32>((s - 1) & 7) << (3 * i);
    }
    return zones == 0 ? Off()
                      : B({0x21, static_cast<u8>(zones), static_cast<u8>(zones >> 8), static_cast<u8>(force),
                           static_cast<u8>(force >> 8), static_cast<u8>(force >> 16), static_cast<u8>(force >> 24)});
}
Effect Weapon(int start, int end, int strength) {
    if (strength <= 0) {
        return Off();
    }
    start = std::clamp(start, 2, 7);
    end = std::clamp(end, start + 1, 8);
    const u32 mask = (1u << start) | (1u << end);
    return B({0x25, static_cast<u8>(mask), static_cast<u8>(mask >> 8),
              static_cast<u8>(std::clamp(strength, 1, 8) - 1)});
}
Effect Vibration(int start, int amplitude, int frequency) {
    if (amplitude <= 0 || frequency <= 0) {
        return Off();
    }
    u32 zones = 0, amp = 0;
    for (int i = std::clamp(start, 0, 9); i < 10; i++) {
        zones |= 1u << i;
        amp |= static_cast<u32>((amplitude - 1) & 7) << (3 * i);
    }
    return B({0x26, static_cast<u8>(zones), static_cast<u8>(zones >> 8), static_cast<u8>(amp),
              static_cast<u8>(amp >> 8), static_cast<u8>(amp >> 16), static_cast<u8>(amp >> 24), 0, 0,
              static_cast<u8>(std::clamp(frequency, 1, 255))});
}
Effect Bow(int start, int end, int strength, int snap) {
    start = std::clamp(start, 0, 8);
    end = std::clamp(end, start + 1, 8);
    const u32 zones = (1u << start) | (1u << end);
    const u32 force = static_cast<u32>((std::clamp(strength, 1, 8) - 1) & 7) |
                      static_cast<u32>(((std::clamp(snap, 1, 8) - 1) & 7) << 3);
    return B({0x22, static_cast<u8>(zones), static_cast<u8>(zones >> 8), static_cast<u8>(force),
              static_cast<u8>(force >> 8)});
}
Effect Machine(int start, int end, int amp_a, int amp_b, int frequency, int period) {
    start = std::clamp(start, 0, 8);
    end = std::clamp(end, start + 1, 9);
    const u32 zones = (1u << start) | (1u << end);
    const u32 amps = static_cast<u32>(std::clamp(amp_a, 0, 7) & 7) |
                     static_cast<u32>((std::clamp(amp_b, 0, 7) & 7) << 3);
    return B({0x27, static_cast<u8>(zones), static_cast<u8>(zones >> 8), static_cast<u8>(amps),
              static_cast<u8>(std::clamp(frequency, 1, 255)), static_cast<u8>(std::clamp(period, 0, 255))});
}
// The same effect with its resistance scaled by k (feedback, slope, weapon, bow); vibrations as they are
Effect Scale(const Effect& e, float k) {
    if (std::abs(k - 1.0f) < 0.01f || !(e[0] == 0x21 || e[0] == 0x22 || e[0] == 0x25)) {
        return e;
    }
    if (k < 0.05f) {
        return Off();
    }
    Effect s = e;
    const auto f = [k](int strength) { return std::clamp(static_cast<int>(std::round(strength * k)), 1, 8); };
    switch (s[0]) {
    case 0x21: {
        const u32 zones = static_cast<u32>(s[1] | s[2] << 8);
        u32 force = 0, scaled = 0;
        std::memcpy(&force, &s[3], 4);
        for (int i = 0; i < 10; i++) {
            if ((zones >> i & 1) != 0) {
                scaled |= static_cast<u32>(f(static_cast<int>(force >> 3 * i & 7) + 1) - 1) << 3 * i;
            }
        }
        std::memcpy(&s[3], &scaled, 4);
        break;
    }
    case 0x25:
        s[3] = static_cast<u8>(f(s[3] + 1) - 1);
        break;
    case 0x22:
        s[3] = static_cast<u8>((f((s[3] & 7) + 1) - 1) | (f((s[3] >> 3 & 7) + 1) - 1) << 3);
        break;
    }
    return s;
}

// ---------- The game's state (Oot.cs), OoT 1.1.1 (build E0F3CCFA) ----------
constexpr u64 Mod0 = 0x3C4705C;
constexpr u64 VtLinkObj = 0x4B25E18, VtParam = 0x4B63438, CameraPtr = 0x4E98068, WorldMgr = 0x4E7EAB0;
constexpr u64 VtCctProperty = 0x4C34F10, VtCctBody = 0x4C34658;
constexpr u64 VerbTable = 0x4B4ABC8;
constexpr std::array<std::pair<u64, u64>, 5> LinkPaths{
    {{0x4E371D0, 0x480}, {0x4E94858, 0x2E0}, {0x4E935B8, 0xEF8}, {0x4E94860, 0x328}, {0x4E94368, 0xD70}}};

struct Mem {
    Core::Memory::Memory& m;
    u64 main;

    bool Read(u64 va, void* out, size_t n) const {
        return va != 0 && m.IsValidVirtualAddressRange(va, n) && m.ReadBlock(va, out, n);
    }
    u64 P(u64 va) const {
        u64 v = 0;
        return Read(va, &v, 8) ? v : 0;
    }
    u32 U32(u64 va) const {
        u32 v = 0;
        return Read(va, &v, 4) ? v : 0;
    }
    std::string Str(u64 va, size_t max = 64) const {
        std::string out;
        char c = 0;
        while (out.size() < max && Read(va + out.size(), &c, 1) && c != 0) {
            out.push_back(c);
        }
        return out;
    }
};

struct State {
    bool in_game{};
    int life = -1, life_max = -1;
    float fov{}, vx{}, vy{}, vz{}, x{}, y{}, z{};
    float SpeedH() const {
        return std::sqrt(vx * vx + vz * vz);
    }
};
struct World {
    bool indoor{}, under_roof{};
    int seconds = -1;
};

struct Oot {
    Mem mem;
    u64 link = 0;

    bool StillThere() const {
        char b[4]{};
        return mem.Read(mem.main + Mod0, b, 4) && std::memcmp(b, "MOD0", 4) == 0;
    }
    u64 LinkActor() const {
        for (const auto& [g, member] : LinkPaths) {
            const u64 holder = mem.P(mem.main + g);
            const u64 actor = holder ? mem.P(holder + member) : 0;
            if (actor && mem.P(actor) == mem.main + VtLinkObj) {
                return actor;
            }
        }
        return 0;
    }
    int Param(u64 p) const {
        if (!p || mem.P(p) != mem.main + VtParam) {
            return -1;
        }
        s32 v = -1;
        return mem.Read(p + 8, &v, 4) ? v : -1;
    }
    void Velocity(u64 actor, float& x, float& y, float& z) const {
        x = y = z = 0;
        u64 p = mem.P(actor + 0x270);
        for (const u64 off : {0x50ull, 0x20ull, 0x10ull, 0x8ull}) {
            p = p ? mem.P(p + off) : 0;
        }
        if (!p || mem.P(p) != mem.main + VtCctProperty) {
            return;
        }
        const u64 body = mem.P(p + 0x20);
        float v[3]{};
        if (!body || mem.P(body) != mem.main + VtCctBody || !mem.Read(body + 0xE0, v, 12)) {
            return;
        }
        if (std::isfinite(v[0] + v[1] + v[2]) && std::abs(v[0]) + std::abs(v[1]) + std::abs(v[2]) < 1000) {
            x = v[0];
            y = v[1];
            z = v[2];
        }
    }
    State Read() {
        State s;
        link = LinkActor();
        if (link) {
            const u64 st = mem.P(link + 8);
            const u64 pars = st ? mem.P(st + 0x6A0) : 0;
            if (pars) {
                s.life = Param(mem.P(pars + 0x1C10));
                s.life_max = Param(mem.P(pars + 0x1C18));
            }
        }
        const u64 mgr = mem.P(mem.main + CameraPtr);
        const u64 cam = mgr ? mem.P(mgr + 0x68) : 0;
        float rad = 0;
        if (cam && mem.Read(cam + 0x130, &rad, 4)) {
            s.fov = rad * (180.0f / std::numbers::pi_v<float>);
        }
        if (link) {
            Velocity(link, s.vx, s.vy, s.vz);
            float p[3]{};
            if (mem.Read(link + 0x31C, p, 12)) {
                s.x = p[0];
                s.y = p[1];
                s.z = p[2];
            }
        }
        s.in_game = link != 0 && s.life_max > 0;
        return s;
    }
    // Link's action verb in BSA layer 0..4: [slot+0x28], or [slot+0x4C] when that is 282
    int Verb(int layer) const {
        const u64 bsa = mem.P(mem.P(link + 0x270) + 0x188);
        const u64 slot = bsa ? mem.P(bsa + 0x1C0 + 8 * static_cast<u64>(layer)) : 0;
        if (!slot) {
            return -1;
        }
        s32 v = -1;
        if (!mem.Read(slot + 0x28, &v, 4)) {
            return -1;
        }
        if (v == 0x11A && !mem.Read(slot + 0x4C, &v, 4)) {
            return -1;
        }
        return v;
    }
    // The first playing animation command's name and frame
    bool FirstAnim(std::string& name, float& frame) const {
        const u64 a = mem.P(mem.P(mem.P(mem.P(link + 0x270) + 0x40) + 0x18) + 0x18);
        s32 count = 0;
        if (!a || !mem.Read(a + 0x18, &count, 4) || count <= 0) {
            return false;
        }
        const u64 cmd = mem.P(mem.P(a + 0x20));
        if (!cmd) {
            return false;
        }
        const u64 n = mem.P(cmd + 0x10);
        name = n ? mem.Str(n) : std::string{};
        frame = 0;
        mem.Read(cmd + 0x10C, &frame, 4);
        return true;
    }
    // The running event's flowchart name ([[main+0x4E94840]+0x338]), "" = none
    std::string EventName() const {
        const u64 mgr = mem.P(mem.main + 0x4E94840);
        const u64 p = mgr ? mem.P(mgr + 0x338) : 0;
        if (!p) {
            return {};
        }
        std::string n = mem.Str(p);
        for (const char ch : n) {
            if (ch < 0x20 || ch >= 0x7F) {
                return {};
            }
        }
        return n;
    }
    World ReadWorld() const {
        World w;
        const u64 comps = link ? mem.P(link + 0x270) : 0;
        const u64 env = comps ? mem.P(comps + 0x460) : 0;
        u32 f = 0;
        if (env && mem.Read(env + 0xA8, &f, 4)) {
            w.indoor = (f >> 14 & 1) != 0;
            w.under_roof = (f >> 5 & 1) != 0;
        }
        const u64 t = mem.P(mem.P(mem.P(mem.main + WorldMgr) + 0x578) + 0xD48);
        u8 c9 = 0;
        if (t && mem.Read(t + 9, &c9, 1)) {
            s32 sec = -1;
            if (mem.Read(t + (((c9 >> 1) & 1) != 0 ? 0x50 : 0x20) + 8, &sec, 4)) {
                w.seconds = sec;
            }
        }
        return w;
    }
};

// ---------- GameData.cs: the save fields, by MurmurHash3_32 (seed 0) of their names ----------
u32 Hash(std::string_view s) {
    constexpr u32 c1 = 0xCC9E2D51, c2 = 0x1B873593;
    const auto rotl = [](u32 v, int r) { return (v << r) | (v >> (32 - r)); };
    u32 h = 0;
    size_t i = 0;
    for (; i + 4 <= s.size(); i += 4) {
        u32 k;
        std::memcpy(&k, s.data() + i, 4);
        k = rotl(k * c1, 15) * c2;
        h = rotl(h ^ k, 13) * 5 + 0xE6546B64;
    }
    u32 t = 0;
    switch (s.size() & 3) {
    case 3:
        t ^= static_cast<u32>(static_cast<u8>(s[i + 2])) << 16;
        [[fallthrough]];
    case 2:
        t ^= static_cast<u32>(static_cast<u8>(s[i + 1])) << 8;
        [[fallthrough]];
    case 1:
        t ^= static_cast<u8>(s[i]);
        h ^= rotl(t * c1, 15) * c2;
        break;
    }
    h ^= static_cast<u32>(s.size());
    h = (h ^ (h >> 16)) * 0x85EBCA6B;
    h = (h ^ (h >> 13)) * 0xC2B2AE35;
    return h ^ (h >> 16);
}

class GameData {
public:
    enum Kind { Int, Bool, Float, Enum };
    explicit GameData(Mem m) : mem{m} {}

    std::optional<int> GetInt(u32 h) {
        const u64 a = Addr(Int, h);
        if (!a) {
            return std::nullopt;
        }
        return static_cast<int>(mem.U32(a));
    }
    std::optional<u32> GetEnum(u32 h) {
        const u64 a = Addr(Enum, h);
        if (!a) {
            return std::nullopt;
        }
        return mem.U32(a);
    }
    std::optional<float> GetFloat(u32 h) {
        const u64 a = Addr(Float, h);
        if (!a) {
            return std::nullopt;
        }
        const u32 v = mem.U32(a);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    std::optional<bool> GetBool(u32 h) {
        const u64 a = Addr(Bool, h);
        u8 b = 0;
        if (!a || !mem.Read(a, &b, 1)) {
            return std::nullopt;
        }
        return b != 0;
    }
    // String64 field: entry at manager+0x608, objects 0x108 apart, char* at +0x68
    std::optional<std::string> GetStr(u32 hash) {
        if (!Ensure()) {
            return std::nullopt;
        }
        const u64 e = mgr + 0x608;
        const u32 count = mem.U32(e + 8), size = mem.U32(e + 0x24);
        const u64 objects = mem.P(e + 0x10), table = mem.P(e + 0x18);
        if (count == 0 || count > 0x10000 || size == 0 || size > 0x100000 || !objects || !table) {
            return std::nullopt;
        }
        for (u32 i = 0, slot = hash % size; i < size; i++, slot = slot + 1 >= size ? 0 : slot + 1) {
            const u32 h = mem.U32(table + slot * 8ull);
            if (h == 0) {
                return std::nullopt;
            }
            if (h != hash) {
                continue;
            }
            const u32 idx = mem.U32(table + slot * 8ull + 4);
            if (idx >= count) {
                return std::nullopt;
            }
            const u64 text = mem.P(objects + idx * 0x108ull + 0x68);
            if (!text) {
                return std::nullopt;
            }
            return mem.Str(text);
        }
        return std::nullopt;
    }

private:
    static constexpr u64 MgrPtr = 0x4C7E180, IntEntry = 0xC8, ScanBytes = 0x800;
    static constexpr std::array<u64, 4> Vtables{0, 0x4C1F140, 0x4C1F3C0, 0x4C1F500};
    static constexpr std::array<std::pair<u64, u64>, 4> Layout{{{0x28, 0x1C}, {0x18, 0x16}, {0x28, 0x1C}, {0x38, 0x1C}}};

    bool Ensure() {
        const u64 m = mem.P(mem.P(mem.main + MgrPtr));
        if (!m) {
            return false;
        }
        const auto now = Clock::now();
        if (m == mgr && index[0] && now - built < std::chrono::seconds(10)) {
            return true;
        }
        mgr = m;
        built = now;
        std::vector<u8> scan(ScanBytes);
        mem.Read(m, scan.data(), scan.size());
        for (int k = 0; k < 4; k++) {
            index[k].reset();
            u64 entry = 0;
            if (k == 0) {
                entry = m + IntEntry;
            } else {
                for (u64 o = 0; o + 0x28 <= scan.size(); o += 8) {
                    u64 v;
                    std::memcpy(&v, scan.data() + o, 8);
                    if (v == mem.main + Vtables[k]) {
                        entry = m + o;
                        break;
                    }
                }
            }
            if (!entry) {
                continue;
            }
            const u32 count = mem.U32(entry + 8), size = mem.U32(entry + 0x24);
            const u64 objs = mem.P(entry + 0x10), table = mem.P(entry + 0x18);
            if (count == 0 || count > 0x10000 || size == 0 || size > 0x100000 || !objs || !table) {
                continue;
            }
            std::vector<u8> raw(static_cast<size_t>(size) * 8);
            if (!mem.Read(table, raw.data(), raw.size())) {
                continue;
            }
            std::unordered_map<u32, u32> d;
            d.reserve(count);
            for (u32 i = 0; i < size; i++) {
                u32 h, idx;
                std::memcpy(&h, raw.data() + i * 8, 4);
                std::memcpy(&idx, raw.data() + i * 8 + 4, 4);
                if (h != 0 && idx < count) {
                    d[h] = idx;
                }
            }
            index[k] = std::move(d);
            objects[k] = objs;
        }
        return index[0].has_value();
    }
    u64 Addr(Kind k, u32 hash) {
        if (!Ensure() || !index[k]) {
            return 0;
        }
        const auto it = index[k]->find(hash);
        if (it == index[k]->end()) {
            return 0;
        }
        return objects[k] + static_cast<u64>(it->second) * Layout[k].first + Layout[k].second;
    }

    Mem mem;
    u64 mgr = 0;
    Clock::time_point built{};
    std::array<std::optional<std::unordered_map<u32, u32>>, 4> index;
    std::array<u64, 4> objects{};
};

// The verb names from the game's table (Verbs.cs: index = the value in a BSA slot), an array of string
// pointers whose stride is the one with "None" and "Walk" first
std::vector<std::string> ReadVerbNames(const Mem& m) {
    for (const u64 stride : {8ull, 16ull, 24ull, 32ull}) {
        if (m.Str(m.P(m.main + VerbTable)) != "None" || m.Str(m.P(m.main + VerbTable + stride)) != "Walk") {
            continue;
        }
        std::vector<std::string> names;
        for (u64 i = 0; i < 400; ++i) {
            const u64 ptr = m.P(m.main + VerbTable + i * stride);
            std::string n = ptr ? m.Str(ptr) : std::string{};
            if (n.empty()) {
                break;
            }
            names.push_back(std::move(n));
        }
        return names;
    }
    return {};
}

using Rgb = std::array<float, 3>;
Rgb Mix(const Rgb& a, const Rgb& b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}
// Outdoors the light follows the sun (Daemon.TimeOfDay)
Rgb TimeOfDay(const Rgb& c, int seconds) {
    const double h = std::fmod(seconds / 3600.0, 24.0);
    if (h >= 5 && h < 7) {
        return Mix(c, {255, 120, 70}, 0.35f * static_cast<float>(1 - std::abs(h - 6)));
    }
    if (h >= 7 && h < 17) {
        return c;
    }
    if (h >= 17 && h < 19.5) {
        return Mix(c, {255, 100, 30}, 0.4f * static_cast<float>(std::clamp((h - 17) / 1.2, 0.0, 1.0)));
    }
    return Mix({c[0] * 0.45f, c[1] * 0.45f, c[2] * 0.5f}, {25, 35, 130}, 0.4f);
}

bool ContainsI(const std::string& hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != hay.end();
}

// ---------- Quad.cs: trigger bursts laid over the resistance ----------
class Quad {
public:
    // A trigger burst: amplitude 0..8 per side, frequency Hz, length; delay to schedule it later
    void Pulse(long long now, float amp_l, float amp_r, int freq, int ms, int delay = 0, bool decay = true, bool tremor = false) {
        if (amp_l < 0.5f && amp_r < 0.5f) return;
        events.push_back({amp_l, amp_r, freq, now + delay, now + delay + ms, decay, tremor});
    }
    // A direction on the four points: side -1 left .. 1 right, front -1 behind .. 1 ahead
    struct Placed {
        float palm, pan, l2, r2;
    };
    static Placed Place(float side, float front, float strength) {
        const float ahead = (std::max)(0.0f, front), behind = (std::max)(0.0f, -front);
        const float palm = strength * (0.5f + 0.5f * behind);
        const float trig = 8 * strength * ahead;
        return {palm, side, trig * std::clamp(1 - side, 0.0f, 1.0f), trig * std::clamp(1 + side, 0.0f, 1.0f)};
    }
    // The active bursts over this frame's effects (a tremor alternates frame by frame with the resistance)
    void Apply(long long now, Effect& r, Effect& l) {
        frame++;
        constexpr float gain = 1.0f; // tuning.json's triggers.tremor
        float al = 0, ar = 0;
        int fl = 40, fr = 40;
        bool tl = false, tr = false;
        for (size_t i = events.size(); i-- > 0;) {
            const Ev& e = events[i];
            if (now >= e.end) {
                events.erase(events.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (now < e.start) continue;
            const float k = e.decay ? 1 - (now - e.start) / static_cast<float>((std::max)(1ll, e.end - e.start)) * 0.7f : 1.0f;
            if (e.l * k > al) { al = e.l * k; fl = e.freq; tl = e.tremor; }
            if (e.r * k > ar) { ar = e.r * k; fr = e.freq; tr = e.tremor; }
        }
        al *= gain;
        ar *= gain;
        const bool odd = (frame & 1) == 1;
        if (al >= 1 && !(tl && odd)) l = Vibration(0, static_cast<int>(std::round((std::min)(8.0f, al))), fl);
        if (ar >= 1 && !(tr && odd)) r = Vibration(0, static_cast<int>(std::round((std::min)(8.0f, ar))), fr);
    }

private:
    struct Ev {
        float l, r;
        int freq;
        long long start, end;
        bool decay, tremor;
    };
    std::vector<Ev> events;
    long long frame = 0;
};

// ---------- Actions.cs: everything driven by Link's action verbs ----------
class Actions {
public:
    struct Out {
        std::optional<Effect> r, l;
        std::optional<Rgb> light;
        float light_mix{};
        bool free_triggers{};
    };

    long long kick = 0, jolt = 0, r_tick = 0, flash_until = 0;
    Rgb flash_color{};
    bool busy{}, in_event{}, raining{};
    float air_left = -1;
    bool flying{}, slippery{}, sinking{};
    long long grabbed_until = 0, terror_until = 0;
    int grab_presses = 0;
    bool hands_busy{}, carried{};
    float PosSpeed() const {
        return pos_vel;
    }

    bool HasVerb(const std::string& v) const {
        return now_set.contains(v);
    }
    void StartRain(long long now, int ms) {
        rain_until = now + ms;
    }

    // paused: Pulse of Time passes no haptics then, so none of its one-shot pulses play
    Out Update(long long now, const State& s, const std::array<int, 5>& verbs,
               const std::vector<std::string>& names, const std::string& anim, float anim_frame,
               const std::string& armor, const std::string& map, Quad& q, int buttons, int prev_buttons,
               bool stick_idle, bool paused) {
        before = now_set;
        now_set.clear();
        // a slot reads empty for a frame now and then: a verb counts as active until gone for 150 ms
        for (const int v : verbs) {
            if (v > 0 && static_cast<size_t>(v) < names.size()) {
                seen[names[static_cast<size_t>(v)]] = now;
            }
        }
        for (const auto& [name, t] : seen) {
            if (now - t <= 150) {
                now_set.insert(name);
            }
        }
        const float speed = (std::max)(s.SpeedH(), pos_vel * 0.9f);
        const auto has = [&](std::initializer_list<const char*> v) {
            for (const char* n : v) {
                if (now_set.contains(n)) {
                    return true;
                }
            }
            return false;
        };
        const auto entered = [&](std::initializer_list<const char*> v) {
            for (const char* n : v) {
                if (now_set.contains(n) && !before.contains(n)) {
                    return true;
                }
            }
            return false;
        };

        const bool hx = !paused; // (Pulse of Time's haptics are on: its one-shots and pulses play)
        if (hx) {
            if (entered({"Drawn"})) q.Pulse(now, 0, 5, 60, 70, 200);
            if (entered({"Sheathed"})) q.Pulse(now, 0, 3, 80, 40, 120);
            if (entered({"JumpAttack", "FallAttack"})) q.Pulse(now, 8, 8, 35, 150, 200);
            if (entered({"ThrustAttack"})) q.Pulse(now, 2, 7, 70, 60);
            if (entered({"Rolling"})) {
                q.Pulse(now, 4, 0, 40, 60, 0);
                q.Pulse(now, 0, 4, 40, 60, 150);
            }
            if (entered({"InterruptGuard", "InterruptCrouchGuard"})) q.Pulse(now, 8, 3, 35, 140, 0, true, true);
            if (entered({"InterruptRepelled"})) q.Pulse(now, 0, 8, 90, 90);
            if (entered({"OpenDoor"})) q.Pulse(now, 4, 4, 25, 700, 0, false, true);
            if (entered({"InterruptSongOfTheStormNoticeOutside", "InterruptSongOfTheStormNoticeInside"})) q.Pulse(now, 6, 6, 30, 400, 0, true, true);
            if (entered({"Dash"})) q.Pulse(now, 3, 3, 45, 90);
            if (entered({"DodgeAttack"})) {
                const int d = coin(rng) ? 2 : 1;
                q.Pulse(now, d == 2 ? 4 : 0, d == 1 ? 4 : 0, 80, 60);
            }
            if (entered({"BackStep"})) q.Pulse(now, 2, 2, 60, 50, 60);
            if (entered({"WallJump"})) {
                wall_side = -wall_side;
                q.Pulse(now, wall_side < 0 ? 6 : 0, wall_side > 0 ? 6 : 0, 60, 70);
            }
            if (entered({"ContinuousJump01", "ContinuousJump02", "TurnJump"})) {
                chain = now - last_chain < 1500 ? (std::min)(chain + 1, 6) : 1;
                last_chain = now;
                q.Pulse(now, 1.0f + chain, 1.0f + chain, 60 + 20 * chain, 50);
            }
            if (entered({"RideJumpAttack"})) q.Pulse(now, 8, 8, 35, 150, 200);
            if (entered({"InterruptCrashedByObject"})) q.Pulse(now, 8, 8, 35, 140);
            if (entered({"InterruptFallFromRidable", "ForceDismount"})) q.Pulse(now, 5, 5, 35, 120);
            if (entered({"InterruptHammer"})) q.Pulse(now, 8, 8, 30, 160);
        }
        // one-shot reactions that reach the triggers and the light (the sounds are Pulse of Time's own)
        if (entered({"InterruptGuard", "InterruptCrouchGuard", "InterruptNayruLoveGuard"})) {
            kick = now + 140;
        }
        if (entered({"InterruptGuardJustRepelled", "InterruptGuardAndRiposte"})) {
            kick = now + 100;
            flash_color = {255, 255, 255};
            flash_until = now + 150;
        }
        if (entered({"InterruptGuardBreak", "InterruptNayruLoveGuardBreak"})) {
            jolt = now + 250;
        }
        if (entered({"InterruptRepelled"})) {
            r_tick = now + 120;
        }
        if (entered({"UseNayruLove"})) {
            nayru_until = now + 8000;
        }
        if (entered({"InterruptBigFall", "InterruptFallDamage"})) {
            jolt = now + 150;
        }
        if (entered({"InterruptAbyssFall"})) {
            fade_abyss = now + 1600;
        }
        if (entered({"InterruptLightArrow"})) {
            flash_color = {255, 250, 200};
            flash_until = now + 500;
        }
        if (entered({"InterruptSongOfTheStormNoticeOutside", "InterruptSongOfTheStormNoticeInside"})) {
            rain_until = now + 90000;
            flash_color = {230, 230, 255};
            flash_until = now + 200;
        }
        if (entered({"InterruptCrashedByObject"})) {
            jolt = now + 120;
            flash_color = {255, 255, 255};
            flash_until = now + 120;
        }
        if (entered({"InterruptHammer"})) {
            r_tick = now + 140;
            jolt = now + 80;
        }
        if (entered({"AttachChemical", "InterruptLightChemical"})) {
            flash_color = {255, 140, 40};
            flash_until = now + 300;
        }
        if (entered({"InterruptGuardFaile"})) {
            jolt = now + 150;
        }
        if (entered({"InterruptGuardBeam"})) {
            kick = now + 300;
            flash_color = {255, 255, 200};
            flash_until = now + 200;
        }
        if (has({"InterruptRestraintRideOnPlayer", "InterruptRestraintRideOnEnemy", "InterruptRestraintRideOnSword"})) {
            grabbed_until = (std::max)(grabbed_until, now + 300);
        }
        if (has({"InterruptTerror"})) {
            terror_until = (std::max)(terror_until, now + 300);
        }
        bool grabbed = now < grabbed_until;
        const bool terror = now < terror_until;
        if (grabbed && !was_grabbed) {
            grab_presses = 0;
            if (hx) q.Pulse(now, 8, 8, 30, 120);
        }
        if (grabbed && (buttons & ~prev_buttons & 0x3F) != 0) { // every button mashed jerks against the grip
            grab_presses++;
            const bool left = grab_presses % 2 == 0;
            if (hx) q.Pulse(now, left ? 6 : 0, left ? 0 : 6, 60, 70);
        }
        if (grabbed && entered({"ReleaseRestraint", "Unstick"})) {
            grabbed_until = 0;
            grabbed = false;
        }
        if (terror && !was_terror) {
            flash_color = {150, 150, 160};
            flash_until = now + 250;
        }
        if (terror && hx) q.Pulse(now, 2, 2, 220, 30, 0, false, true);
        was_grabbed = grabbed;
        was_terror = terror;
        // events (dialogue, cutscenes) play "Evt..." animations and run with no action verb at all
        in_event = anim.starts_with("Evt");
        if (rain_until > 0 && now > rain_until) {
            rain_until = 0;
        }
        raining = rain_until > 0;

        if (has({"InterruptCombust"})) burn_until = now + 400;
        if (has({"InterruptFrozen", "FrozenInIce", "FeelCold"})) freeze_until = now + 400;
        if (has({"InterruptShock", "InterruptParalyze", "InterruptSleepParalysis"})) shock_until = now + 300;
        if (has({"InterruptEaten"})) squeeze_until = now + 300;

        const bool swim = has({"Swim"}), under = has({"Dive", "QuickDive", "WaterDive", "AirDive", "GuideDive"});
        const bool climb = has({"Climb", "ClimbLadder"}), cliff = has({"CliffHold"});
        const bool push = has({"GrabPushPull"}), lift = has({"Lift"}), crawl = has({"Crawl"});
        const bool slide = has({"Slide", "SlippingDown"});
        const bool music = has({"PlayMusic"}) || anim.find("Ocarina") != std::string::npos;
        const bool talk = has({"Talk", "Listen"}), sleep = has({"Sleep"}), fishing = has({"Fishing"});
        const bool hook = has({"HookshotShoot", "UseHookshot"}), falling = has({"Fall"}) && s.vy < -4.0f;
        const bool burn = now < burn_until, frozen = now < freeze_until, shock = now < shock_until,
                   squeeze = now < squeeze_until;

        // breath: without the Zora tunic a dive gets harder the longer it lasts
        const bool zora = armor.find("Zora") != std::string::npos;
        if (under) {
            if (dive_start < 0) dive_start = now;
        } else {
            dive_start = -1;
        }
        const float dive_sec = dive_start >= 0 ? (now - dive_start) / 1000.0f : 0.0f;
        const float pressure = zora ? 0.4f : (std::min)(1.0f, dive_sec / 20.0f);
        air_left = under && !zora ? (std::max)(0.0f, 1 - dive_sec / 20.0f) : -1;

        // ocarina: each note button's colour flash, and its place in the triggers, humming its pitch
        if (music && hx) {
            struct Note {
                int btn;
                float hz;
                Rgb col;
            };
            static constexpr std::array<Note, 5> Notes{{{4, 150, {40, 90, 255}}, {16, 178, {255, 210, 0}},
                                                        {32, 224, {255, 190, 20}}, {1, 252, {255, 170, 40}}, {8, 300, {255, 230, 60}}}};
            for (const auto& n : Notes) {
                if ((buttons & n.btn) == 0) continue;
                if ((prev_buttons & n.btn) == 0) {
                    note_color = n.col;
                    note_flash_until = now + 350;
                }
                const int f = static_cast<int>((std::min)(255.0f, n.hz));
                const auto [al, ar] = n.btn == 16 ? std::pair{5, 0} : n.btn == 32 ? std::pair{0, 5} : n.btn == 1 ? std::pair{3, 3}
                                      : n.btn == 8 ? std::pair{5, 5} : std::pair{0, 0};
                q.Pulse(now, static_cast<float>(al), static_cast<float>(ar), f, 40, 0, false);
            }
        }
        // movement from Link's position (animation-driven moves show only there)
        const float dt = std::clamp((now - last_t) / 1000.0f, 0.001f, 0.1f);
        const float dx = s.x - last_x, dy = s.y - last_y, dz = s.z - last_z;
        float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (moved > 2) moved = 0; // a warp, not a step
        pos_vel = pos_vel * 0.6f + moved / dt * 0.4f;
        pos_vy = pos_vy * 0.7f + (std::abs(dy) < 2 ? dy / dt : 0.0f) * 0.3f;

        // hookshot flight: the chain pulls Link fast toward the target
        if (hook) hook_t = now;
        const bool fly_now = now - hook_t < 2500 && pos_vel > 6.0f;
        if (fly_now && !flying_state && hx) { // the hook bites, then the pull runs from the fingertips back
            q.Pulse(now, 0, 8, 120, 60);
            q.Pulse(now, 5, 5, 40, 200, 80, true, true);
        }
        if (flying_state && fly_now && now >= next_ratchet && hx) { // chain links ratcheting through the hand
            q.Pulse(now, 4, 5, 160, 50, 0, false, true);
            next_ratchet = now + 110;
        }
        if (fly_now) {
            flying_state = true;
        } else if (flying_state && pos_vel < 3.0f) {
            flying_state = false;
            if (hx) q.Pulse(now, 6, 6, 45, 110);
        }
        // a moving platform: Link stands still (no stick, no action) but the world carries him
        bool idle_verb = true;
        for (const int v : verbs) {
            if (v > 0 && static_cast<size_t>(v) < names.size()) {
                const std::string& n = names[static_cast<size_t>(v)];
                if (n != "Wait" && n != "Drawn" && n != "Sheathed" && n != "None") idle_verb = false;
            }
        }
        const bool carried_now = stick_idle && idle_verb && pos_vel > 0.35f && pos_vel < 8.0f &&
                                 !now_set.contains("Swim") && !now_set.contains("Climb") && !now_set.contains("ClimbLadder");
        carry_t = carried_now ? (carry_t < 0 ? now : carry_t) : -1;
        carried = carry_t >= 0 && now - carry_t > 250;
        flying = flying_state;

        // ice: the ground slides away; quicksand: a slow pull down
        const bool icy = ContainsI(map, "IceCavern") || ContainsI(map, "Ice_");
        slippery = icy && speed > 1 && !swim && !climb;
        const bool sandy = ContainsI(map, "Wasteland") || ContainsI(map, "Hounted");
        sinking = sandy && pos_vy < -0.05f && pos_vy > -1.5f && !has({"Fall", "Jump"});

        // climbing: one grip per hand movement, hand over hand
        if (climb) {
            const bool moving = pos_vel > 0.12f;
            climb_travel += moved;
            const bool wrapped = anim_frame < last_frame - 2;
            bool grip = (moving && !climb_moving) || climb_travel > 0.22f || (wrapped && moving);
            if (entered({"Climb", "ClimbLadder"})) {
                grip = true;
                grip_side = 1;
            }
            if (grip && now - last_grip > 130) {
                if (hx) q.Pulse(now, grip_side < 0 ? 4 : 0, grip_side > 0 ? 4 : 0, 110, 45, 0, true, true);
                grip_pulse_until = now + 150;
                grip_side = -grip_side;
                climb_travel = 0;
                last_grip = now;
            }
            climb_moving = moving;
            last_frame = anim_frame;
        } else {
            climb_travel = 0;
            climb_moving = false;
        }
        last_x = s.x;
        last_y = s.y;
        last_z = s.z;
        last_t = now;

        // ---- triggers ----
        Out o;
        auto& r = o.r;
        auto& l = o.l;
        const auto both = [&](const Effect& e) { r = e; l = e; };
        if (swim && !under) both(speed > 3.5f ? Vibration(0, 3, 25) : Feedback(0, 2));
        if (under) both(Feedback(0, std::clamp(3 + static_cast<int>(5 * pressure), 3, 8)));
        if (climb) {
            both(Feedback(0, 5));
            if (now < grip_pulse_until) {
                if (grip_side < 0) r = Feedback(0, 8);
                else l = Feedback(0, 8);
            }
        }
        if (cliff) both(Feedback(0, 7));
        if (push) both(Feedback(0, 8));
        if (lift) both(Feedback(0, 6));
        if (crawl) both(Feedback(0, 3));
        if (slide) both(Vibration(0, 4, 40));
        if (falling) both(Vibration(0, std::clamp(1 + static_cast<int>((-s.vy - 4) / 3), 1, 6), 50));
        if (hook) r = Feedback(1, 3);
        if (flying) both(Machine(0, 9, 7, 3, 40, 1));
        if (slippery) both(Feedback(0, 1));
        if (sinking) {
            const int p = 3 + static_cast<int>(4 * (std::min)(1.0f, (now % 4000) / 4000.0f));
            both(Feedback(0, p));
        }
        if (fishing) r = Feedback(2, 4);
        if (burn) both(Vibration(0, 5, 90));
        if (frozen) both(Feedback(0, 8));
        if (shock) both(Vibration(0, 8, 120));
        if (squeeze) {
            const int p = 3 + static_cast<int>(5 * (0.5 + 0.5 * std::sin(now / 1000.0 * 2 * Pi * 1.2)));
            r = Feedback(0, p);
            l = Feedback(0, 11 - p);
        }
        busy = music || talk || sleep || in_event;
        hands_busy = climb || cliff || crawl || push || lift || (swim && !under) || hook || flying;
        bool free = busy;
        if (grabbed || terror) {
            both(Feedback(0, 8));
            free = false;
        }

        // ---- light ----
        std::optional<Rgb> light;
        float mix = 0;
        const double ts = now / 1000.0;
        if (swim && !under) {
            const float c = 0.5f + 0.25f * static_cast<float>(std::sin(ts * 5.1)) * static_cast<float>(std::sin(ts * 3.3 + 1)) +
                            0.25f * static_cast<float>(std::sin(ts * 7.7 + 2));
            light = Rgb{20 + 60 * c, 120 + 110 * c, 220 + 35 * c};
            mix = 0.75f;
        }
        if (under) {
            const float sparkle = uniform(rng) < 0.04 ? 1.0f : 0.0f;
            light = Rgb{5 + 100 * sparkle, 35 + 120 * sparkle, 150 + 100 * sparkle};
            mix = 0.9f;
        }
        if (burn) {
            const float f = 0.6f + 0.4f * static_cast<float>(uniform(rng));
            light = Rgb{255 * f, 90 * f, 0};
            mix = 0.95f;
        }
        if (frozen) {
            light = Rgb{170, 230, 255};
            mix = 0.9f;
        }
        if (shock) {
            const bool on = static_cast<int>(ts * 15) % 2 == 0;
            light = on ? Rgb{255, 255, 200} : Rgb{60, 60, 20};
            mix = 0.95f;
        }
        if (squeeze) {
            const float p = 0.5f + 0.5f * static_cast<float>(std::sin(ts * 2 * Pi * 1.2));
            light = Rgb{120 * p + 40, 30, 140 * p + 30};
            mix = 0.85f;
        }
        if (now < nayru_until) {
            const float p = 0.7f + 0.3f * static_cast<float>(std::sin(ts * 9));
            light = Rgb{80 * p, 220 * p, 255 * p};
            mix = 0.6f;
        }
        if (has({"LockOnWait", "LockOnMove"})) {
            light = Rgb{255, 230, 120};
            mix = 0.25f;
        }
        if (has({"ShowItem", "HoldItem"})) {
            const float sp = uniform(rng) < 0.15 ? 1.0f : 0.7f;
            light = Rgb{255 * sp, 210 * sp, 60 * sp};
            mix = 0.8f;
        }
        if (under && !zora && dive_sec > 10) {
            const float p = static_cast<float>(0.5 + 0.5 * std::sin(ts * 2 * Pi * (0.8 + dive_sec / 15)));
            light = Rgb{40 + 120 * p * pressure, 30, 150};
            mix = 0.9f;
        }
        if (music) {
            light = now < note_flash_until ? note_color : Rgb{180, 140, 40};
            mix = now < note_flash_until ? 1.0f : 0.5f;
        }
        if (talk || in_event) {
            light = Rgb{110, 90, 70};
            mix = 0.6f;
        }
        if (sleep) {
            const float b = 0.15f + 0.15f * static_cast<float>(std::sin(ts * 2 * Pi * 0.2));
            light = Rgb{60 * b * 3, 50 * b * 3, 120 * b * 3};
            mix = 0.9f;
        }
        if (now < fade_abyss) {
            light = Rgb{0, 0, 0};
            mix = 1 - (fade_abyss - now) / 1600.0f + 0.3f;
        }
        if (grabbed) {
            const float p = 0.5f + 0.5f * static_cast<float>(std::sin(ts * 2 * Pi * 1.5));
            light = Rgb{110 * p + 40, 0, 50 * p + 20};
            mix = 0.85f;
        }
        if (terror) {
            light = Rgb{90, 90, 100};
            mix = 0.95f;
        }
        o.light = light;
        o.light_mix = std::clamp(mix, 0.0f, 1.0f);
        o.free_triggers = free;
        return o;
    }

private:
    std::unordered_set<std::string> now_set, before;
    std::unordered_map<std::string, long long> seen;
    std::mt19937 rng{3};
    std::uniform_real_distribution<double> uniform{0.0, 1.0};
    long long burn_until = 0, freeze_until = 0, shock_until = 0, squeeze_until = 0, nayru_until = 0;
    long long fade_abyss = 0, rain_until = 0, grip_pulse_until = 0, dive_start = -1, last_grip = 0, last_t = 0;
    long long hook_t = -100000;
    int grip_side = 1;
    float climb_travel = 0, last_x = 0, last_y = 0, last_z = 0, pos_vel = 0, pos_vy = 0, last_frame = 0;
    bool climb_moving = false, flying_state = false, was_grabbed = false, was_terror = false;
    int wall_side = 1, chain = 0;
    long long last_chain = 0, next_ratchet = 0, note_flash_until = 0, carry_t = -1;
    Rgb note_color{};
    std::bernoulli_distribution coin{0.5};
};

// ---------- Combat.cs: enemies near Link and the damage they take ----------
// Actor manager [main+0x4E90D90]: red-black tree at +0x38 (left +0, right +8), entry = node - 8: +0x28 actor,
// +0x38 next entry with the same key. Actor: name char* +0x20, state u32 +0x2C (4-5 running), position +0x31C,
// life [[[actor+0x270]+0x2B8]+0x1C10]+8 (max +0x1C18). The walk runs every 350 ms; life every frame.
class Combat {
public:
    enum class Kind { None, Treasure, Heat, Water, Danger, Wind, Lock, Fish, Guard };
    struct Foe {
        std::string name;
        int life{}, max{};
        float pan{}, front{}, distance{};
        bool boss{};
    };
    struct Act {
        std::string name, verb;
        float pan{}, front{}, distance{};
        bool boss{};
    };
    struct Hit {
        std::string name;
        int damage{}, max{};
        bool killed{};
        float pan{}, front{}, distance{};
        bool boss{};
    };
    struct Seen {
        Kind kind{};
        std::string name;
        float pan{}, front{}, distance{};
        float closing{}; // m/s it comes nearer
    };

    std::vector<Foe> live;
    std::vector<Act> acts;
    std::vector<Seen> objects;
    int nearby = 0;
    float nearest_distance = 999, nearest_side = 0, nearest_front = 1;
    bool boss_near = false;
    std::vector<Seen> gone; // objects that vanished near Link since the last poll

    static Kind KindOf(const std::string& n) {
        const auto has = [&](std::initializer_list<std::string_view> k) {
            for (const auto x : k) {
                if (ContainsI(n, x)) return true;
            }
            return false;
        };
        if (has({"Gerudo"}) && (n.starts_with("Npc_") || n.starts_with("Enemy_")) && !has({"MiddleBoss", "Shop"})) return Kind::Guard;
        if (has({"Fish"}) && !has({"Fishing", "Rod", "Lure", "Bottle", "BGM_"})) return Kind::Fish;
        if (n.starts_with("BGM_") || n.starts_with("Npc_") || n.starts_with("EftLoc_CameraEnv") || n.starts_with("Shop_")) return Kind::None;
        if (has({"ChainLock", "KeyLock", "LockedDoor", "Door_Lock"})) return Kind::Lock;
        if (has({"Heart_Piece", "HeartPiece", "HeartContainer", "Obj_Rupee", "Rupee_Gold", "Rupee_Silver"})) return Kind::Treasure;
        if (has({"RollingRock", "Boulder", "Spike", "Blade", "Guillotine", "IronBall", "Trap"})) return Kind::Danger;
        if (has({"Fire", "Torch", "Flame", "Lava", "Brazier", "Bonfire", "Magma", "Burner", "Candle"})) return Kind::Heat;
        if (has({"Waterfall", "Geyser", "WaterSpout"})) return Kind::Water;
        if (has({"Fan", "Windmill", "Tornado", "Whirlwind"})) return Kind::Wind;
        return Kind::None;
    }

    // The enemies and objects near Link (every 350 ms)
    void Discover(const Mem& m, u64 link, long long now) {
        if (now < next_walk) return;
        next_walk = now + 350;
        const auto pos = [&](u64 a, float& x, float& y, float& z) {
            float p[3]{};
            m.Read(a + 0x31C, p, 12);
            x = p[0]; y = p[1]; z = p[2];
        };
        float lx = 0, ly = 0, lz = 0;
        if (link) pos(link, lx, ly, lz);
        const auto P = [&](u64 va) { return va == 0 || (va & 7) != 0 ? 0ull : m.P(va); };
        const auto in_main = [&](u64 p) { return p >= m.main && p < m.main + 0x5000000; };
        std::vector<Enemy> list;
        std::vector<Obj> objs;
        const u64 mgr = P(m.main + 0x4E90D90), root = mgr ? P(mgr + 0x38) : 0;
        std::vector<u64> stack;
        u64 node = root;
        int visits = 0;
        while ((node != 0 || !stack.empty()) && visits < 6000) {
            while (node != 0 && stack.size() < 64) {
                stack.push_back(node);
                node = P(node);
            }
            if (node != 0) break; // deeper than a balanced tree: changed under us
            node = stack.back();
            stack.pop_back();
            u8 entry[0x40];
            for (u64 e = node - 8; e != 0 && visits < 6000;) {
                visits++;
                if ((e & 7) != 0 || !m.Read(e, entry, 0x40)) break;
                u64 next, actor;
                std::memcpy(&next, entry + 0x38, 8);
                std::memcpy(&actor, entry + 0x28, 8);
                u8 head[0x10];
                if (actor == 0 || actor == link || !m.Read(actor + 0x20, head, 0x10)) {
                    e = next;
                    continue;
                }
                u32 state;
                u64 name_ptr;
                std::memcpy(&state, head + 0xC, 4);
                std::memcpy(&name_ptr, head, 8);
                if (state < 4 || state > 5 || name_ptr == 0) {
                    e = next;
                    continue;
                }
                const std::string s = m.Str(name_ptr, 48);
                float x, y, z;
                pos(actor, x, y, z);
                const float dx = x - lx, dy = y - ly, dz = z - lz, d2 = dx * dx + dy * dy + dz * dz;
                if (!s.starts_with("Enemy_")) {
                    const Kind k = KindOf(s);
                    if (k != Kind::None && (link == 0 || d2 < 20.0f * 20.0f) && objs.size() < 48) objs.push_back({actor, s, k, -1});
                    e = next;
                    continue;
                }
                if (s.find("Skulltula_Gold") != std::string::npos && (link == 0 || d2 < 400) && objs.size() < 48) {
                    objs.push_back({actor, s, Kind::Treasure, -1});
                } else if ((KindOf(s) == Kind::Guard || KindOf(s) == Kind::Fish) && (link == 0 || d2 < 400) && objs.size() < 48) {
                    objs.push_back({actor, s, KindOf(s), -1});
                }
                if (link != 0 && d2 > 25.0f * 25.0f) {
                    e = next;
                    continue;
                }
                const u64 comp = P(P(actor + 0x270) + 0x2B8);
                const u64 cur = comp ? P(comp + 0x1C10) : 0, max = comp ? P(comp + 0x1C18) : 0;
                if (cur && max && in_main(P(cur)) && in_main(P(max))) list.push_back({actor, cur, max, s});
                e = next;
            }
            node = P(node + 8);
        }
        found = std::move(list);
        obj_found = std::move(objs);
    }

    // The life of the enemies found so far; the hits since the last call
    std::vector<Hit> Poll(const Mem& m, u64 link, const std::vector<std::string>& names) {
        std::vector<Hit> hits;
        acts.clear();
        live.clear();
        objects.clear();
        float lx = 0, lz = 0;
        ly_ = 0;
        if (link) {
            float p[3]{};
            m.Read(link + 0x31C, p, 12);
            lx = p[0];
            ly_ = p[1];
            lz = p[2];
        }
        // camera right and forward on the ground plane (gameplay camera eye +0x38, look-at +0x44)
        float rx = 1, rz = 0, ux = 0, uz = 1;
        const u64 cam = m.P(m.P(m.main + 0x4E98068) + 0x68);
        float b[6]{};
        if (cam && m.Read(cam + 0x38, b, 24)) {
            float fx = b[3] - b[0], fz = b[5] - b[2];
            const float len = std::sqrt(fx * fx + fz * fz);
            if (len > 1e-3f) {
                fx /= len;
                fz /= len;
                rx = -fz;
                rz = fx;
                ux = fx;
                uz = fz;
            }
        }
        int near_count = 0;
        float nearest = 999;
        bool boss = false;
        std::unordered_set<u64> seen;
        for (const auto& f : found) {
            seen.insert(f.actor);
            Enemy& e = tracked.try_emplace(f.actor, f).first->second;
            s32 life = 0, max = 0;
            if (!m.Read(e.life_ptr + 8, &life, 4) || !m.Read(e.max_ptr + 8, &max, 4) || max <= 0) continue;
            float p[3]{};
            m.Read(e.actor + 0x31C, p, 12);
            const float dx = p[0] - lx, dz = p[2] - lz, dist = std::sqrt(dx * dx + dz * dz);
            const float pan = dist > 0.3f ? std::clamp((dx * rx + dz * rz) / dist, -1.0f, 1.0f) : 0.0f;
            const float front = dist > 0.3f ? std::clamp((dx * ux + dz * uz) / dist, -1.0f, 1.0f) : 1.0f;
            near_count++;
            if (dist < nearest) {
                nearest = dist;
                nearest_side = pan;
                nearest_front = front;
            }
            const bool is_boss = e.name.find("Boss") != std::string::npos;
            boss |= is_boss;
            if (e.life >= 0 && life < e.life) hits.push_back({e.name, e.life - life, max, life <= 0, pan, front, dist, is_boss});
            if (dist < 14) {
                const int v = VerbOf(m, e.actor);
                if (v != e.verb && e.verb != -1 && v >= 0 && static_cast<size_t>(v) < names.size()) {
                    acts.push_back({e.name, names[static_cast<size_t>(v)], pan, front, dist, is_boss});
                }
                e.verb = v;
            }
            e.life = life;
            e.max = max;
            if (life > 0) live.push_back({e.name, life, max, pan, front, dist, is_boss});
        }
        std::erase_if(tracked, [&](const auto& kv) { return !seen.contains(kv.first); });
        nearby = near_count;
        nearest_distance = nearest;
        boss_near = boss;
        gone.clear();
        const auto t_now = Clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(t_now - last_poll).count(), 0.005f, 0.2f);
        last_poll = t_now;
        std::unordered_set<u64> obj_seen;
        for (const auto& o : obj_found) {
            obj_seen.insert(o.actor);
            Obj& t = obj_tracked.try_emplace(o.actor, o).first->second;
            float p[3]{};
            m.Read(t.actor + 0x31C, p, 12);
            const float dx = p[0] - lx, dy = p[1] - ly_, dz = p[2] - lz, h = std::sqrt(dx * dx + dz * dz);
            const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float pan = h > 0.3f ? std::clamp((dx * rx + dz * rz) / h, -1.0f, 1.0f) : 0.0f;
            const float front = h > 0.3f ? std::clamp((dx * ux + dz * uz) / h, -1.0f, 1.0f) : 1.0f;
            const float closing = t.last_dist >= 0 ? (t.last_dist - dist) / dt : 0.0f;
            t.last_dist = dist;
            objects.push_back({t.kind, t.name, pan, front, dist, std::clamp(closing, -30.0f, 30.0f)});
        }
        for (auto it = obj_tracked.begin(); it != obj_tracked.end();) {
            if (!obj_seen.contains(it->first)) {
                if (it->second.last_dist >= 0 && it->second.last_dist < 5) gone.push_back({it->second.kind, it->second.name, 0, 1, it->second.last_dist, 0});
                it = obj_tracked.erase(it);
            } else {
                ++it;
            }
        }
        return hits;
    }

private:
    struct Enemy {
        u64 actor{}, life_ptr{}, max_ptr{};
        std::string name;
        int life = -1, max = 0, verb = -1;
    };
    struct Obj {
        u64 actor{};
        std::string name;
        Kind kind{};
        float last_dist = -1;
    };
    std::unordered_map<u64, Obj> obj_tracked;
    Clock::time_point last_poll{};
    float ly_ = 0;
    static int VerbOf(const Mem& m, u64 actor) {
        const u64 bsa = m.P(m.P(actor + 0x270) + 0x188);
        const u64 slot = bsa ? m.P(bsa + 0x1C0) : 0;
        s32 v = -1;
        if (!slot || !m.Read(slot + 0x28, &v, 4)) return -1;
        if (v == 0x11A && !m.Read(slot + 0x4C, &v, 4)) return -1;
        return v;
    }
    std::vector<Enemy> found;
    std::vector<Obj> obj_found;
    std::unordered_map<u64, Enemy> tracked;
    long long next_walk = 0;
};

// ---------- Scene.cs: the place's lightbar mood ----------
class Scene {
public:
    enum class Theme { None, Fire, Water, Organic, Forest, Shadow, Desert, Ice, Ganon, Holy, Dream };
    void SetMap(const std::string& map) {
        if (map == current_map) return;
        current_map = map;
        theme = Classify(map);
    }
    std::optional<Rgb> Light(long long now) {
        const double t = now / 1000.0;
        switch (theme) {
        case Theme::Fire:
            flicker = std::clamp(flicker + static_cast<float>(uniform(rng) - 0.5) * 0.25f, 0.55f, 1.0f);
            return Rgb{255 * flicker, 85 * flicker, 8 * flicker};
        case Theme::Water: {
            const float w = 0.65f + 0.35f * static_cast<float>(std::sin(2 * Pi * 0.4 * t));
            return Rgb{10 * w, (80 + 60 * w) * w, 255 * w};
        }
        case Theme::Organic: {
            const float p = 0.45f + 0.55f * static_cast<float>(std::pow((std::max)(0.0, std::sin(2 * Pi * t / 1.6)), 4));
            return Rgb{210 * p, 40 * p, 70 * p};
        }
        case Theme::Forest: {
            if (now > sparkle_until && uniform(rng) < 0.003) sparkle_until = now + 90; // a firefly
            if (now < sparkle_until) return Rgb{200, 255, 80};
            const float b = 0.7f + 0.3f * static_cast<float>(std::sin(2 * Pi * 0.25 * t));
            return Rgb{20 * b, 200 * b, 60 * b};
        }
        case Theme::Shadow: {
            const float b = 0.35f + 0.15f * static_cast<float>(std::sin(2 * Pi * 0.17 * t)) + 0.05f * static_cast<float>(uniform(rng));
            return Rgb{90 * b, 0, 150 * b};
        }
        case Theme::Desert: {
            const float b = 0.8f + 0.2f * static_cast<float>(std::sin(2 * Pi * 1.3 * t)) * static_cast<float>(std::sin(2 * Pi * 0.21 * t));
            return Rgb{235 * b, 160 * b, 40 * b};
        }
        case Theme::Ice:
            if (now > sparkle_until && uniform(rng) < 0.006) sparkle_until = now + 60;
            return now < sparkle_until ? Rgb{255, 255, 255} : Rgb{110, 210, 255};
        case Theme::Ganon: {
            const float p = 0.4f + 0.6f * static_cast<float>(std::pow(0.5 + 0.5 * std::sin(2 * Pi * 0.3 * t), 3));
            return Rgb{160 * p, 0, 12 * p};
        }
        case Theme::Holy:
            return Rgb{255, 225, 160};
        case Theme::Dream: {
            const double h = std::fmod(t * 0.05, 1.0);
            return Rgb{150 + 80 * static_cast<float>(std::sin(2 * Pi * h)), 50, 255};
        }
        default:
            return std::nullopt;
        }
    }

private:
    static Theme Classify(const std::string& m) {
        const auto has = [&](std::initializer_list<std::string_view> k) {
            for (const auto x : k) {
                if (ContainsI(m, x)) return true;
            }
            return false;
        };
        if (has({"Dream"})) return Theme::Dream;
        if (has({"Ganon", "FinalBattle"})) return Theme::Ganon;
        if (has({"FireTemple", "DeathMountainCrater", "Crater", "DodongoCavern", "Volvagia"})) return Theme::Fire;
        if (has({"JabuJabu"})) return Theme::Organic;
        if (has({"IceCavern", "Ice"})) return Theme::Ice;
        if (has({"WaterTemple", "Zora", "LakeHylia", "Lake"})) return Theme::Water;
        if (has({"ShadowTemple", "BottomOfTheWell", "Well", "Graveyard"})) return Theme::Shadow;
        if (has({"SpiritTemple", "Colossus", "Wasteland", "Gerudo", "Desert"})) return Theme::Desert;
        if (has({"ForestTemple", "LostWood", "Kokiri", "SacredForest", "DekuTree", "Forest"})) return Theme::Forest;
        if (has({"TempleOfTime", "ShrineOfTime", "DoorOfTime"})) return Theme::Holy;
        return Theme::None;
    }
    std::string current_map;
    Theme theme = Theme::None;
    std::mt19937 rng{7};
    std::uniform_real_distribution<double> uniform{0.0, 1.0};
    long long sparkle_until = 0;
    float flicker = 1;
};

// ---------- Events.cs: scripted moments by the running event's name ----------
class Events {
public:
    void Update(long long now, const std::string& name, bool in_event, bool paused, Actions& act, Quad& q) {
        if (!initialized) {
            initialized = true;
            last = name;
            was_event = in_event;
            return;
        }
        const bool started = !name.empty() && (name != last || (in_event && !was_event));
        was_event = in_event;
        last = name;
        if (!started) return;
        if (const auto it = fired.find(name); it != fired.end() && now - it->second < 4000) return;
        fired[name] = now;
        if (paused) return; // (Pulse of Time passes no haptics while paused, and so plays nothing)
        const auto has = [&](std::initializer_list<std::string_view> k) {
            for (const auto x : k) {
                if (ContainsI(name, x)) return true;
            }
            return false;
        };
        const auto glow = [&](float r, float g, float b, int ms) {
            light = {r, g, b};
            light_from = now;
            light_until = now + ms;
            sweep_day = false;
        };
        if (has({"Ocarina", "Melody", "Warp"})) {
            if (has({"SunSong", "Sun"})) { sweep_day = true; light_from = now; light_until = now + 3500; q.Pulse(now, 3, 3, 25, 2500, 0, false, true); return; }
            if (has({"Storm"})) { act.StartRain(now, 90000); glow(200, 210, 255, 400); q.Pulse(now, 7, 7, 30, 500, 0, true, true); return; }
            if (has({"Epona"})) { glow(255, 170, 90, 4500); return; }
            if (has({"Saria"})) { glow(60, 255, 90, 2500); return; }
            if (has({"Zelda", "Lullaby"})) { glow(255, 215, 90, 3000); return; }
            if (has({"SongOfTime", "TimeSong", "Time"})) { glow(170, 210, 255, 3000); q.Pulse(now, 5, 5, 25, 1200, 0, true, true); return; }
            if (has({"Minuet", "Forest"})) { glow(40, 255, 70, 3000); return; }
            if (has({"Bolero", "Fire"})) { glow(255, 70, 0, 3000); q.Pulse(now, 6, 6, 40, 500); return; }
            if (has({"Serenade", "Water"})) { glow(40, 120, 255, 3000); return; }
            if (has({"Nocturne", "Shadow"})) { glow(110, 20, 170, 3000); return; }
            if (has({"Requiem", "Spirit"})) { glow(255, 150, 40, 3000); return; }
            if (has({"Prelude", "Light"})) { glow(255, 250, 200, 3000); return; }
            if (has({"Scarecrow"})) return;
            if (has({"Warp"})) { glow(120, 200, 255, 2500); return; }
            return;
        }
        if (has({"Boss"}) && has({"Appear", "Demo", "Start", "Encount", "Opening"})) {
            for (int i = 0; i < 3; i++) q.Pulse(now, 6, 6, 30, 90, i * 650, false, true); // three slow heavy heartbeats
            glow(200, 0, 20, 3000);
            return;
        }
        if (has({"SageOf", "Sage_"})) { glow(255, 255, 255, 3000); return; }
        if (has({"GameOver"})) { glow(60, 0, 0, 3000); return; }
        if (has({"FallDown"})) { glow(0, 0, 0, 1500); return; }
        if (has({"BancChange"})) { glow(0, 0, 0, 700); return; }
        if (has({"Defeat_"})) { glow(255, 220, 120, 3000); return; }
    }
    std::optional<Rgb> LightNow(long long now) const {
        if (now >= light_until) return std::nullopt;
        if (sweep_day) {
            const float p = (now - light_from) / static_cast<float>((std::max)(1ll, light_until - light_from));
            return p < 0.33f ? Rgb{30, 40, 140} : p < 0.66f ? Rgb{255, 120, 60} : Rgb{255, 245, 210};
        }
        return light;
    }

private:
    bool initialized = false, was_event = false, sweep_day = false;
    std::string last;
    std::unordered_map<std::string, long long> fired;
    Rgb light{};
    long long light_from = 0, light_until = 0;
};

// ---------- Moments.cs: story, minigames and places (the parts that reach triggers, light, LEDs, mute LED) ----------
class Moments {
public:
    std::optional<Effect> r, l; // trigger overrides this frame
    float bar = -1;             // player-LED bar (0..1), -1 = none
    u8 mic = 0;
    bool child{}, dream{}, escaping{}, racing{}, on_ship{};

    void Update(long long now, GameData& gd, Actions& act, const State& s, const std::string& map,
                const Combat* cb, const std::string& weapon, bool paused, Quad& q, float pull) {
        quad = &q;
        now_ms = now;
        r.reset();
        l.reset();
        bar = -1;
        mic = now < mic_until ? 2 : 0;
        for (size_t i = later.size(); i-- > 0;) {
            if (now >= later[i].first) {
                auto run = later[i].second;
                later.erase(later.begin() + static_cast<std::ptrdiff_t>(i));
                run();
            }
        }
        const auto in_map = [&](std::initializer_list<std::string_view> k) {
            for (const auto x : k) {
                if (ContainsI(map, x)) return true;
            }
            return false;
        };
        if (now >= next_poll && !paused) {
            next_poll = now + 100;
            Poll(now, gd, map);
        }
        // the minigame timer: soft seconds, the last ten harder, the last three a heartbeat (and the bar on the LEDs)
        if (timer_on && timer_left >= 0 && !paused) {
            const int sec = static_cast<int>(std::ceil(timer_left));
            if (sec != last_second && last_second >= 0 && sec >= 0) {
                if (sec <= 3) q.Pulse(now, 6, 6, 30, 70, 0, false, true);
                else if (sec <= 10) q.Pulse(now, 2, 2, 200, 25);
            }
            last_second = sec;
            if (timer_frac >= 0) bar = timer_frac;
        } else {
            last_second = -1;
        }
        if (!timer_on && now < score_game_until && timer_frac >= 0) bar = timer_frac;

        // the collapsing castle: the mute LED stays on
        escaping = now < escape_until && in_map({"Ganon"});
        if (escaping && !paused) {
            if (now >= next_debris) { // rubble falls all around
                const auto pl = Quad::Place(static_cast<float>(uniform(rng) * 2 - 1), static_cast<float>(uniform(rng) * 2 - 1), 0.8f);
                q.Pulse(now, pl.l2, pl.r2, 35, 90);
                next_debris = now + 500 + static_cast<long long>(uniform(rng) * 1500);
            }
            mic = (std::max)(mic, static_cast<u8>(1));
        }

        // a ship rocks: the weight shifts from one trigger to the other (not while fighting)
        if (on_ship && !paused && (cb == nullptr || cb->nearest_distance > 8)) {
            const float a = static_cast<float>(std::sin(2 * Pi * 0.22 * now / 1000.0));
            l = Feedback(0, 2 + static_cast<int>(std::round(3 * (0.5f - 0.5f * a))));
            r = Feedback(0, 2 + static_cast<int>(std::round(3 * (0.5f + 0.5f * a))));
        }

        // fishing: a fish on the line pulls from where it swims, R2 is the line's brake (hooked = a fish
        // coming closer while you reel)
        const bool fishing = act.HasVerb("Fishing") || act.HasVerb("ChangeLure");
        bool hooked = false;
        if (fishing && cb) {
            const Combat::Seen* fish = nullptr;
            for (const auto& o : cb->objects) {
                if (o.kind == Combat::Kind::Fish && o.distance < 25 && (fish == nullptr || o.distance < fish->distance)) fish = &o;
            }
            const bool reeling = fish && fish->closing > 0.25f && pull > 0.15f;
            if (reeling) {
                if (hook_t < 0) hook_t = now;
                last_pull = now;
            } else if (hook_t >= 0 && now - last_pull > 1500) {
                hook_t = -1;
            }
            hooked = hook_t >= 0 && now - hook_t > 250;
            if (hooked && fish) {
                if (now - hook_t < 350 && next_tug == 0) { // the bite
                    q.Pulse(now, 0, 8, 60, 90);
                    Glow({120, 230, 255}, 200, 0.9f);
                }
                if (now >= next_tug) { // it pulls this way and that
                    const float drag = std::clamp(fish->pan + static_cast<float>(uniform(rng) - 0.5), -1.0f, 1.0f);
                    q.Pulse(now, drag < 0 ? 4.0f : 1.0f, drag > 0 ? 4.0f : 1.0f, 45, 140, 0, true, true);
                    next_tug = now + 250 + static_cast<long long>(uniform(rng) * 450);
                }
                r = Feedback(1, std::clamp(3 + static_cast<int>(5 * (std::max)(0.0f, pull)), 1, 8));
                bar = std::clamp(pull, 0.0f, 1.0f);
            }
        }
        if (!hooked) next_tug = 0;
        if (fishing && flags[H("MiniGame_Fishing_Rainy")]) act.StartRain(now, 1500);

        // the Master Sword knocked away (Ganon): R2 goes slack until it is back
        bool ganon_near = false;
        if (cb) {
            for (const auto& f : cb->live) {
                if (f.name.find("Ganon") != std::string::npos && f.distance < 40) ganon_near = true;
            }
        }
        if (!last_weapon.empty() && weapon.empty() && ganon_near) {
            sword_gone_since = now;
            Glow({60, 60, 80}, 600, 0.6f);
        }
        if (sword_gone_since >= 0) {
            if (!weapon.empty()) {
                q.Pulse(now, 0, 6, 90, 80);
                sword_gone_since = -1;
            } else {
                r = Off();
                if (now % 900 < 30) q.Pulse(now, 0, 1.5f, 250, 25);
            }
        }
        last_weapon = weapon;
        // Ganon's third eye open: a window to strike, the triggers glow with a faint tremor
        if (ganon_near && flags[H("Enemy_DungeonBossGanon_IsOpenedThirdEye")] && now % 600 < 20) q.Pulse(now, 2, 2, 120, 200, 0, false, true);

        // Gerudo guards: caught = alarm (a scene starts near a guard, then Link is somewhere else)
        const bool in_event = act.in_event;
        if (in_event && !was_event) event_start = now;
        was_event = in_event;
        if (in_map({"Gerudo"}) && cb && !paused) {
            const Combat::Seen* guard = nullptr;
            for (const auto& o : cb->objects) {
                if (o.kind == Combat::Kind::Guard && o.distance < 14 && (guard == nullptr || o.distance < guard->distance)) guard = &o;
            }
            if (guard) { // a heartbeat that quickens as the nearest guard comes closer
                guard_seen = now;
                if (now >= next_guard_beat && !in_event) {
                    const float k = 1 - guard->distance / 14;
                    const auto pl = Quad::Place(guard->pan, guard->front, 0.3f + 0.5f * k);
                    q.Pulse(now, pl.l2 * 0.5f, pl.r2 * 0.5f, 30, 70, 0, false, true);
                    next_guard_beat = now + static_cast<long long>(380 + 1000 * (1 - k));
                }
            }
            const float jump = std::abs(s.x - last_x) + std::abs(s.z - last_z);
            if (jump > 3 && event_start >= 0 && now - event_start < 6000 && event_start - guard_seen < 2500 && now >= caught_until) {
                Glow({255, 0, 0}, 1500, 0.9f, true);
                mic_until = now + 2000;
                caught_until = now + 5000;
            }
            if (now < caught_until - 4300) {
                r = Feedback(0, 8);
                l = Feedback(0, 8);
            }
        }
        last_x = s.x;
        last_z = s.z;
    }

    std::optional<std::pair<Rgb, float>> Light(long long now) {
        if (dream && now >= light_until) return std::make_pair(Rgb{120, 70, 200}, 0.35f);
        if (now < light_from || now >= light_until) return std::nullopt;
        Rgb c = light;
        if (light_flicker && uniform(rng) < 0.35) c = {c[0] * 0.35f, c[1] * 0.35f, c[2] * 0.35f};
        const float fade = std::clamp((light_until - now) / 400.0f, 0.0f, 1.0f);
        return std::make_pair(c, light_mix * fade);
    }

private:
    static u32 H(std::string_view s) {
        return Hash(s);
    }
    void Glow(const Rgb& c, int ms, float mix = 0.85f, bool flicker = false, int delay = 0) {
        light = c;
        light_from = now_ms + delay;
        light_until = light_from + ms;
        light_mix = mix;
        light_flicker = flicker;
    }
    void Later(long long now, int ms, std::function<void()> run) {
        later.emplace_back(now + ms, std::move(run));
    }
    void WaterChange(long long now, bool up) { // the water rises from the palms into the fingers, or drains away
        if (up) {
            quad->Pulse(now, 3, 3, 30, 600, 900, false, true);
            quad->Pulse(now, 6, 6, 30, 700, 1600, true, true);
        } else {
            quad->Pulse(now, 6, 6, 30, 700, 0, true, true);
            quad->Pulse(now, 3, 3, 30, 600, 700, true, true);
        }
        Glow({30, 110, 255}, 2400, 0.7f);
    }
    bool Rise(u32 h, bool v) {
        const auto it = flags.find(h);
        const bool had = it != flags.end(), old = had && it->second;
        flags[h] = v;
        return had && v && !old;
    }

    void Poll(long long now, GameData& gd, const std::string& map) {
        const auto in_map = [&](std::initializer_list<std::string_view> k) {
            for (const auto x : k) {
                if (ContainsI(map, x)) return true;
            }
            return false;
        };
        // time travel in the Temple of Time: white light after a moment of silence
        const bool c = gd.GetBool(H("Sequence_IsChildhood")).value_or(true);
        if (child_known && was_child != c && in_map({"TempleOfTime", "ShrineOfTime", "DoorOfTime"})) {
            Glow({255, 255, 255}, 3500, 0.95f, false, 800);
            Later(now, 800, [this, now] { quad->Pulse(now + 800, 5, 5, 30, 1800, 0, true, true); });
        }
        child_known = true;
        was_child = c;
        child = c;
        dream = in_map({"Dream"});

        // minigames: start shot, timer, score
        const bool flag = gd.GetBool(H("MiniGame_IsTimerCounting")).value_or(false);
        const bool down = gd.GetBool(H("MiniGame_IsTimerCountDown")).value_or(false);
        float val = gd.GetFloat(H("MiniGame_TimerValue")).value_or(0), lim = gd.GetFloat(H("MiniGame_TimerValueLimit")).value_or(0);
        if (lim > 1000) { val /= 1000; lim /= 1000; }
        if (std::isnan(timer_val)) timer_val = val;
        else if (val != timer_val) { timer_val = val; timer_moved = now; }
        const bool on = flag && now - timer_moved < 1500;
        const float left = down ? val : lim > 0 ? lim - val : -1;
        if (on && !timer_on) { quad->Pulse(now, 0, 8, 50, 90); Glow({255, 255, 255}, 250, 0.9f); score_game_until = now + 120000; }
        if (!on && timer_on && timer_left >= 0 && timer_left < 0.6f) { mic_until = now + 1500; Glow({255, 40, 0}, 800, 0.8f); }
        timer_on = on;
        timer_left = on ? left : -1;
        timer_frac = on && lim > 0 && left >= 0 ? std::clamp(left / lim, 0.0f, 1.0f) : -1;
        const int score = gd.GetInt(H("MiniGame_Score")).value_or(-1), max = gd.GetInt(H("MiniGame_ScoreMax")).value_or(0);
        if (last_score >= 0 && score > last_score && score - last_score < 1000) {
            quad->Pulse(now, 4, 5, 150, 50);
            Glow({255, 220, 60}, 180, 0.7f);
            if ((last_score < 750 && score >= 750) || (last_score < 1400 && score >= 1400)) Glow({255, 200, 40}, 1500, 0.9f);
            score_game_until = now + 60000;
        }
        last_score = score;
        if (now < score_game_until && !timer_on && max > 0 && score >= 0) timer_frac = std::clamp(score / static_cast<float>(max), 0.0f, 1.0f);

        // the fishing pond: a new record
        const int fa = gd.GetInt(H("MiniGame_Fishing_RecordFishSize_Adult")).value_or(-1);
        const int fc = gd.GetInt(H("MiniGame_Fishing_RecordFishSize_Child")).value_or(-1);
        if ((fish_a >= 0 && fa > fish_a) || (fish_c >= 0 && fc > fish_c)) Glow({255, 210, 60}, 2000, 0.9f, false, 900);
        fish_a = fa;
        fish_c = fc;

        // water levels
        const int water = gd.GetInt(H("WaterTemple_Room00_WaterLevel")).value_or(-1);
        if (last_water >= 0 && water >= 0 && water != last_water) WaterChange(now, water > last_water);
        last_water = water;
        for (const char* n : {"BottomOfTheWell_Room_01_WaterLevel_Down", "InsideTheDekuTree_RoomG_WaterLevel"}) {
            if (Rise(H(n), gd.GetBool(H(n)).value_or(false))) WaterChange(now, false);
        }
        // ice melts
        for (const char* n : {"Gimmick_RedIce_Melted", "ForestTemple_Room14_MeltedIce", "ForestTemple_Room27_MeltedIce"}) {
            if (Rise(H(n), gd.GetBool(H(n)).value_or(false))) Glow({120, 200, 255}, 1200, 0.6f);
        }
        // the castle collapses
        for (const char* n : {"2F_FallDown", "4F_BreakRoom", "AfterStep03", "AfterStep05", "AfterStep08", "AfterStep10",
                              "StartFireTrap_3F", "OpenLastGate", "SetBackRubble"}) {
            const u32 h = H(std::string("EscapeFromCrumblingGanonCastle_") + n);
            if (Rise(h, gd.GetBool(h).value_or(false))) {
                quad->Pulse(now, 8, 8, 30, 300, 0, true, true);
                Glow({255, 80, 0}, 1500, 0.8f, true);
                escape_until = now + 90000;
            }
        }
        // Ganon
        const u32 eye = H("Enemy_DungeonBossGanon_IsOpenedThirdEye"), zelda = H("Enemy_DungeonBossGanon_IsZeldaMagicChargeFinish");
        if (Rise(eye, gd.GetBool(eye).value_or(false))) Glow({255, 215, 90}, 2500, 0.6f);
        if (Rise(zelda, gd.GetBool(zelda).value_or(false))) {
            quad->Pulse(now, 5, 5, 60, 900, 0, false, true);
            Glow({255, 250, 220}, 2000, 0.9f);
        }
        Rise(H("MiniGame_Fishing_Rainy"), gd.GetBool(H("MiniGame_Fishing_Rainy")).value_or(false));
        // cuccos
        const u32 cucco = H("Cucco_IsAngry");
        if (Rise(cucco, gd.GetBool(cucco).value_or(false))) Glow({255, 255, 255}, 6000, 0.5f, true);

        // Ingo's race and the Shadow Temple's ship (sequence enums)
        constexpr u32 EponaSeq = 0x4969f026, ShadowSeq = 0x7a91dbd5;
        const u32 race = H("GambleRacePlaying"), complete = H("Complete"), ship = H("BattleOnShip");
        const u32 ep = gd.GetEnum(EponaSeq).value_or(0);
        if (epona_known && epona_seq != ep) {
            if (ep == race) { quad->Pulse(now, 0, 8, 50, 90); Glow({255, 160, 60}, 1200, 0.6f); }
            if (epona_seq == race && ep == complete) {
                Later(now, 400, [this, now] { quad->Pulse(now + 400, 8, 8, 35, 250); });
                Glow({255, 200, 40}, 2500, 0.9f, false, 400);
            }
        }
        epona_known = true;
        epona_seq = ep;
        racing = ep == race;
        const u32 sh = gd.GetEnum(ShadowSeq).value_or(0);
        if (shadow_known && shadow_seq == ship && sh == H("BeforeBossRoom")) quad->Pulse(now, 4, 4, 28, 2000, 0, true, true); // the ship sinks
        shadow_known = true;
        shadow_seq = sh;
        on_ship = sh == ship && in_map({"Shadow"});
    }

    long long now_ms = 0, next_poll = 0, mic_until = 0, score_game_until = 0, escape_until = 0, timer_moved = -100000;
    long long light_from = 0, light_until = 0, sword_gone_since = -1, event_start = -1, guard_seen = -10000, caught_until = 0;
    std::vector<std::pair<long long, std::function<void()>>> later;
    std::unordered_map<u32, bool> flags;
    bool child_known = false, was_child = false, timer_on = false, epona_known = false, light_flicker = false, was_event = false;
    float timer_left = -1, timer_frac = -1, timer_val = std::numeric_limits<float>::quiet_NaN(), light_mix = 0;
    float last_x = 0, last_z = 0;
    int last_score = -1, fish_a = -1, fish_c = -1, last_water = -1;
    u32 epona_seq = 0, shadow_seq = 0;
    bool shadow_known = false;
    Quad* quad = nullptr;
    int last_second = -1;
    long long next_debris = 0, hook_t = -1, last_pull = 0, next_tug = 0, next_guard_beat = 0;
    Rgb light{};
    std::string last_weapon;
    std::mt19937 rng{7};
    std::uniform_real_distribution<double> uniform{0.0, 1.0};
};

// ---------- The loop (Daemon.cs: life, aiming, riding, triggers, lights) ----------
const u32 HPause = Hash("Sequence_IsPause"), HPauseReq = Hash("Sequence_GamePauseRequestCount");
const u32 HPrePause = Hash("Sequence_IsPreGamePause"), HChild = Hash("Sequence_IsChildhood");
const u32 HLoading = Hash("Sequence_IsLoadingBanc"), HMap = Hash("Sequence_CurrentBanc");
const u32 HArrows = Hash("PouchData.Item_Arrow_Normal.StockNum"), HSeeds = Hash("PouchData.Item_DekuSeed.StockNum");
const u32 HMaxLife = Hash("PlayerStatus.MaxLife");
const u32 HShield = Hash("PlayerStatus.EquipShield"), HBoot = Hash("PlayerStatus.EquipBoot");
const u32 HArmor = Hash("PlayerStatus.EquipArmor");
const u32 HRupee = Hash("PlayerStatus.CurrentRupee"), HMp = Hash("PlayerStatus.MP"), HMpMax = Hash("PlayerStatus.MPMax");
const u32 HNuts = Hash("PouchData.Item_DekuNut.StockNum"), HBombs = Hash("PouchData.Item_Bomb.StockNum");
const u32 HWalletM = Hash("PouchData.Item_Wallet_Medium.StockNum"), HWalletL = Hash("PouchData.Item_Wallet_Large.StockNum");
const u32 HWeapon = Hash("PlayerStatus.EquipWeapon");
const u32 HChus = Hash("PouchData.Item_Bombchu.StockNum");
constexpr int BSquare = 1, BCross = 2, BCircle = 4, BL1 = 0x10, BR1 = 0x20;
// The stick's direction 0 (up) clockwise to 7, -1 near the middle
int StickDir(int x, int y) {
    const float dx = (x - 128) / 128.0f, dy = (y - 128) / 128.0f;
    if (dx * dx + dy * dy < 0.5f * 0.5f) return -1;
    const double a = std::atan2(dx, -dy) * 180 / Pi;
    return static_cast<int>(std::round(std::fmod(a + 360, 360) / 45)) % 8;
}
// GameNames.cs (PracticeROM / sloperina's gamedata fields): IsGet.<item>, DefeatedEnemyNum.<enemy>
constexpr std::string_view Items[]{"Armor_Adult_Default_Head", "Armor_Adult_Gauntlet_Gold", "Armor_Adult_Gauntlet_Silver", "Item_AgonyStone", "Item_AlarmCucco", "Item_Arrow_Normal", "Weapon_Base", "Weapon_Sword_Biggoron", "Item_BombBag_Large", "Item_BombBag_Medium", "Item_BombBundle_Small", "Item_BombchuBundle_Large", "Item_BombchuBundle_Medium", "Item_BombchuBundle_Small", "Item_Boomerang", "Item_Bottle_BluePotion", "Item_Bottle_Bug", "Item_Bottle_Fairy", "Item_Bottle_Fish", "Item_Bottle_GreenPotion", "Item_Bottle_HalfMilk", "Item_Bottle_Milk", "Item_Bottle_RedPotion", "Weapon_Sword_BrokenGiant", "Item_BrokenSword", "Item_BulletBag_Large", "Armor_Child_Default_Head", "Armor_Child_Gauntlet_Goron", "Armor_Child_Mask_Amiibo0", "Armor_Child_Mask_Amiibo1", "Armor_Child_Mask_Bunny", "Armor_Child_Mask_Gerudo", "Armor_Child_Mask_Goron", "Armor_Child_Mask_Keaton", "Armor_Child_Mask_Skull", "Armor_Child_Mask_Spooky", "Armor_Child_Mask_Truth", "Armor_Child_Mask_Zora", "Item_ClaimCheck", "Item_CojiroCucco", "Weapon_Shield_DarkLinkShield", "Item_DekuStickBundle_Small", "Item_DekuNut", "Item_DekuSeed", "Item_Dungeon_BossKey", "Item_Dungeon_Compass", "Item_Dungeon_Map", "Item_Dungeon_SmallKey", "Item_Melody_EponaSong", "Item_EyeDrop", "Item_EyeballFrog", "Item_Ocarina_Fairy", "Item_Medal_Fire", "Item_FishingRod", "Item_Medal_Forest", "Weapon_Sword_Giant", "Item_Token_Gold", "Armor_Adult_Goron", "Item_Stone_Fire", "Item_Hammer", "Item_Hat_FishingPondOwner", "Armor_Adult_HoverBoot", "Weapon_Shield_Hylian", "Armor_Adult_HeavyBoot", "Armor_GlassTruth", "Item_Arrow_Fire", "Item_Arrow_Ice", "Item_Arrow_Light", "Item_Bomb", "Item_BombBag_Small", "Item_Bombchu", "Item_Bottle", "Item_Bottle_BigPoe", "Item_Bottle_BlueFire", "Item_Bottle_Letter", "Item_Bottle_Poe", "Item_BulletBag_Medium", "Item_BulletBag_Small", "Item_DekuStick", "Item_DinFire", "Item_FairyBless_DefenseDouble", "Item_FairyBless_MP", "Item_FairyBless_MPDouble", "Item_FaroreWind", "Item_Hookshot", "Item_Longshot", "Item_Melody_FireBolero", "Item_Melody_ForestMinuet", "Item_Melody_SariaSong", "Item_Melody_ShadowNocturne", "Item_Melody_SpiritRequiem", "Item_Melody_StormSong", "Item_Melody_SunSong", "Item_Melody_WaterSerenade", "Item_Melody_ZeldaLullaby", "Item_NayruLove", "Item_Quiver_Small", "Item_ScrubBusinessSeed", "Item_Token_Gerudo", "Item_WeirdEgg", "Pouch_Amiibo", "Weapon_Range_Bow", "Weapon_Range_SlingShot", "Weapon_Shield_Deku", "Weapon_SinkingLure", "Weapon_Sword_Kokiri", "Item_NormalBoot", "Armor_Adult_Normal", "Item_Stone_Forest", "Item_Medal_Light", "Weapon_Lure", "Item_MagicBean", "Armor_Mask_Base", "Weapon_Sword_Master", "Weapon_Shield_Mirror", "Item_NutBag_Large", "Item_NutBag_Medium", "Item_NutBag_Small", "Item_Ocarina_Time", "Item_OddMushroom", "Item_OddPowder", "Item_PoacherSaw", "Item_PocketCucco", "Item_PocketEgg", "Item_Melody_LightPrelude", "Item_Prescription", "Item_Quiver_Large", "Item_Quiver_Medium", "Item_Scale_Gold", "Item_Scale_Silver", "Item_Melody_ScarecrowSong", "Item_Medal_Shadow", "Item_Melody_TimeSong", "Item_Medal_Spirit", "Item_StickBag_Large", "Item_StickBag_Medium", "Item_StickBag_Small", "Weapon_Sword_DarkLinkSword", "Item_Wallet_Large", "Item_Wallet_Medium", "Item_Wallet_Small", "Item_Medal_Water", "Item_ZeldaLetter", "Armor_Adult_Zora", "Item_Stone_Water"};
constexpr std::string_view Enemies[]{"Enemy_Amos", "Enemy_Anubis", "Enemy_BabyDodongo", "Enemy_Bari", "Enemy_Beamos", "Enemy_Biri", "Enemy_Bubble_Blue", "Enemy_Bubble_Green", "Enemy_Bubble_Green_Big", "Enemy_Bubble_Red", "Enemy_Bubble_White", "Enemy_CrazyFloorTile", "Enemy_DeadHand", "Enemy_Dekubaba", "Enemy_Dekubaba_Withered", "Enemy_Dinolfos", "Enemy_Dodongo", "Enemy_DungeonBoss_Barinade", "Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_Dodongo", "Enemy_DungeonBoss_Ganon", "Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Gohma", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_PhantomGanon", "Enemy_DungeonBoss_Twinrova", "Enemy_DungeonBoss_Volvagia", "Enemy_Floormaster", "Enemy_FlyingPot", "Enemy_Freezard", "Enemy_Gibdo", "Enemy_Gohma_Egg", "Enemy_Gohma_Larva", "Enemy_Guay", "Enemy_IronKnuckle_Black", "Enemy_IronKnuckle_Silver", "Enemy_IronKnuckle_White", "Enemy_Keese", "Enemy_Keese_Fire", "Enemy_Keese_Ice", "Enemy_Leever", "Enemy_Leever_Big", "Enemy_LikeLike", "Enemy_Lizalfos", "Enemy_MiddleBoss_BigOcto", "Enemy_MiddleBoss_DarkLink", "Enemy_MiddleBoss_FlareDancer", "Enemy_MiddleBoss_GerudoThief", "Enemy_MiddleBoss_IronKnuckle_Nabooru", "Enemy_MiddleBoss_Lizalfos", "Enemy_MiddleBoss_Poe_Meg", "Enemy_Moblin", "Enemy_Moblin_Club", "Enemy_Octorok", "Enemy_Peahat", "Enemy_Peahat_Larva", "Enemy_Poe", "Enemy_Poe_Amy", "Enemy_Poe_Beth", "Enemy_Poe_Big", "Enemy_Poe_Flat", "Enemy_Poe_Hyrule", "Enemy_Poe_Joelle", "Enemy_Poe_Sharp", "Enemy_ReDead", "Enemy_Scrub_Business", "Enemy_Scrub_Deku", "Enemy_Scrub_Mad", "Enemy_Shabom", "Enemy_ShellBlade", "Enemy_SkullKid", "Enemy_Skulltula", "Enemy_Skulltula_Gold", "Enemy_Skullwalltula", "Enemy_Spike", "Enemy_Stalchild", "Enemy_Stalfos", "Enemy_Stalfos_Weak", "Enemy_Stinger", "Enemy_Tailpasaran", "Enemy_Tektite_Blue", "Enemy_Tektite_Red", "Enemy_Tentacle_Blue", "Enemy_Tentacle_Green", "Enemy_Tentacle_Red", "Enemy_TorchSlug", "Enemy_TrickyDoor", "Enemy_TrickyDoor_SpiritTemple", "Enemy_Wallmaster", "Enemy_Wolfos", "Enemy_Wolfos_White"};
constexpr float AimFov = 40.0f;
constexpr long long AimHoldMs = 120;

std::atomic<bool> g_stop{false};
std::thread g_thread;
std::mutex g_mutex;

void PublishOff() {
    Block b{};
    b[0] = 0x04 | 0x08;
    const Effect off = Off();
    std::copy(off.begin(), off.end(), b.begin() + 10);
    std::copy(off.begin(), off.end(), b.begin() + 21);
    Common::DualSenseEffects::Publish(b);
}

void Run(Core::System& system, u64 main_base) {
    Common::SetCurrentThreadName("OotDualSense");
    bool checked = false;
    std::vector<std::string> names;
    Actions act;
    Combat combat;
    Scene scene;
    Events events;
    Moments mom;
    Quad quad;
    int prev_buttons = 0, swing_dir = 1, combo_n = 0, last_ammo = -1, last_chus = -1, prev_dir = -1, prev_l2b = 0, prev_r2b = 0;
    int touch_y0 = 0, touch_steps = 0, touch_free_y = 0, shield_charge = 0;
    long long square_down = -1, last_swing = 0, next_beat = 0, aim_end = -1, nav_repeat = 0, next_sonar = 0, heat_since = -1;
    long long next_heat = 0, next_drop = 0, touch_start = -1, touch_free = -1, led_level_until = 0, navi_until = 0;
    long long atk_at = -10000, air_min_vy = 0, last_shield_charge = 0;
    float charge = 0, min_vy = 0, atk_side = 0, atk_front = 1;
    double iron_ph = 0;
    bool dry_clicked = false, full_draw = false, full_charge = false, was_aiming = false, was_paused = false;
    bool touch_moved = false, touch_fired = false, volvagia_under = false;
    std::string prev_shield, prev_boot, prev_armor, prev_weapon, touch_key = "master";
    float tune_l2 = 0.75f, tune_r2 = 1.0f, tune_master = 1.0f; // tuning.json's defaults
    Rgb navi_color{};
    std::mt19937 rng{11};
    std::uniform_real_distribution<double> uniform{0.0, 1.0};
    std::string shield, boot, armor, map, weapon;
    long long brace_until = 0, next_telegraph = 0, led_chip_until = 0, led_show_until = 0, lens_until = 0;
    long long last_mp_drop = 0, fuse_until = 0, next_kills = 0, next_flags = 0, next_sparkle = 0;
    float led_show = 0;
    int last_rupee = -1, last_mp = -1, last_nuts = -1, last_bombs = -1, last_hour = -1;
    bool kills_ready = false, flags_ready = false;
    std::unordered_map<u32, int> kill_state;
    std::unordered_map<u32, bool> flag_state;
    std::unordered_map<std::string, long long> recent_kills;
    long long next_strings = 0, aim_since = -1, ride_since = -1, flash_until = 0, flash_ms = 1;
    long long dead_until = 0, dead_at = -1, jolt = 0, r_tick = 0, l_kick = 0, celebrate_until = 0;
    long long led_sweep_until = 0, last_now = 0;
    int last_life = -1, last_max_life = -1, led_src = -1;
    Rgb flash{}, light_now{0, 170, 40};
    const auto t0 = Clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        // Main is loaded before the process runs: nothing is read until the system is on and not stopping
        if (!system.IsPoweredOn() || system.IsShuttingDown() || !system.ApplicationProcess()) {
            continue;
        }
        Oot oot{Mem{system.ApplicationMemory(), main_base}};
        if (!checked) {
            if (!oot.StillThere()) {
                continue; // (the module may not be readable yet; another build never matches)
            }
            checked = true;
            LOG_INFO(Input, "OoT DualSense: triggers and lights from the game (a port of Pulse of Time)");
        }
        const long long now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        const float dt = std::clamp((now - last_now) / 1000.0f, 0.001f, 0.25f);
        last_now = now;
        GameData gd{oot.mem};
        const State s = oot.Read();
        const auto& pad = Common::DualSenseEffects::GetInput();
        const bool have_pad = pad.valid.load();
        const float pull = have_pad ? pad.r2.load() / 255.0f : -1.0f;
        const int buttons = have_pad ? pad.buttons.load() : 0;
        if (!s.in_game || gd.GetBool(HLoading).value_or(false)) {
            last_life = last_rupee = last_mp = last_max_life = last_nuts = last_bombs = last_ammo = last_chus = -1;
            aim_since = -1;
            min_vy = 0;
            charge = 0;
            fuse_until = 0;
            PublishOff();
            continue;
        }
        if (names.empty()) {
            names = ReadVerbNames(oot.mem);
            LOG_INFO(Input, "OoT DualSense: {} action verbs read from the game", names.size());
        }
        std::array<int, 5> verbs{};
        for (int i = 0; i < 5; i++) {
            verbs[i] = oot.Verb(i);
        }
        const World world = oot.ReadWorld();
        std::string anim;
        float anim_frame = 0;
        oot.FirstAnim(anim, anim_frame);

        const bool paused = gd.GetBool(HPause).value_or(false) || gd.GetBool(HPrePause).value_or(false) ||
                            gd.GetInt(HPauseReq).value_or(0) > 0;
        const bool child = gd.GetBool(HChild).value_or(false);
        const int ammo = (child ? gd.GetInt(HSeeds) : gd.GetInt(HArrows)).value_or(0);
        const int max_life = gd.GetInt(HMaxLife).value_or(-1);
        if (now >= next_strings) {
            shield = gd.GetStr(HShield).value_or("");
            boot = gd.GetStr(HBoot).value_or("");
            armor = gd.GetStr(HArmor).value_or("");
            map = gd.GetStr(HMap).value_or("");
            weapon = gd.GetStr(HWeapon).value_or("");
            scene.SetMap(map);
            next_strings = now + 500;
        }
        const bool sword = !weapon.empty();
        const bool iron = boot.find("HeavyBoot") != std::string::npos;
        const bool hover = boot.find("HoverBoot") != std::string::npos;
        const auto flash_to = [&](float r, float g, float b, long long ms) {
            flash = {r, g, b};
            flash_ms = ms;
            flash_until = now + ms;
        };
        // how an enemy dies, by its name (Daemon.Family / KillFx): the flashes and the celebration
        const auto kill_fx = [&](const std::string& name) {
            const auto has = [&](std::initializer_list<std::string_view> k) {
                for (const auto x : k) {
                    if (ContainsI(name, x)) return true;
                }
                return false;
            };
            const bool boss = has({"DungeonBoss"}), mid = !boss && has({"MiddleBoss"});
            const bool token = !boss && !mid && has({"Skulltula_Gold"});
            const bool ghost = !boss && !mid && !token && !has({"Stalfos", "Stalchild", "IronKnuckle", "Lizalfos", "Dinolfos", "Moblin", "Amos", "Anubis"}) &&
                               has({"Poe", "Floormaster", "SkullKid"});
            if (name.find("Ganon") != std::string::npos && boss) {
                flash_to(255, 255, 255, 4000);
                celebrate_until = now + 2500;
            }
            if (boss) {
                flash_to(255, 220, 120, 2500);
                celebrate_until = now + 1400;
            } else if (mid) {
                flash_to(255, 200, 120, 1200);
                celebrate_until = now + 800;
            } else if (token) {
                flash_to(255, 200, 0, 300);
            } else if (ghost) {
                flash_to(160, 90, 255, 600);
            }
        };
        // enemies: our hits, their deaths, what they are doing
        combat.Discover(oot.mem, oot.link, now);
        const auto hits = !paused ? combat.Poll(oot.mem, oot.link, names) : std::vector<Combat::Hit>{};
        for (const auto& h : hits) {
            // the blade lands where the enemy is: in front the fingertips on that side shake
            const float st = 0.6f + (std::min)(1.0f, h.damage / static_cast<float>((std::max)(1, h.max))) * 0.9f;
            const auto pl = Quad::Place(h.pan, h.front, (std::min)(1.0f, st * 0.8f));
            quad.Pulse(now, (std::max)(pl.l2, 2.0f), (std::max)(pl.r2, 3.0f), h.boss ? 35 : 55, h.boss ? 160 : 90);
            if (combo_n >= 3) quad.Pulse(now, 6, 6, 40, 120);
            flash_to(255, 245, 220, 110);
            led_chip_until = now + 90; // the target's life bar loses a chip
            if (h.killed) {
                kill_fx(h.name);
                recent_kills[h.name] = now;
            }
        }
        for (const auto& a : combat.acts) {
            const std::string& v = a.verb;
            const bool windup = v.find("Attack") != std::string::npos || v == "Charge" || v == "PrepareWeapon" ||
                                v == "SpecialAttackPrepare" || v == "PreLaunch" || v == "LandCharge" || v == "Inhale";
            const auto apl = Quad::Place(a.pan, a.front, 0.8f);
            if (windup && a.distance < 8 && now >= next_telegraph) {
                atk_side = a.pan; // a blow soon after comes from here
                atk_front = a.front;
                atk_at = now;
                quad.Pulse(now, apl.l2 * 0.5f, apl.r2 * 0.5f, 30, 220, 0, false, true);
                brace_until = now + 450; // the shield hand tenses
                next_telegraph = now + 350;
                flash_to(255, 170, 40, 160);
            }
            else if (v == "Appear" || v == "JumpOut" || v == "Summon" || v == "Transform" || v == "Metamorphose") {
                quad.Pulse(now, apl.l2 * 0.6f, apl.r2 * 0.6f, 25, 400, 0, true, true);
            } else if ((v.starts_with("InterruptGuard") || v == "Parry") && now - last_swing < 500) {
                quad.Pulse(now, 0, 8, 80, 90);
            } else if ((v == "GetAngry" || v == "Uproar" || v == "Shout") && a.distance < 15) {
                quad.Pulse(now, apl.l2 * 0.5f, apl.r2 * 0.5f, 30, 300, 0, true, true);
            }
            if ((v == "RangedAttack" || v == "RangedLargeAttack") && a.distance < 20) quad.Pulse(now, apl.l2 * 0.7f, apl.r2 * 0.7f, 90, 120, 200);
            if (v == "InterruptBrokenArmor") {
                quad.Pulse(now, apl.l2, apl.r2, 120, 80);
                quad.Pulse(now, 5, 5, 60, 120, 120);
                flash_to(255, 255, 255, 150);
            }
            if (v == "InterruptWeakPointHit") {
                quad.Pulse(now, 6, 6, 120, 80);
                flash_to(255, 255, 160, 150);
            }
            if (v == "InterruptRestraintRideOnPlayer" && a.distance < 3) act.grabbed_until = now + 2500;
            if ((v == "ReleaseRestraint" || v == "Unstick") && a.distance < 3) act.grabbed_until = 0;
        }
        const bool enemy_near = combat.nearby > 0 && combat.nearest_distance < 10;
        const bool boss_near = combat.boss_near;
        // kills the walk didn't see: the game's per-enemy counters
        if (now >= next_kills) {
            for (const auto name : Enemies) {
                const u32 hash = Hash(std::string("DefeatedEnemyNum.") + std::string(name));
                const int v = gd.GetInt(hash).value_or(-1);
                if (v < 0) continue;
                const std::string n{name};
                const auto it = kill_state.find(hash);
                const auto rk = recent_kills.find(n);
                if (kills_ready && it != kill_state.end() && v > it->second && !(rk != recent_kills.end() && now - rk->second < 1500)) {
                    kill_fx(n);
                }
                kill_state[hash] = v;
            }
            kills_ready = true;
            next_kills = now + 300;
        }
        // treasure
        if (now >= next_flags) {
            for (const auto name : Items) {
                const u32 hash = Hash(std::string("IsGet.") + std::string(name));
                const bool v = gd.GetBool(hash).value_or(false);
                const auto it = flag_state.find(hash);
                if (flags_ready && v && it != flag_state.end() && !it->second) {
                    const auto has = [&](std::string_view k) { return name.find(k) != std::string_view::npos; };
                    const bool song = has("Melody"), medal = !song && !has("BossKey") && !has("SmallKey") && !has("Map") &&
                                                             !has("Compass") && (has("Medal") || has("Stone"));
                    const bool key = !song && has("SmallKey"), boss_key = !song && has("BossKey");
                    const bool map_item = !song && !boss_key && !key && (has("Map") || has("Compass"));
                    if (song) flash_to(120, 180, 255, 1300);
                    else if (medal) flash_to(255, 255, 255, 2500);
                    else flash_to(255, 200, 40, 1300);
                    if (!song && !key && !map_item) celebrate_until = now + 900; // an item, a medal, the boss key
                }
                flag_state[hash] = v;
            }
            flags_ready = true;
            next_flags = now + 400;
        }
        if (last_max_life > 0 && max_life > last_max_life) {
            flash_to(255, 60, 120, 1600); // a heart container
            celebrate_until = now + 1500;
        }
        last_max_life = max_life;

        const bool stick_idle = !have_pad || (std::abs(pad.lx.load() - 128) < 24 && std::abs(pad.ly.load() - 128) < 24);
        const auto pressed = [&](int b) { return (buttons & b) != 0 && (prev_buttons & b) == 0; };
        const auto released = [&](int b) { return (buttons & b) == 0 && (prev_buttons & b) != 0; };
        const auto ao = act.Update(now, s, verbs, names, anim, anim_frame, armor, map, quad, buttons, prev_buttons, stick_idle, paused);
        l_kick = (std::max)(l_kick, act.kick);
        jolt = (std::max)(jolt, act.jolt);
        r_tick = (std::max)(r_tick, act.r_tick);
        if (act.flash_until > now && act.flash_until > flash_until) {
            flash = act.flash_color;
            flash_ms = act.flash_until - now;
            flash_until = act.flash_until;
        }

        // aiming: the view narrows to 40 degrees, held 120 ms, never while busy or with the hookshot
        const bool at_aim = std::abs(s.fov - AimFov) < 0.6f;
        aim_since = at_aim ? (aim_since < 0 ? now : aim_since) : -1;
        const bool aiming = aim_since >= 0 && now - aim_since >= AimHoldMs && !act.busy &&
                            !act.HasVerb("UseHookshot") && !act.HasVerb("HookshotShoot");
        if (was_aiming && !aiming) aim_end = now;
        was_aiming = aiming;
        last_ammo = ammo;
        if (aiming && ammo == 0 && pull > 0.85f && !dry_clicked) dry_clicked = true; // the dry click: R2 jams
        if (pull < 0.5f) dry_clicked = false;
        if (aiming && ammo > 0 && pull >= 0.95f && !full_draw) { // full draw: a click point at the end of R2's travel
            quad.Pulse(now, 0, 3, 250, 20);
            full_draw = true;
        }
        if (pull < 0.85f) full_draw = false;

        // sword: swings, the spin attack's charge (Square is no swing while busy, aiming or with full hands)
        const bool can_swing = sword && !aiming && !paused && !act.busy && !act.hands_busy;
        if (can_swing && pressed(BSquare)) {
            square_down = now;
            if (now - last_swing > 120) {
                combo_n = now - last_swing < 650 ? (std::min)(combo_n + 1, 3) : 1;
                float cs = combo_n == 1 ? 0.8f : combo_n == 2 ? 0.95f : 1.2f;
                if (combat.nearest_distance < 3) cs *= 1.15f;
                if (weapon.find("Giant") != std::string::npos || weapon.find("Biggoron") != std::string::npos) {
                    quad.Pulse(now, 5, 5, 45, 160, 0, true, true); // a two-handed blade: both triggers carry it
                } else { // the arc runs across the front, from one fingertip to the other
                    const int tf = weapon.find("Kokiri") != std::string::npos ? 110 : 85;
                    const float a1 = static_cast<float>(static_cast<int>(3 * cs)), a2 = static_cast<float>(static_cast<int>(4.5f * cs));
                    if (swing_dir == 1) {
                        quad.Pulse(now, a1, 0, tf, 50);
                        quad.Pulse(now, 0, a2, tf, 60, 70);
                    } else {
                        quad.Pulse(now, 0, a1, tf, 50);
                        quad.Pulse(now, a2, 0, tf, 60, 70);
                    }
                }
                swing_dir = 3 - swing_dir;
                last_swing = now;
            }
        }
        if ((buttons & BSquare) != 0 && square_down >= 0 && can_swing && now - square_down > 350) {
            charge = (std::min)(1.0f, (now - square_down - 350) / 900.0f);
        }
        if (released(BSquare) || !can_swing) {
            if (charge >= 0.35f && can_swing) { // the spin goes once round the controller
                flash_to(140, 200, 255, 300);
                quad.Pulse(now, 8, 0, 60, 110);
                quad.Pulse(now, 0, 8, 60, 110, 120);
            }
            charge = 0;
            square_down = -1;
        }
        if (charge >= 1 && !full_charge) { // the spin is ready
            quad.Pulse(now, 3, 3, 250, 25);
            flash_to(140, 200, 255, 120);
            full_charge = true;
        }
        if (charge < 1) full_charge = false;

        // the pause screen: a page you hold
        if (paused != was_paused) quad.Pulse(now, 2, 2, 60, 50);
        was_paused = paused;
        if (paused && have_pad) {
            const int dpad = pad.dpad.load();
            const int dir = dpad < 8 ? dpad : StickDir(pad.lx.load(), pad.ly.load());
            if (dir >= 0 && (dir != prev_dir || now >= nav_repeat)) {
                const float np = dir == 5 || dir == 6 || dir == 7 ? -0.75f : dir == 1 || dir == 2 || dir == 3 ? 0.75f : 0.0f;
                quad.Pulse(now, np < 0 ? 2 : 0, np > 0 ? 2 : 0, 120, 25);
                nav_repeat = now + (dir != prev_dir ? 380 : 130);
            }
            prev_dir = dir;
            if (pressed(BCircle)) quad.Pulse(now, 3, 3, 80, 40);
            const bool zl = pad.l2.load() > 200, zr = pad.r2.load() > 200;
            if (pressed(BL1) || (zl && prev_l2b == 0)) quad.Pulse(now, 4, 0, 60, 50);
            if (pressed(BR1) || (zr && prev_r2b == 0)) quad.Pulse(now, 0, 4, 60, 50);
            prev_l2b = zl ? 1 : 0;
            prev_r2b = zr ? 1 : 0;
        } else {
            prev_dir = -1;
        }

        // gear changes: each has its own weight
        if (!prev_weapon.empty() && weapon != prev_weapon && !weapon.empty()) quad.Pulse(now, 0, 4, 90, 60);
        if (!prev_shield.empty() && shield != prev_shield && !shield.empty()) quad.Pulse(now, 4, 0, 70, 60);
        if (prev_boot != boot && prev_armor.size() + prev_boot.size() + prev_weapon.size() > 0 && boot.find("HeavyBoot") != std::string::npos) {
            quad.Pulse(now, 6, 0, 30, 90);
            quad.Pulse(now, 0, 6, 30, 90, 180);
        }
        prev_shield = shield;
        prev_boot = boot;
        prev_armor = armor;
        if (!weapon.empty()) prev_weapon = weapon;

        // life: a hit jolts the triggers, death fades the light, a fairy brings it back
        if (last_life >= 0 && s.life < last_life) {
            jolt = now + 200;
            if (s.life <= 0) {
                dead_until = now + 2500;
                dead_at = now;
            } else { // the blow comes from the enemy that just wound up, else from the nearest one
                const float st = (0.6f + (last_life - s.life) / 8.0f) * 1.0f;
                const bool from_attacker = now - atk_at < 1500;
                if (from_attacker || combat.nearest_distance < 6) {
                    const auto pl = Quad::Place(from_attacker ? atk_side : combat.nearest_side, from_attacker ? atk_front : combat.nearest_front,
                                                (std::min)(1.0f, st));
                    quad.Pulse(now, (std::max)(pl.l2, 3.0f), (std::max)(pl.r2, 3.0f), 40, 220);
                }
                flash_to(255, 0, 0, 260);
            }
        }
        if (last_life == 0 && s.life > 0 && dead_at >= 0 && now - dead_at < 15000) {
            flash_to(255, 150, 220, 1200);
            dead_until = 0;
            dead_at = -1;
        } else if (last_life > 0 && s.life > last_life) {
            flash_to(60, 255, 120, 350);
        }
        last_life = s.life;

        // Epona: on her and fast for 300 ms
        const bool fast = act.HasVerb("Ride") && s.SpeedH() > 2.5f;
        ride_since = fast ? (ride_since < 0 ? now : ride_since) : -1;
        const bool riding = ride_since >= 0 && now - ride_since > 300;

        // rupees: a gold flash, the wallet filling on the LEDs; magic: one drop is a spell, a drain the Lens of Truth
        const int rupee = gd.GetInt(HRupee).value_or(-1), mp = gd.GetInt(HMp).value_or(-1);
        if (last_rupee >= 0 && rupee > last_rupee) {
            flash_to(255, 170, 0, 220);
            const int cap = gd.GetInt(HWalletL).value_or(0) > 0 ? 500 : gd.GetInt(HWalletM).value_or(0) > 0 ? 200 : 99;
            led_show = std::clamp(rupee / static_cast<float>(cap), 0.05f, 1.0f);
            led_show_until = now + 1400;
        }
        last_rupee = rupee;
        if (last_mp >= 0 && mp < last_mp) {
            lens_until = now - last_mp_drop < 1500 ? now + 1500 : lens_until;
            if (now >= lens_until) flash_to(170, 60, 255, 300);
            const int mp_max = gd.GetInt(HMpMax).value_or(0);
            if (mp_max > 0) {
                led_show = std::clamp(mp / static_cast<float>(mp_max), 0.05f, 1.0f);
                led_show_until = now + 1200;
            }
            last_mp_drop = now;
        }
        last_mp = mp;
        // items used up: a Deku nut's flash, a bomb's fuse
        const int nuts = gd.GetInt(HNuts).value_or(-1), bombs = gd.GetInt(HBombs).value_or(-1);
        const int chus = gd.GetInt(HChus).value_or(-1);
        if (last_nuts >= 0 && nuts < last_nuts) {
            flash_to(255, 255, 240, 450);
            quad.Pulse(now, 5, 5, 120, 80);
        }
        if (last_chus >= 0 && chus < last_chus) quad.Pulse(now, 3, 3, 45, 1200, 0, true, true);
        last_chus = chus;
        if (last_bombs >= 0 && bombs < last_bombs) fuse_until = now + 4500;
        last_nuts = nuts;
        last_bombs = bombs;
        // the day turns
        const int hour = world.seconds >= 0 ? world.seconds / 3600 : -1;
        if (last_hour >= 0 && hour >= 0 && hour != last_hour && !world.indoor) {
            if (hour == 6) flash_to(255, 150, 80, 2500);
            if (hour == 18) flash_to(120, 60, 160, 2500);
        }
        last_hour = hour;

        // landing from a height jolts the triggers
        if (s.vy < -1.0f) {
            min_vy = (std::min)(min_vy, s.vy);
            air_min_vy = now;
        } else if (min_vy < -5.0f && std::abs(s.vy) < 0.5f) {
            const float st = std::clamp((-min_vy - 4.0f) / 12.0f, 0.2f, 1.2f);
            if (st > 0.5f) jolt = (std::max)(jolt, now + 90);
            min_vy = 0;
        } else if (now - air_min_vy > 1500) {
            min_vy = 0;
        }
        // sonar: an enemy close by but not in front of you pings from where it is, faster as it nears
        if (!paused && !act.busy && combat.nearest_distance < 9 && combat.nearest_front < 0.3f && now >= next_sonar) {
            const auto pl = Quad::Place(combat.nearest_side, combat.nearest_front, 0.5f);
            quad.Pulse(now, pl.l2 * 0.4f, pl.r2 * 0.4f, 100, 40);
            next_sonar = now + static_cast<long long>(350 + 1050 * std::clamp((combat.nearest_distance - 1.5f) / 7.5f, 0.0f, 1.0f));
        }
        // heat: fire places without the Goron tunic press in slow waves that grow with time
        const bool hot = (ContainsI(map, "Crater") || ContainsI(map, "FireTemple")) && armor.find("Goron") == std::string::npos;
        heat_since = hot ? (heat_since < 0 ? now : heat_since) : -1;
        const float heat = hot ? (std::min)(1.0f, 0.3f + (now - heat_since) / 60000.0f) : 0.0f;
        if (hot && !paused && now >= next_heat) {
            quad.Pulse(now, 2 * heat, 2 * heat, 20, 600, 0, true, true);
            next_heat = now + 2600 - static_cast<long long>(1200 * heat);
        }
        // rain: drops tap the fingertips
        if (act.raining && !world.indoor && !paused && now >= next_drop) {
            const bool left = uniform(rng) < 0.5;
            quad.Pulse(now, left ? 1.5f : 0.0f, left ? 0.0f : 1.5f, 200, 30);
            next_drop = now + 200 + static_cast<long long>(uniform(rng) * 500);
        }
        // the world around: treasure glints, danger rolling at you, a lock opened
        if (!paused) {
            const Combat::Seen* treasure = nullptr;
            const Combat::Seen* danger = nullptr;
            for (const auto& o : combat.objects) {
                if (o.kind == Combat::Kind::Treasure && o.distance < 9 && (treasure == nullptr || o.distance < treasure->distance)) treasure = &o;
                if (o.kind == Combat::Kind::Danger && o.distance < 16 && o.closing > 1.5f && (danger == nullptr || o.distance < danger->distance)) danger = &o;
            }
            if (treasure && now >= next_sparkle && !act.in_event) {
                const auto pl = Quad::Place(treasure->pan, treasure->front, 0.6f);
                quad.Pulse(now, pl.l2 * 0.3f, pl.r2 * 0.3f, 200, 30);
                next_sparkle = now + static_cast<long long>(350 + 1300 * std::clamp(treasure->distance / 9, 0.0f, 1.0f));
            }
            if (danger && danger->distance < 4 && danger->front > 0) {
                quad.Pulse(now, 5 * (1 - danger->pan) / 2 + 2, 5 * (1 + danger->pan) / 2 + 2, 30, 80, 0, true, true);
            }
            if (combat.gone.size() == 1 && combat.gone[0].kind == Combat::Kind::Lock) quad.Pulse(now, 4, 4, 60, 60, 350);
        }
        // iron boots: every step a heavy stamp
        const bool grounded = std::abs(s.vy) < 0.5f && !act.HasVerb("Swim") && !riding;
        const float walk = (std::max)(s.SpeedH(), act.PosSpeed());
        if (iron && grounded && walk > 0.4f && !paused && !act.carried) {
            const double before = iron_ph;
            iron_ph += walk * 0.75 * dt;
            if (std::floor(iron_ph) != std::floor(before)) {
                const bool left_foot = static_cast<long long>(std::floor(iron_ph)) % 2 == 0;
                quad.Pulse(now, left_foot ? 3 : 1, left_foot ? 1 : 3, 35, 70);
            }
        }
        events.Update(now, oot.EventName(), act.in_event, paused, act, quad);
        mom.Update(now, gd, act, s, map, &combat, weapon, paused, quad, pull);

        // boss signatures: the nearest boss's colour
        std::optional<Rgb> boss_light;
        if (!paused) {
            const Combat::Foe* boss = nullptr;
            for (const auto& f : combat.live) {
                if (f.boss && (boss == nullptr || f.distance < boss->distance)) boss = &f;
            }
            if (boss) {
                const std::string& bn = boss->name;
                const auto has = [&](std::string_view k) { return bn.find(k) != std::string::npos; };
                for (const auto& a : combat.acts) {
                    if (!a.boss) continue;
                    const auto apl = Quad::Place(a.pan, a.front, 0.9f);
                    if (a.name.find("Volvagia") != std::string::npos && (a.verb == "Appear" || a.verb == "JumpOut")) {
                        quad.Pulse(now, apl.l2 + 3, apl.r2 + 3, 30, 300, 0, true, true); // it bursts out of the lava
                    }
                }
                if ((has("Twinrova") || has("Kotake") || has("Koume")) && act.HasVerb("InterruptGuard") && now - last_shield_charge > 600) {
                    shield_charge++; // every block with the Mirror Shield charges it; the third releases the blast
                    last_shield_charge = now;
                    led_show = (std::min)(1.0f, shield_charge / 3.0f);
                    led_show_until = now + 2500;
                    if (shield_charge >= 3) {
                        quad.Pulse(now, 8, 8, 40, 400);
                        shield_charge = 0;
                    }
                }
                boss_light = has("Volvagia") ? Rgb{255, 60, 0}
                             : has("Twinrova") || has("Kotake") || has("Koume") ? (now / 1500 % 2 == 0 ? Rgb{255, 60, 0} : Rgb{60, 160, 255})
                             : has("Barinade") ? Rgb{80, 200, 255}
                             : has("BongoBongo") ? Rgb{90, 0, 140}
                             : has("Morpha") ? Rgb{20, 90, 255}
                                             : Rgb{200, 0, 30};
            }
        }
        const bool low = s.life > 0 && s.life * 4 <= s.life_max;
        const bool critical = s.life > 0 && s.life <= 4;


        // ---- triggers ----
        const int heavy = iron ? 1 : hover ? -2 : 0;
        Effect r = Off(), l = Off();
        if (now < dead_until) {
            r = l = Off();
        } else if (paused) {
            r = l = Weapon(3, 5, 3); // menu: light button detents, ticks on top
            quad.Apply(now, r, l);
        } else {
            // firm by default: a short, hard click point on R2, the full weight of the shield on L2
            if (aiming && ammo > 0) {
                r = child ? Bow(0, 6, std::clamp(7 + heavy, 1, 8), 8) : Bow(0, 7, 8, 8);
            } else if (aiming && dry_clicked) {
                r = Feedback(2, 8); // empty quiver: after the dry click R2 jams
            } else if (aiming) {
                r = Feedback(1, 2);
            } else {
                r = Weapon(2, 4, std::clamp(8 + heavy, 1, 8));
            }
            l = shield.find("Mirror") != std::string::npos || shield.find("Hylian") != std::string::npos
                    ? Feedback(0, std::clamp(8 + heavy, 1, 8))
                : shield.find("Deku") != std::string::npos ? Slope(0, 9, std::clamp(6 + heavy, 1, 8), 8)
                                                           : Feedback(1, std::clamp(5 + heavy, 1, 8));
            if (ao.r) r = *ao.r;
            if (ao.l) l = *ao.l;
            if (charge > 0) r = l = Vibration(0, 2 + static_cast<int>(6 * charge), 30 + static_cast<int>(60 * charge));
            if (riding) r = l = Feedback(1, 4); // the reins
            if (now < brace_until && !riding) l = Feedback(0, 8); // an enemy winds up: the shield hand braces
            if (ao.free_triggers) r = l = Off();
            if (mom.r) r = *mom.r;
            if (mom.l) l = *mom.l;
            if (low) { // the heartbeat goes through the triggers: lub hard, dub softer
                const long long period = critical ? 650 : 1000;
                const long long ph = now % period;
                if (now >= next_beat) {
                    if (!ao.free_triggers) {
                        quad.Pulse(now, critical ? 8 : 5, critical ? 8 : 5, 30, 75, 0, false, true);
                        quad.Pulse(now, critical ? 5 : 3, critical ? 5 : 3, 30, 65, 170, false, true);
                    }
                    next_beat = now - ph + period;
                }
            }
            if (now < celebrate_until) r = l = Machine(1, 9, 6, 2, 30, 2);
            r = Scale(r, tune_r2);
            l = Scale(l, tune_l2);
            quad.Apply(now, r, l);
            if (now < r_tick) r = Vibration(3, 6, 60);
            if (now < l_kick) l = Vibration(0, 8, 35);
            if (now < jolt) r = l = Vibration(0, 8, 45);
        }

        // Navi's compass: swipe up on the touchpad (no click) and a pulse runs toward what matters most nearby
        if (have_pad && !paused) {
            if (pad.touch_down.load() && !pad.touch_click.load()) {
                if (touch_free < 0) {
                    touch_free = now;
                    touch_free_y = pad.touch_y.load();
                    touch_fired = false;
                }
            } else {
                touch_free = -1;
            }
            if (touch_free >= 0 && !touch_fired && now - touch_free < 450 && touch_free_y - pad.touch_y.load() > 350) {
                touch_fired = true;
                const Combat::Seen* goal = nullptr;
                const Combat::Foe* foe = nullptr;
                for (const auto& o : combat.objects) {
                    if (o.kind == Combat::Kind::Treasure && (goal == nullptr || o.distance < goal->distance)) goal = &o;
                }
                for (const auto& f : combat.live) {
                    if (f.distance < 25 && (foe == nullptr || f.distance < foe->distance)) foe = &f;
                }
                if (goal || foe) {
                    const float side = goal ? goal->pan : foe->pan, front = goal ? goal->front : foe->front;
                    const auto pl = Quad::Place(side, front, 0.8f);
                    quad.Pulse(now, pl.l2 * 0.6f, pl.r2 * 0.6f, 150, 60, 0);
                    quad.Pulse(now, pl.l2 * 0.6f, pl.r2 * 0.6f, 150, 60, 220);
                    navi_color = goal ? Rgb{60, 255, 90} : Rgb{255, 220, 40};
                } else {
                    navi_color = Rgb{80, 140, 255};
                }
                navi_until = now + 900;
            }
        }
        // strength on the touchpad: hold the click and slide up/down to step it; a click alone cycles gentle
        // 0.6 / normal 1.0 / extreme 1.5. Left third = L2's resistance, right third = R2's, middle = everything
        // (here the voice-coil haptics' strength). The player LEDs show the level.
        if (have_pad) {
            const bool click = pad.touch_click.load();
            if (click && touch_start < 0) {
                touch_start = now;
                touch_y0 = pad.touch_y.load();
                touch_steps = 0;
                touch_moved = false;
                const int x = pad.touch_x.load();
                touch_key = x < 640 ? "triggers.L2" : x > 1280 ? "triggers.R2" : "master";
            }
            float& tune = touch_key == "triggers.L2" ? tune_l2 : touch_key == "triggers.R2" ? tune_r2 : tune_master;
            if (click && touch_start >= 0 && pad.touch_down.load()) {
                const int steps = (touch_y0 - pad.touch_y.load()) / 120;
                if (steps != touch_steps) {
                    const bool trig = touch_key != "master";
                    tune = std::clamp(std::round((tune + 0.1f * (steps - touch_steps)) * 10) / 10, trig ? 0.0f : 0.2f, trig ? 1.5f : 2.0f);
                    touch_steps = steps;
                    touch_moved = true;
                    led_level_until = now + 1500;
                }
            }
            if (!click && touch_start >= 0) {
                if (!touch_moved && now - touch_start < 600) {
                    tune_master = tune_master < 0.8f ? 1.0f : tune_master < 1.25f ? 1.5f : 0.6f;
                    touch_key = "master";
                    led_level_until = now + 1500;
                }
                touch_start = -1;
            }
            Common::DualSenseEffects::MasterGain().store(tune_master);
        }

        // ---- lights ----
        Rgb c = armor.find("Goron") != std::string::npos  ? Rgb{200, 25, 10}
                : armor.find("Zora") != std::string::npos ? Rgb{15, 70, 230}
                                                          : Rgb{0, 170, 40};
        if (const auto place = scene.Light(now)) c = *place;
        if (!world.indoor && world.seconds >= 0) c = TimeOfDay(c, world.seconds);
        if (act.raining && !world.indoor) c = Mix(c, {90, 100, 130}, 0.4f);
        if (ao.light) c = Mix(c, *ao.light, ao.light_mix);
        if (const auto evl = events.LightNow(now)) c = Mix(c, *evl, 0.85f);
        if (const auto ml = mom.Light(now)) c = Mix(c, ml->first, ml->second);
        if (now < lens_until) c = Mix(c, {130, 40, 210}, 0.55f + 0.15f * static_cast<float>(std::sin(now / 1000.0 * 6))); // Lens of Truth
        if (now < fuse_until) { // the fuse burns faster
            const bool on = now / (std::max)(60ll, (fuse_until - now) / 12) % 2 == 0;
            c = Mix(c, on ? Rgb{255, 120, 0} : Rgb{60, 20, 0}, 0.6f);
        }
        if (boss_near) {
            const float p = 0.5f + 0.5f * static_cast<float>(std::sin(now / 1000.0 * 2 * Pi * 0.8));
            const Rgb bl = boss_light.value_or(Rgb{220, 0, 30});
            c = Mix(c, {bl[0] * p, bl[1] * p, bl[2] * p}, 0.6f);
        } else if (enemy_near) {
            const float p = 0.5f + 0.5f * static_cast<float>(std::sin(now / 1000.0 * 2 * Pi * (1 + 3 / (std::max)(1.0f, combat.nearest_distance))));
            c = Mix(c, {255, 140 * p, 0}, 0.35f);
        }
        if (low) {
            const long long period = s.life <= 4 ? 700 : 1000;
            c = Mix(c, {(now % period) < 260 ? 255.0f : 60.0f, 0, 0}, 0.85f);
        }
        if (now < navi_until) c = Mix(c, navi_color, 0.8f * (navi_until - now) / 900.0f);
        if (now < flash_until) c = Mix(c, flash, (flash_until - now) / static_cast<float>(flash_ms));
        if (now < dead_until) {
            const float a = (dead_until - now) / 2500.0f;
            c = {180 * a, 0, 0};
        }
        if (paused) c = {c[0] * 0.25f, c[1] * 0.25f, c[2] * 0.25f};

        // player LEDs, most urgent first: wallet / magic, being held, a moment's bar, breath, the boss's life, the
        // locked-on enemy's life, else Link's own (the bow's draw, the spin charge and the touchpad's level need the
        // pad's input: not ported yet)
        float boss_frac = -1, target_frac = -1;
        for (const auto& f : combat.live) {
            if (f.boss && f.distance < 30) boss_frac = (std::max)(boss_frac, f.life / static_cast<float>(f.max));
        }
        if (act.HasVerb("LockOnWait") || act.HasVerb("LockOnMove")) {
            const Combat::Foe* best = nullptr;
            for (const auto& f : combat.live) {
                if (f.front > 0.5f && f.distance < 15 && (best == nullptr || f.distance < best->distance)) best = &f;
            }
            if (best) target_frac = best->life / static_cast<float>(best->max);
        }
        const bool grabbed_now = now < act.grabbed_until;
        const float level = touch_key == "master" ? tune_master / 2.0f : (touch_key == "triggers.L2" ? tune_l2 : tune_r2) / 1.5f;
        const int src = aiming && ammo > 0 && pull >= 0 ? 1 : charge > 0 ? 2 : now < led_level_until ? 3 : now < led_show_until ? 4 : grabbed_now ? 5 : mom.bar >= 0 ? 6 : act.air_left >= 0 ? 7
                        : boss_frac >= 0 ? 8 : target_frac >= 0 ? 9 : 10;
        const float bar = src == 1   ? pull
                          : src == 2 ? charge
                          : src == 3 ? std::clamp(level, 0.0f, 1.0f)
                          : src == 4 ? led_show
                          : src == 5 ? (std::min)(1.0f, act.grab_presses / 8.0f)
                          : src == 6 ? mom.bar
                          : src == 7 ? act.air_left
                          : src == 8 ? boss_frac
                          : src == 9 ? target_frac
                                     : (s.life_max > 0 ? (std::max)(0, s.life) / static_cast<float>(s.life_max) : 0.0f);
        int leds = static_cast<int>(std::ceil(5 * std::clamp(bar, 0.0f, 1.0f)));
        if (now < led_chip_until && (boss_frac >= 0 || target_frac >= 0)) leds = (std::max)(0, leds - 1); // a chip breaks off
        if (act.air_left >= 0 && act.air_left < 0.25f && now % 400 < 200) leds = 0; // last breath blinks
        if (critical && now % 650 >= 260 && now >= led_show_until && charge == 0 && !aiming) leds = 0; // the last heart blinks with the beat
        int led_mask = (1 << leds) - 1;
        // a new meter announces itself: one light runs across before it shows its value
        if (src != led_src) {
            if (led_src >= 0 && src != 10) led_sweep_until = now + 240;
            led_src = src;
        }
        if (now < led_sweep_until) {
            led_mask = 1 << static_cast<int>(std::clamp<long long>((now - (led_sweep_until - 240)) / 48, 0, 4));
        }
        const u8 mic = (std::max)(static_cast<u8>(s.life > 0 && s.life <= 4 ? 2 : boss_near ? 1 : 0), mom.mic);
        // the light glides between colours (~80 ms); flashes, alarms and blinking cut straight through
        const bool hard_cut = now < flash_until || low || now < dead_until || paused || ao.light_mix >= 0.95f ||
                              mom.escaping || now < act.terror_until;
        const float k = hard_cut ? 1.0f : 1.0f - std::exp(-dt / 0.08f);
        for (int i = 0; i < 3; i++) {
            light_now[i] += (c[i] - light_now[i]) * k;
        }

        Block b{};
        b[0] = 0x04 | 0x08;
        std::copy(r.begin(), r.end(), b.begin() + 10);
        std::copy(l.begin(), l.end(), b.begin() + 21);
        b[1] = 0x01 | 0x04 | 0x10;
        b[8] = mic;
        b[42] = 0;
        b[43] = static_cast<u8>(led_mask & 0x1F);
        for (int i = 0; i < 3; i++) {
            b[44 + i] = static_cast<u8>(std::clamp(light_now[i], 0.0f, 255.0f));
        }
        Common::DualSenseEffects::Publish(b);
        prev_buttons = buttons;
    }
    PublishOff(); // triggers free when the game stops
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
