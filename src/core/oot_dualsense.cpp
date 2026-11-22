// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

// A port of Pulse of Time's controller output (github.com/Balemuni/pulse-of-time, GPL-3.0-or-later, commit
// 6d2cf17): Oot.cs (the game's state), GameData.cs (save fields), Actions.cs (Link's action verbs) and the
// trigger and light parts of Daemon.cs, with their numbers. Not ported yet: Combat.cs (enemies, bosses, the
// shield's brace), Scene.cs, Events.cs and Moments.cs (places, flashes, scripted moments), Quad.cs pulses and
// what needs the pad's own input (R2's pull, buttons: the bow's draw on the LEDs, dry fire, the spin
// charge, ocarina notes, struggling), and the haptic textures (Fritchii plays the game's own HD rumble).

#include "core/oot_dualsense.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
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

    bool HasVerb(const std::string& v) const {
        return now_set.contains(v);
    }

    Out Update(long long now, const State& s, const std::array<int, 5>& verbs,
               const std::vector<std::string>& names, const std::string& anim, float anim_frame,
               const std::string& armor, const std::string& map) {
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
        }
        if (grabbed && entered({"ReleaseRestraint", "Unstick"})) {
            grabbed_until = 0;
            grabbed = false;
        }
        if (terror && !was_terror) {
            flash_color = {150, 150, 160};
            flash_until = now + 250;
        }
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
        if (fly_now) {
            flying_state = true;
        } else if (flying_state && pos_vel < 3.0f) {
            flying_state = false;
        }
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
            light = Rgb{180, 140, 40}; // (a note's colour flash needs the pad's buttons: not ported yet)
            mix = 0.5f;
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
};

// ---------- The loop (Daemon.cs: life, aiming, riding, triggers, lights) ----------
const u32 HPause = Hash("Sequence_IsPause"), HPauseReq = Hash("Sequence_GamePauseRequestCount");
const u32 HPrePause = Hash("Sequence_IsPreGamePause"), HChild = Hash("Sequence_IsChildhood");
const u32 HLoading = Hash("Sequence_IsLoadingBanc"), HMap = Hash("Sequence_CurrentBanc");
const u32 HArrows = Hash("PouchData.Item_Arrow_Normal.StockNum"), HSeeds = Hash("PouchData.Item_DekuSeed.StockNum");
const u32 HMaxLife = Hash("PlayerStatus.MaxLife");
const u32 HShield = Hash("PlayerStatus.EquipShield"), HBoot = Hash("PlayerStatus.EquipBoot");
const u32 HArmor = Hash("PlayerStatus.EquipArmor");
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
    // tuning.json's defaults: L2 at 0.75 of its resistance, R2 full
    constexpr float TuneL2 = 0.75f, TuneR2 = 1.0f;
    bool checked = false;
    std::vector<std::string> names;
    Actions act;
    std::string shield, boot, armor, map;
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
        if (!s.in_game || gd.GetBool(HLoading).value_or(false)) {
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
            next_strings = now + 500;
        }
        const bool iron = boot.find("HeavyBoot") != std::string::npos;
        const bool hover = boot.find("HoverBoot") != std::string::npos;
        const auto flash_to = [&](float r, float g, float b, long long ms) {
            flash = {r, g, b};
            flash_ms = ms;
            flash_until = now + ms;
        };
        if (last_max_life > 0 && max_life > last_max_life) {
            flash_to(255, 60, 120, 1600); // a heart container
            celebrate_until = now + 1500;
        }
        last_max_life = max_life;

        const auto ao = act.Update(now, s, verbs, names, anim, anim_frame, armor, map);
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

        // life: a hit jolts the triggers, death fades the light, a fairy brings it back
        if (last_life >= 0 && s.life < last_life) {
            jolt = now + 200;
            if (s.life <= 0) {
                dead_until = now + 2500;
                dead_at = now;
            } else {
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
        const bool low = s.life > 0 && s.life * 4 <= s.life_max;
        const bool critical = s.life > 0 && s.life <= 4;

        // Epona: on her and fast for 300 ms
        const bool fast = act.HasVerb("Ride") && s.SpeedH() > 2.5f;
        ride_since = fast ? (ride_since < 0 ? now : ride_since) : -1;
        const bool riding = ride_since >= 0 && now - ride_since > 300;

        // ---- triggers ----
        const int heavy = iron ? 1 : hover ? -2 : 0;
        Effect r = Off(), l = Off();
        if (now < dead_until) {
            r = l = Off();
        } else if (paused) {
            r = l = Weapon(3, 5, 3); // menu: light button detents
        } else {
            // firm by default: a short, hard click point on R2, the full weight of the shield on L2
            if (aiming && ammo > 0) {
                r = child ? Bow(0, 6, std::clamp(7 + heavy, 1, 8), 8) : Bow(0, 7, 8, 8);
            } else if (aiming) {
                r = Feedback(1, 2); // (the dry pull's jam needs R2's position: not ported yet)
            } else {
                r = Weapon(2, 4, std::clamp(8 + heavy, 1, 8));
            }
            l = shield.find("Mirror") != std::string::npos || shield.find("Hylian") != std::string::npos
                    ? Feedback(0, std::clamp(8 + heavy, 1, 8))
                : shield.find("Deku") != std::string::npos ? Slope(0, 9, std::clamp(6 + heavy, 1, 8), 8)
                                                           : Feedback(1, std::clamp(5 + heavy, 1, 8));
            if (ao.r) r = *ao.r;
            if (ao.l) l = *ao.l;
            if (riding) r = l = Feedback(1, 4); // the reins
            if (ao.free_triggers) r = l = Off();
            if (now < celebrate_until) r = l = Machine(1, 9, 6, 2, 30, 2);
            r = Scale(r, TuneR2);
            l = Scale(l, TuneL2);
            if (now < r_tick) r = Vibration(3, 6, 60);
            if (now < l_kick) l = Vibration(0, 8, 35);
            if (now < jolt) r = l = Vibration(0, 8, 45);
        }

        // ---- lights ----
        Rgb c = armor.find("Goron") != std::string::npos  ? Rgb{200, 25, 10}
                : armor.find("Zora") != std::string::npos ? Rgb{15, 70, 230}
                                                          : Rgb{0, 170, 40};
        if (!world.indoor && world.seconds >= 0) c = TimeOfDay(c, world.seconds);
        if (act.raining && !world.indoor) c = Mix(c, {90, 100, 130}, 0.4f);
        if (ao.light) c = Mix(c, *ao.light, ao.light_mix);
        if (low) {
            const long long period = s.life <= 4 ? 700 : 1000;
            c = Mix(c, {(now % period) < 260 ? 255.0f : 60.0f, 0, 0}, 0.85f);
        }
        if (now < flash_until) c = Mix(c, flash, (flash_until - now) / static_cast<float>(flash_ms));
        if (now < dead_until) {
            const float a = (dead_until - now) / 2500.0f;
            c = {180 * a, 0, 0};
        }
        if (paused) c = {c[0] * 0.25f, c[1] * 0.25f, c[2] * 0.25f};

        // player LEDs, most urgent first: being held (presses), breath, else Link's life (the bow's draw and
        // spin charge need the pad's input, the boss's and target's life need Combat.cs: not ported yet)
        const bool grabbed_now = now < act.grabbed_until;
        const int src = grabbed_now ? 5 : act.air_left >= 0 ? 7 : 10;
        const float bar = src == 5   ? (std::min)(1.0f, act.grab_presses / 8.0f)
                          : src == 7 ? act.air_left
                                     : (s.life_max > 0 ? (std::max)(0, s.life) / static_cast<float>(s.life_max) : 0.0f);
        int leds = static_cast<int>(std::ceil(5 * std::clamp(bar, 0.0f, 1.0f)));
        if (act.air_left >= 0 && act.air_left < 0.25f && now % 400 < 200) leds = 0; // last breath blinks
        if (critical && now % 650 >= 260 && !aiming) leds = 0;                     // the last heart blinks with the beat
        int led_mask = (1 << leds) - 1;
        // a new meter announces itself: one light runs across before it shows its value
        if (src != led_src) {
            if (led_src >= 0 && src != 10) led_sweep_until = now + 240;
            led_src = src;
        }
        if (now < led_sweep_until) {
            led_mask = 1 << static_cast<int>(std::clamp<long long>((now - (led_sweep_until - 240)) / 48, 0, 4));
        }
        const u8 mic = s.life > 0 && s.life <= 4 ? 2 : 0;
        // the light glides between colours (~80 ms); flashes, alarms and blinking cut straight through
        const bool hard_cut = now < flash_until || low || now < dead_until || paused || ao.light_mix >= 0.95f ||
                              now < act.terror_until;
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
