// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "input_common/helpers/dualsense_haptics.h"

#ifdef _WIN32

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <mutex>
#include <numbers>
#include <thread>

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/thread.h"

namespace InputCommon {
namespace {

// PKEY_Device_FriendlyName, defined here so no property-key library is needed
constexpr PROPERTYKEY FriendlyNameKey{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr GUID FloatSubtype{
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

struct Band {
    float amplitude{}, frequency{};
};
using Clock = std::chrono::steady_clock;
struct Side {
    Band low{0.0f, 160.0f}, high{0.0f, 320.0f};
    Clock::time_point updated{};
    // RenHDrum's motor layer (src/dualsense/motor.rs), from the game's raw amplitudes: a heavy hit glides the low
    // band toward 75 Hz (held while it stays above half its peak, at most 150 ms, then 60 ms back)
    float raw_low{}, raw_hist[2]{};
    bool heavy{};
    float heavy_peak{};
    Clock::time_point heavy_start{}, heavy_release{};
};

struct State {
    std::mutex mutex;
    std::array<Side, 2> target;
    Clock::time_point boom_since{}; // both sides' low band at 0.7+ (and 200 Hz or less) since then: an explosion
    std::atomic<bool> started{false}, running{false};
    std::atomic<u32> sets{0};          // (the 5 s activity line)
    std::atomic<float> set_peak{0.0f};
    float gain = 1.0f;
};

State& GetState() {
    static State state;
    return state;
}

bool Enabled() {
    const char* const value = std::getenv("FRITCHII_DUALSENSE_HAPTICS");
    return !(value && value[0] == '0');
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// The first active render device named like a DualSense with 4 or more float channels
IMMDevice* FindDevice(IMMDeviceEnumerator* enumerator) {
    IMMDeviceCollection* devices = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) {
        return nullptr;
    }
    UINT count = 0;
    devices->GetCount(&count);
    IMMDevice* found = nullptr;
    for (UINT i = 0; i < count && !found; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(devices->Item(i, &device))) {
            continue;
        }
        IPropertyStore* props = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        bool named = false;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) &&
            SUCCEEDED(props->GetValue(FriendlyNameKey, &name)) && name.vt == VT_LPWSTR) {
            named = std::wcsstr(name.pwszVal, L"DualSense") != nullptr ||
                    std::wcsstr(name.pwszVal, L"Wireless Controller") != nullptr;
        }
        PropVariantClear(&name);
        SafeRelease(props);
        if (named) {
            IAudioClient* client = nullptr;
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(&client))) &&
                SUCCEEDED(client->GetMixFormat(&format)) && format->nChannels >= 4) {
                found = device;
                device = nullptr;
            }
            if (format) {
                CoTaskMemFree(format);
            }
            SafeRelease(client);
        }
        SafeRelease(device);
    }
    SafeRelease(devices);
    return found;
}

bool IsFloat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, FloatSubtype) && format->wBitsPerSample == 32;
    }
    return false;
}

// Plays until the device goes away; returns to look for it again
void Play(IMMDevice* device) {
    State& state = GetState();
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* format = nullptr;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto cleanup = [&] {
        if (client) {
            client->Stop();
        }
        SafeRelease(render);
        SafeRelease(client);
        if (format) {
            CoTaskMemFree(format);
        }
        if (event) {
            CloseHandle(event);
        }
        state.running = false;
    };
    if (!event ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&client))) ||
        FAILED(client->GetMixFormat(&format)) || !IsFloat(format)) {
        LOG_WARNING(Input, "DualSense haptics: the controller's sound device has no float format");
        cleanup();
        return;
    }
    constexpr REFERENCE_TIME buffer_100ns = 200000; // 20 ms
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  buffer_100ns, 0, format, nullptr)) ||
        FAILED(client->SetEventHandle(event)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                  reinterpret_cast<void**>(&render)))) {
        LOG_WARNING(Input, "DualSense haptics: the controller's sound device did not open");
        cleanup();
        return;
    }
    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    const u32 channels = format->nChannels;
    const float rate = static_cast<float>(format->nSamplesPerSec);
    LOG_INFO(Input, "DualSense haptics: playing the game's HD rumble on the voice coils ({} Hz, {} "
                    "channels, {} frames buffered)",
             format->nSamplesPerSec, channels, buffer_frames);

    struct Voice {
        double phase_low{}, phase_high{};
        float amp_low{}, amp_high{};
        float freq_low{160.0f};           // glides (15 ms) toward the heavy hit's lowered frequency
        float hp_x{}, hp_y{}, lp_y{};    // the explosion's 50-130 Hz noise (one-pole high and low pass)
        u32 noise{0x9E3779B9u};
    };
    std::array<Voice, 2> voices{};
    const float ramp = 1.0f / (0.004f * rate); // amplitude follows the target over ~4 ms (RenHDrum's smoothing)
    constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
    constexpr auto stale = std::chrono::milliseconds(250);

    client->Start();
    state.running = true;
    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    u64 frames_written = 0;
    float out_peak = 0.0f;
    for (;;) {
        if (WaitForSingleObject(event, 200) != WAIT_OBJECT_0) {
            continue;
        }
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) {
            break; // (the device went away)
        }
        const UINT32 frames = buffer_frames - padding;
        if (frames == 0) {
            continue;
        }
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) {
            break;
        }
        std::array<Side, 2> target;
        float gain;
        Clock::time_point boom_since;
        {
            std::scoped_lock lock{state.mutex};
            target = state.target;
            gain = state.gain;
            boom_since = state.boom_since;
        }
        const auto now = std::chrono::steady_clock::now();
        // Heavy hit weight per side, and the explosion's (both sides held 200 ms, 50 ms in)
        std::array<float, 2> heavy_w{};
        for (int s = 0; s < 2; ++s) {
            const Side& t = target[s];
            if (t.heavy) {
                heavy_w[s] = 1.0f;
            } else if (t.heavy_release != Clock::time_point{}) {
                const float dt = std::chrono::duration<float, std::milli>(now - t.heavy_release).count();
                heavy_w[s] = std::clamp(1.0f - dt / 60.0f, 0.0f, 1.0f);
            }
        }
        float boom_w = 0.0f;
        if (boom_since != Clock::time_point{}) {
            const float held = std::chrono::duration<float, std::milli>(now - boom_since).count();
            boom_w = std::clamp((held - 200.0f) / 50.0f, 0.0f, 1.0f);
        }
        const float glide = 1.0f - std::exp(-1.0f / (0.015f * rate));
        const float hp_a = 1.0f / (1.0f + two_pi * 50.0f / rate), lp_a = 1.0f - std::exp(-two_pi * 130.0f / rate);
        for (auto& side : target) {
            if (now - side.updated > stale) {
                side.low.amplitude = side.high.amplitude = 0.0f;
            }
        }
        float* const out = reinterpret_cast<float*>(data);
        for (UINT32 f = 0; f < frames; ++f) {
            float* const frame = out + static_cast<size_t>(f) * channels;
            std::fill(frame, frame + channels, 0.0f);
            for (int s = 0; s < 2; ++s) {
                Voice& v = voices[s];
                const Side& t = target[s];
                const auto approach = [ramp](float current, float want) {
                    return current < want ? (std::min)(current + ramp, want)
                                          : (std::max)(current - ramp, want);
                };
                v.amp_low = approach(v.amp_low, t.low.amplitude);
                v.amp_high = approach(v.amp_high, t.high.amplitude);
                const float want_low = t.low.frequency - heavy_w[s] * (std::max)(t.low.frequency - 75.0f, 0.0f);
                v.freq_low += (want_low - v.freq_low) * glide;
                v.phase_low += v.freq_low / rate;
                v.phase_high += t.high.frequency / rate;
                v.phase_low -= std::floor(v.phase_low);
                v.phase_high -= std::floor(v.phase_high);
                // As RenHDrum: the two bands scaled down together when their amplitudes add past 1, then a 0.8 ceiling
                const float sum = v.amp_low + v.amp_high;
                const float norm = sum > 1.0f ? 1.0f / sum : 1.0f;
                float low_sin = std::sin(two_pi * static_cast<float>(v.phase_low));
                float noise = 0.0f;
                if (boom_w > 0.0f) {   // RenHDrum's explosion: the low band saturated, and 50-130 Hz noise
                    low_sin = (1.0f - boom_w) * low_sin + boom_w * std::tanh(low_sin * 1.25f);
                    v.noise = v.noise * 1664525u + 1013904223u;
                    const float white = static_cast<float>(v.noise >> 8) / 8388608.0f - 1.0f;
                    v.hp_y = hp_a * (v.hp_y + white - v.hp_x);
                    v.hp_x = white;
                    v.lp_y += (v.hp_y - v.lp_y) * lp_a;
                    noise = 0.15f * t.raw_low * boom_w * v.lp_y * 4.0f;
                }
                const float sample =
                    norm * (v.amp_low * low_sin +
                            v.amp_high * std::sin(two_pi * static_cast<float>(v.phase_high))) + noise;
                frame[2 + s] = std::clamp(std::clamp(sample, -1.0f, 1.0f) * 0.8f * gain, -1.0f, 1.0f);
                out_peak = (std::max)(out_peak, std::abs(frame[2 + s]));
            }
        }
        render->ReleaseBuffer(frames, 0);
        frames_written += frames;
        if (now >= report_at) {   // what came in and what went out, so a silent controller can be told apart
            const u32 sets = state.sets.exchange(0);
            const float set_peak = state.set_peak.exchange(0.0f);
            if (sets || out_peak > 0.0f)
                LOG_INFO(Input, "DualSense haptics, 5 s: {} vibration updates (strongest amplitude {:.2f}), {} "
                                "frames written (largest sample {:.2f} on channels 3-4)",
                         sets, set_peak, frames_written, out_peak);
            frames_written = 0;
            out_peak = 0.0f;
            report_at = now + std::chrono::seconds(5);
        }
    }
    LOG_INFO(Input, "DualSense haptics: the controller's sound device went away");
    cleanup();
}

void ThreadMain() {
    Common::SetCurrentThreadName("DualSenseHaptics");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return;
    }
    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator)))) {
        CoUninitialize();
        return;
    }
    bool told = false;
    for (;;) {
        IMMDevice* device = FindDevice(enumerator);
        if (device) {
            told = false;
            Play(device);
            SafeRelease(device);
        } else if (!told) {
            LOG_INFO(Input, "DualSense haptics: no DualSense sound device (the controller needs a USB "
                            "cable); motors are used");
            told = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

} // namespace

DualSenseHaptics& DualSenseHaptics::Instance() {
    static DualSenseHaptics instance;
    return instance;
}

bool DualSenseHaptics::Start() {
    State& state = GetState();
    if (!Enabled()) {
        return false;
    }
    if (!state.started.exchange(true)) {
        if (const char* const g = std::getenv("FRITCHII_DUALSENSE_HAPTICS_GAIN")) {
            state.gain = std::clamp(static_cast<float>(std::atof(g)), 0.0f, 4.0f);
        }
        std::thread{ThreadMain}.detach();
    }
    return true;
}

bool DualSenseHaptics::Running() const {
    return GetState().running.load(std::memory_order_relaxed);
}

void DualSenseHaptics::Set(int side, float low_amplitude, float low_frequency, float high_amplitude,
                           float high_frequency) {
    State& state = GetState();
    const auto now = Clock::now();
    state.sets.fetch_add(1, std::memory_order_relaxed);
    const float peak = (std::max)(low_amplitude, high_amplitude);
    if (peak > state.set_peak.load(std::memory_order_relaxed)) {
        state.set_peak.store(peak, std::memory_order_relaxed);
    }
    std::scoped_lock lock{state.mutex};
    for (int s = 0; s < 2; ++s) {
        if (side == 0 || side == 1) {
            if (s != side) {
                continue;
            }
        }
        // Switch 2 games ask for small amplitudes (OoT: 0.04-0.06 bumping into a wall) that the console's actuators make
        // felt. RenHDrum's Switch 2 law (src/nx2.rs mode 2, MIT OR Apache-2.0): min(3.5 * a^0.75, 1), 0 at or below 0.001
        const auto curve = [](float a) {
            a = std::clamp(a, 0.0f, 1.0f);
            return a <= 0.001f ? 0.0f : (std::min)(3.5f * std::pow(a, 0.75f), 1.0f);
        };
        Side& t = state.target[s];
        t.low = {curve(low_amplitude), std::clamp(low_frequency, 10.0f, 2000.0f)};
        t.high = {curve(high_amplitude), std::clamp(high_frequency, 10.0f, 2000.0f)};
        // RenHDrum's heavy hit (motor.rs): the low band jumps (0.12+ over two updates) to 0.30+, mostly low, at 220 Hz or less
        const float al = std::clamp(low_amplitude, 0.0f, 1.0f), ah = std::clamp(high_amplitude, 0.0f, 1.0f);
        const float prev2 = t.raw_hist[1];
        t.raw_hist[1] = t.raw_hist[0];
        t.raw_hist[0] = al;
        t.raw_low = al;
        if (!t.heavy && al >= 0.30f && al - prev2 >= 0.12f && ah / (al + ah) < 0.40f && low_frequency <= 220.0f) {
            t.heavy = true;
            t.heavy_peak = al;
            t.heavy_start = now;
        } else if (t.heavy) {
            t.heavy_peak = (std::max)(t.heavy_peak, al);
            if (al < 0.5f * t.heavy_peak || now - t.heavy_start > std::chrono::milliseconds(150)) {
                t.heavy = false;
                t.heavy_release = now;
            }
        }
        t.updated = now;
    }
    // RenHDrum's explosion: both sides' low band at 0.7+ and 200 Hz or less (held 200 ms in the synthesis)
    const auto booming = [](const Side& t) { return t.raw_low >= 0.70f && t.low.frequency <= 200.0f; };
    if (booming(state.target[0]) && booming(state.target[1])) {
        if (state.boom_since == Clock::time_point{}) {
            state.boom_since = now;
        }
    } else {
        state.boom_since = {};
    }
}

} // namespace InputCommon

#else

namespace InputCommon {
DualSenseHaptics& DualSenseHaptics::Instance() {
    static DualSenseHaptics instance;
    return instance;
}
bool DualSenseHaptics::Start() {
    return false;
}
bool DualSenseHaptics::Running() const {
    return false;
}
void DualSenseHaptics::Set(int, float, float, float, float) {}
} // namespace InputCommon

#endif
