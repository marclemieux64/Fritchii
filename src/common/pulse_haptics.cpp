// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pulse of Time's HapticFx.cs, HapticLoops.cs and the effect part of Haptics.cs (commit 6d2cf17,
// GPL-3.0-or-later). The effect and texture bodies are mechanical translations of the C# (fx / loops below).

#include "common/pulse_haptics.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <string>
#include <mutex>
#include <numbers>
#include <random>
#include <vector>

#include "common/logging.h"

namespace Common::PulseHaptics {
namespace {

constexpr double Rate = 48000;
constexpr double Pi = std::numbers::pi;
inline double Clamp(double v, double lo, double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
inline double Max(double a, double b) {
    return a > b ? a : b;
}
inline double Min(double a, double b) {
    return a < b ? a : b;
}
long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// RBJ cookbook biquad
class Biquad {
public:
    static Biquad LowPass(double f, double q = 0.7071) {
        const auto [c, a] = W(f, q);
        return {(1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a};
    }
    static Biquad HighPass(double f, double q = 0.7071) {
        const auto [c, a] = W(f, q);
        return {(1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a};
    }
    static Biquad BandPass(double f, double q) {
        const auto [c, a] = W(f, q);
        return {a, 0, -a, 1 + a, -2 * c, 1 - a};
    }
    Biquad(double b0_, double b1_, double b2_, double a0, double a1_, double a2_)
        : b0{static_cast<float>(b0_ / a0)}, b1{static_cast<float>(b1_ / a0)}, b2{static_cast<float>(b2_ / a0)},
          a1{static_cast<float>(a1_ / a0)}, a2{static_cast<float>(a2_ / a0)} {}
    float Run(float x) {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        if (std::abs(z1) < 1e-15f) z1 = 0;
        if (std::abs(z2) < 1e-15f) z2 = 0;
        return y;
    }

private:
    static std::pair<double, double> W(double f, double q) {
        const double w = 2 * Pi * f / Rate;
        return {std::cos(w), std::sin(w) / (2 * q)};
    }
    float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
};

// tuning.json's defaults (Tuning.cs), per effect and per texture
constexpr float FxGain[] = {1.0f, 1.15f, 1.3f, 1.25f, 0.85f, 0.45f, 0.95f, 1.4f, 1.25f, 1.25f, 1.6f, 1.9f, 1.1f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.25f, 1.35f, 1.6f, 1.3f, 1.3f, 1.3f, 1.3f, 1.2f, 1.25f, 1.3f, 1.1f, 1.0f, 1.0f, 1.35f, 1.45f, 1.6f, 1.6f, 1.35f, 1.35f, 1.0f, 1.3f, 1.35f, 1.0f, 1.7f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.15f, 1.0f, 1.35f, 0.7f, 1.15f, 1.25f, 1.25f, 1.35f, 1.5f, 1.35f, 1.0f, 1.35f, 1.2f, 1.25f, 1.35f, 1.0f, 1.25f, 1.25f, 1.0f, 1.15f, 1.0f, 1.0f, 1.35f, 1.6f, 1.35f, 1.6f, 1.35f, 1.45f, 1.6f, 1.45f, 1.5f, 1.4f, 1.5f, 1.2f, 1.4f, 1.9f, 1.45f, 1.45f, 2.1f, 1.7f, 1.8f, 1.7f, 1.9f, 1.25f, 1.25f, 1.45f, 1.45f, 1.2f, 1.35f, 1.25f, 1.6f, 1.15f, 1.45f, 1.35f, 1.15f, 1.6f, 1.25f, 1.3f, 1.3f, 1.3f, 1.2f, 1.4f, 1.2f, 1.4f, 1.8f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr float LoopGain[] = {1.15f, 1.0f, 1.45f, 0.75f, 1.0f, 0.9f, 1.2f, 1.1f, 1.5f, 1.1f, 1.1f, 1.0f, 1.35f, 1.3f, 1.45f, 1.25f, 1.1f, 1.2f, 1.2f, 1.3f, 1.2f, 1.2f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
constexpr float CreakGain = 1.25f, ChargeGain = 1.0f;

Params g_params;

float BodyRate() { return g_params.body_rate.load(std::memory_order_relaxed); }
float BodyWeight() { return g_params.body_weight.load(std::memory_order_relaxed); }

class HVoice {
public:
    Fx Kind;
    float S;
    int N;
    float Pan;
    float G = 1;

    HVoice(Fx k, float s, int n, float pan, int seed) : Kind{k}, S{s}, N{n}, Pan{pan}, _rng{static_cast<unsigned>(seed)} {
        // every repetition a little different (+-5 % speed and pitch, +-6 % strength); the musical ones stay exact
        const bool exact = k == Fx::NoteOn || k == Fx::Sunrise || k == Fx::Nightfall || k == Fx::Chime || k == Fx::Heartbeat ||
                           k == Fx::Hello || k == Fx::TimerTick || k == Fx::TimeShift || k == Fx::Detent || k == Fx::AimTick;
        _speed = exact ? 1 : 0.95 + 0.1 * NextDouble();
        if (!exact) S *= static_cast<float>(0.94 + 0.12 * NextDouble());
        // the body: the child is lighter and quicker, the adult heavier
        if (IsBody(k)) {
            _speed *= BodyRate();
            S *= BodyWeight();
        }
        _mat = static_cast<Mat>(g_params.material.load(std::memory_order_relaxed));
    }

    /// (left, right); done when past its length (the speaker part is not played)
    void Next(float& out_l, float& out_r, bool& done) {
        const double t = T;
        T += _speed / Rate;
        float l = 0, r = 0, spk = 0;
        float pan = Pan;
        switch (Kind)
        {
            case Fx::Hello:
                l = Hann(t, 0.03) * Sin(t, 180) * 0.6f; r = Hann(t - 0.12, 0.03) * Sin(t, 180) * 0.6f;
                done = t > 0.2; break;

            // ---- Link ----
            case Fx::Hit: // thud + crack
                l = r = S * (Env(t, 0.003, 0.09) * Sin(t, 48) * 0.95f + Env(t, 0.001, 0.018) * Sin(t, 170) * 0.6f);
                done = t > 0.45; break;
            case Fx::Heal:
                for (int i = 0; i < 3; i++) { double u = t - i * 0.075; l += Hann(u, 0.045) * Sin(u, 250 + 45 * i) * 0.38f; }
                r = l; done = t > 0.3; break;
            case Fx::Heartbeat:
                l = r = S * (Hann(t, 0.075) * Sin(t, 55) + Hann(t - 0.17, 0.065) * Sin(t, 47) * 0.7f) * 0.9f;
                done = t > 0.25; break;
            case Fx::Land: // weight + grit
                l = r = S * (Env(t, 0.002, 0.06) * Sin(t, 60) + Env(t, 0.001, 0.012) * Sin(t, 210) * 0.4f + Env(t, 0.001, 0.03) * Grain() * 0.25f + MatTex(t, 1));
                done = t > 0.3; break;
            case Fx::Jump: // push off the ground, then lift
                l = r = Env(t, 0.002, 0.035) * Sin(t, 70) * 0.65f + Hann(t, 0.1) * Chirp(t, 110, 210, 0.1) * 0.22f;
                done = t > 0.12; break;
            case Fx::Roll: // tumble: three thumps going round the hands, ground scraping under them
            {
                for (int i = 0; i < 3; i++)
                {
                    float th = Env(t - i * 0.15, 0.003, 0.05) * Sin(t, 55) * 0.75f;
                    if (i == 1) r += th; else l += th;
                }
                float scrape = Hann(t, 0.48) * Grain() * 0.18f + MatTex(t - 0.15, 0.8f);
                l += scrape; r += scrape; done = t > 0.5; break;
            }
            case Fx::Swing: // sword cuts the air across the hands (N 1: left to right, 2: right to left)
            {
                double p = Clamp(t / 0.15, 0, 1);
                float g = Hann(t, 0.15) * Grain() * 0.6f * S;
                float toR = (float)(N == 2 ? 1 - p : p);
                l = g * (1 - toR); r = g * toR; done = t > 0.16; break;
            }
            case Fx::SpinRelease: // the spin goes once round both hands, ending in a crack
            {
                float g = Hann(t, 0.5) * Grain() * 0.75f + Hann(t, 0.5) * Sin(t, 85) * 0.25f;
                float a = (float)std::sin(2 * Pi * t / 0.5);
                l = g * (0.5f - 0.5f * a); r = g * (0.5f + 0.5f * a);
                float crack = Env(t - 0.48, 0.001, 0.03) * Sin(t, 190) * 0.7f;
                l += crack; r += crack; done = t > 0.6; break;
            }
            case Fx::ShieldBlock: // metal ring + body shove, on the shield hand
                l = r = Env(t, 0.0005, 0.08) * (Sin(t, 205) + 0.4f * Sin(t, 410)) * 0.8f + Env(t, 0.002, 0.05) * Sin(t, 55) * 0.8f;
                done = t > 0.3; break;
            case Fx::LockOn: // two quick ticks
                l = r = (Hann(t, 0.008) + Hann(t - 0.04, 0.008)) * Sin(t, 270) * 0.5f; done = t > 0.06; break;
            case Fx::Grip: // a hand closes on a ledge or vine
                l = r = Hann(t, 0.03) * Sin(t, 150) * 0.45f + Env(t, 0.001, 0.01) * Grain() * 0.15f; done = t > 0.05; break;
            case Fx::Spur: // Epona kicks off: thump and rush
                l = r = Env(t, 0.002, 0.06) * Sin(t, 55) * 0.9f + Hann(t, 0.35) * Grain() * 0.3f; done = t > 0.36; break;
            case Fx::Revive: // a fairy: sparkles raining down, warmth swelling up
            {
                float sparkle = 0;
                if (Click(t, 40 * (1.2 - t))) _burst = 1;
                _burst *= 0.993f;
                sparkle = _burst * Sin(t, 300 + 60 * Noise()) * 0.5f;
                l = r = sparkle + Hann(t, 1.2) * Sin(t, 90) * 0.3f;
                spk = _burst * Sin(t, 2600 + 900 * _burst) * 0.4f;
                done = t > 1.2; break;
            }
            case Fx::Death:
                l = r = Env(t, 0.02, 0.6) * Sin(t, 40) * 0.9f * (0.7f + 0.3f * Sin(t, 6));
                done = t > 2.0; break;
            case Fx::DeathFire:
                if (Click(t, 250)) _burst = 1;
                _burst *= 0.995f;
                l = r = Env(t, 0.01, 0.8) * (_burst * Sin(t, 230) * 0.8f + Sin(t, 75) * 0.35f);
                done = t > 2.0; break;
            case Fx::DeathDrown:
            {
                double ph = std::fmod(t * 5, 1.0);
                float bub = Hann(ph, 0.25) * Sin(t, 90 + 120 * ph) * Env(t, 0.05, 1.0);
                if ((int)(t * 5) % 2 == 0) l = bub; else r = bub;
                done = t > 2.2; break;
            }
            case Fx::DeathShock:
                l = r = ((int)(t * 12) % 2 == 0 ? 1.0f : 0.2f) * Sin(t, 290) * Env(t, 0.005, 0.5) * 0.8f;
                done = t > 1.5; break;

            // ---- weapons ----
            case Fx::BowRelease: // twang with falling pitch, mostly the right hand; "thwip" from the speaker
            {
                double f = 160 + 70 * std::exp(-t / 0.05);
                float v = Env(t, 0.001, 0.07) * (float)std::sin(2 * Pi * f * t) * S;
                r = v * 0.95f; l = v * 0.35f;
                spk = Env(t, 0.0005, 0.02) * Chirp(t, 1800, 700, 0.06) * 0.35f;
                done = t > 0.3; break;
            }
            case Fx::SlingRelease:
            {
                float v = Env(t, 0.0005, 0.025) * Sin(t, 260) * 0.8f + Env(t - 0.02, 0.005, 0.06) * Sin(t, 110) * 0.35f;
                r = v * S; l = v * 0.25f * S; spk = Env(t, 0.0005, 0.015) * Sin(t, 1400) * 0.25f; done = t > 0.2; break;
            }
            case Fx::DryFire:
                r = Hann(t, 0.012) * Sin(t, 320) * 0.5f; l = r * 0.2f; done = t > 0.03; break;

            // ---- enemies ----
            case Fx::EnemyHit: // the blade lands: crisp contact + body, from the enemy's side; N 2 = heavy / boss
                l = r = S * (Env(t, 0.0005, 0.012) * Sin(t, 185) * 0.9f + Env(t, 0.002, N == 2 ? 0.12 : 0.06) * Sin(t, N == 2 ? 40 : 52) * 0.8f);
                done = t > (N == 2 ? 0.4 : 0.25); break;
            case Fx::KillPop: // keese, bubbles, larvae: a small pop
                l = r = Env(t, 0.0005, 0.02) * Sin(t, 240) * 0.7f + Env(t, 0.001, 0.03) * Sin(t, 90) * 0.3f; done = t > 0.12; break;
            case Fx::KillCrunch: // plants, spiders: a burst of snapping fibres
                if (t < 0.12 && Click(t, 60)) _burst = 1;
                _burst *= 0.97f;
                l = r = _burst * Noise() * 0.6f + Env(t, 0.002, 0.05) * Sin(t, 65) * 0.5f; done = t > 0.25; break;
            case Fx::KillBones: // stalfos, stalchildren: bones rattle apart, then fall
            {
                if (Click(t, 70 * std::exp(-t / 0.2))) { _burst = 1; _nextPanR = NextDouble() < 0.5; }
                _burst *= 0.985f;
                float c = _burst * Sin(t, 300) * 0.55f;
                if (_nextPanR) r += c; else l += c;
                float drop = Env(t - 0.32, 0.002, 0.06) * Sin(t, 48) * 0.6f;
                l += drop; r += drop; done = t > 0.55; break;
            }
            case Fx::KillArmor: // iron knuckles, lizalfos, moblins: a clang, then the armour hits the floor
                l = r = Env(t, 0.0005, 0.15) * (Sin(t, 210) + 0.5f * Sin(t, 317)) * 0.6f + Env(t - 0.35, 0.003, 0.12) * Sin(t, 40) * 1.0f;
                done = t > 0.75; break;
            case Fx::KillSquish: // like-likes, jellies, octoroks: a wobble that collapses
                l = r = Hann(t, 0.4) * Sin(t, 70 + 30 * std::sin(2 * Pi * 12 * t)) * 0.7f; done = t > 0.4; break;
            case Fx::KillGhost: // poes: a shimmer that sinks and fades, drifting across
            {
                float v = Hann(t, 0.9) * Chirp(t, 320, 90, 0.9) * (0.5f + 0.5f * Sin(t, 9)) * 0.45f;
                float a = (float)(t / 0.9);
                l = v * (1 - a); r = v * a; spk = Hann(t, 0.9) * Chirp(t, 1600, 500, 0.9) * 0.08f; done = t > 0.9; break;
            }
            case Fx::KillHeavy: // dodongos, leevers, beamos: the body drops
                l = r = Env(t, 0.004, 0.2) * Sin(t, 36) * 1.1f + Env(t, 0.001, 0.03) * Sin(t, 160) * 0.5f; done = t > 0.6; break;
            case Fx::KillMidBoss: // shockwave and three aftershocks
                for (int i = 0; i < 4; i++) { double u = t - i * 0.28; l += Env(u, 0.003, 0.12) * Sin(u, 42 + 6 * i) * (1.1f - 0.25f * i); }
                l += Hann(t - 0.9, 0.5) * Chirp(t - 0.9, 120, 260, 0.5) * 0.3f;
                r = l; done = t > 1.5; break;
            case Fx::KillBoss: // the floor drops away, three slow beats, then the light
            {
                l = Env(t, 0.004, 0.35) * Sin(t, 32) * 1.2f + Env(t, 0.001, 0.05) * Sin(t, 150) * 0.6f;
                for (int i = 0; i < 3; i++) { double u = t - 0.7 - i * 0.45; l += Hann(u, 0.09) * Sin(u, 50) * (0.8f - 0.2f * i); }
                for (int i = 0; i < 3; i++) { double u = t - 2.2 - i * 0.12; l += Hann(u, 0.07) * Sin(u, 230 + 40 * i) * 0.5f; spk += Hann(u, 0.07) * Sin(u, 1800 + 400 * i) * 0.25f; }
                r = l; done = t > 2.7; break;
            }
            case Fx::Token: // gold skulltula token: three bright ticks
                for (int i = 0; i < 3; i++) { double u = t - i * 0.07; float c = Hann(u, 0.025) * Sin(u, 290 + 30 * i) * 0.5f; if (i % 2 == 0) r += c; else l += c; spk += Hann(u, 0.03) * Sin(u, 2400 + 300 * i) * 0.3f; }
                done = t > 0.25; break;

            // ---- pickups ----
            case Fx::Coin:
                for (int i = 0; i < N; i++)
                {
                    double u = t - i * 0.06;
                    float c = Hann(u, 0.022) * Sin(t, 300 + 25 * i) * 0.55f;
                    if (i % 2 == 0) r += c; else l += c;
                    spk += Hann(u, 0.035) * (Sin(u, 2600) + 0.5f * Sin(u, 3900)) * 0.22f;
                }
                done = t > N * 0.06 + 0.04; break;
            case Fx::CoinSpend:
                l = r = Hann(t, 0.03) * Sin(t, 220 - 800 * t) * 0.35f; done = t > 0.04; break;
            case Fx::AmmoGet:
                l = r = (Hann(t, 0.015) + Hann(t - 0.05, 0.015) * 0.7f) * Sin(t, 180) * 0.45f; done = t > 0.08; break;
            case Fx::MagicUse:
                l = r = Hann(t, 0.35) * Sin(t, 270 + 40 * std::sin(2 * Pi * 18 * t)) * 0.4f; done = t > 0.36; break;
            case Fx::MagicFill:
                l = r = Hann(t, 0.3) * Chirp(t, 150, 450, 0.3) * 0.35f; done = t > 0.31; break;
            case Fx::ItemGet: // the chest: a swell rising in both hands, then three bright pulses
            {
                double ramp = Clamp(t / 0.8, 0, 1);
                l = r = (float)(ramp * ramp) * Hann(t, 1.6) * Chirp(t, 50, 140, 0.8) * 0.5f;
                for (int i = 0; i < 3; i++)
                {
                    double u = t - 0.85 - i * 0.15;
                    l += Hann(u, 0.06) * Sin(u, 260) * 0.6f;
                    spk += Hann(u, 0.08) * Sin(u, 1600 * std::pow(1.26, i)) * 0.25f;
                }
                r = l; done = t > 1.4; break;
            }
            case Fx::SongGet: // five soft rising pulses
                for (int i = 0; i < 5; i++) { double u = t - i * 0.2; l += Hann(u, 0.14) * Sin(u, 180 + 25 * i) * 0.4f; }
                r = l; done = t > 1.2; break;
            case Fx::KeyGet:
            case Fx::BossKeyGet:
                for (int i = 0; i < 2; i++)
                {
                    double u = t - i * 0.12;
                    l += Env(u, 0.0005, 0.05) * (Sin(u, 300) + 0.5f * Sin(u, 470)) * 0.5f;
                    spk += Env(u, 0.0005, 0.06) * (Sin(u, 3200) + 0.6f * Sin(u, 4100)) * 0.2f;
                }
                if (Kind == Fx::BossKeyGet) l += Env(t - 0.3, 0.004, 0.25) * Sin(t, 38) * 1.0f + Hann(t - 0.3, 0.8) * Chirp(t - 0.3, 60, 120, 0.8) * 0.3f;
                r = l; done = t > (Kind == Fx::BossKeyGet ? 1.2 : 0.35); break;
            case Fx::MapGet:
                l = r = (Hann(t, 0.04) + Hann(t - 0.1, 0.04)) * _bp2.Run(Noise()) * 2.5f; done = t > 0.15; break;
            case Fx::MedalGet: // a stone or medallion: long deep swell with shimmer, two strong pulses
            {
                double ramp = Clamp(t / 1.5, 0, 1);
                l = (float)ramp * Hann(t, 2.6) * (Chirp(t, 40, 80, 1.5) * 0.6f + Grain() * 0.15f * (float)ramp);
                for (int i = 0; i < 2; i++) { double u = t - 1.7 - i * 0.35; l += Env(u, 0.003, 0.15) * Sin(u, 45) * 1.0f; }
                r = l; done = t > 2.5; break;
            }
            case Fx::HeartContainer: // three heartbeats, each stronger
                for (int i = 0; i < 3; i++) { double u = t - i * 0.45; l += (Hann(u, 0.08) * Sin(u, 55) + Hann(u - 0.17, 0.07) * Sin(u, 47) * 0.7f) * (0.5f + 0.25f * i); }
                r = l; done = t > 1.6; break;

            // ---- ambience ----
            case Fx::AmbLava: // a lava bubble somewhere below
                l = r = S * (Hann(t, 0.35) * Sin(t, 32) * 0.6f + Env(t - 0.3, 0.001, 0.02) * Sin(t, 120) * 0.3f); done = t > 0.4; break;
            case Fx::AmbDrip:
                l = r = S * (Env(t, 0.0005, 0.006) * Sin(t, 260) * 0.6f + Env(t, 0.002, 0.05) * Sin(t, 120) * 0.15f); done = t > 0.08; break;
            case Fx::AmbOrganic: // inside Jabu-Jabu: a slow wet pulse
                l = r = S * Hann(t, 0.3) * Sin(t, 40) * 0.6f; done = t > 0.3; break;
            case Fx::AmbEerie:
                l = r = S * Hann(t, 1.4) * (Sin(t, 28) * 0.6f + _bp2.Run(Noise()) * 0.6f); done = t > 1.4; break;
            case Fx::AmbThunder: // distant, rolling
                l = r = S * Hann(t, 1.8) * (float)(0.6 + 0.4 * std::sin(2 * Pi * 3 * t)) * (_bp2.Run(Noise()) * 1.5f + Sin(t, 34) * 0.4f); done = t > 1.8; break;
            case Fx::AmbSand: // a gust of sand grains
                if (Click(t, 35 * Hann(t, 0.8))) _burst = 1;
                _burst *= 0.96f;
                l = r = S * _burst * Grain() * 0.4f; done = t > 0.8; break;
            case Fx::AmbCrystal:
                l = r = S * Env(t, 0.0005, 0.06) * (Sin(t, 330) + 0.4f * Sin(t, 495)) * 0.4f; done = t > 0.15; break;

            // ---- actions and reactions ----
            case Fx::Splash: // breaking the water surface: a slap, then spray scattered over the hands
            {
                if (t < 0.25 && Click(t, 90 * (1 - t * 4))) { _burst = 1; _nextPanR = NextDouble() < 0.5; }
                _burst *= 0.97f;
                float spray = _burst * Grain() * 0.4f;
                float slap = Env(t, 0.001, 0.04) * Sin(t, 75) * 0.7f * S;
                l = slap + (_nextPanR ? spray * 0.4f : spray); r = slap + (_nextPanR ? spray : spray * 0.4f);
                done = t > 0.3; break;
            }
            case Fx::Slam: // a jump attack / plunge comes down: rising, then the impact through both hands
                l = r = Hann(t, 0.2) * Chirp(t, 90, 200, 0.2) * 0.25f + Env(t - 0.2, 0.002, 0.14) * Sin(t, 38) * 1.1f + Env(t - 0.2, 0.0005, 0.02) * Sin(t, 190) * 0.6f;
                done = t > 0.55; break;
            case Fx::Thrust: // a stab: one sharp forward poke
                l = r = Env(t, 0.0005, 0.03) * Sin(t, 210) * 0.6f + Env(t, 0.002, 0.05) * Sin(t, 85) * 0.4f; done = t > 0.12; break;
            case Fx::Flip: // backflip / side hop: lift, a sweep over, and the landing
            {
                float a = (float)Clamp(t / 0.35, 0, 1);
                float g = Hann(t, 0.35) * Grain() * 0.35f;
                l = g * (N == 2 ? a : 1 - a); r = g * (N == 2 ? 1 - a : a);
                float land = Env(t - 0.35, 0.002, 0.05) * Sin(t, 60) * 0.6f; l += land; r += land;
                done = t > 0.45; break;
            }
            case Fx::DoorOpen: // a heavy door grinds open, then settles
                l = r = Hann(t, 0.9) * _bp2.Run(Noise()) * 1.4f + Env(t - 0.85, 0.003, 0.12) * Sin(t, 42) * 0.8f; done = t > 1.1; break;
            case Fx::BootsClunk:
                l = r = Env(t, 0.001, 0.05) * (Sin(t, 120) + 0.5f * Sin(t, 260)) * 0.6f + Env(t - 0.12, 0.001, 0.05) * Sin(t, 50) * 0.6f; done = t > 0.25; break;
            case Fx::Lift: // picking something heavy up: the strain swells in both hands
                l = r = Hann(t, 0.45) * Sin(t, 48) * 0.6f * (float)Clamp(t / 0.3, 0, 1); done = t > 0.45; break;
            case Fx::Throw: // released: a whoosh away from you
            {
                float a = (float)Clamp(t / 0.2, 0, 1);
                float g = Hann(t, 0.2) * Grain() * 0.5f;
                l = g * (1 - a * 0.5f); r = g * (0.5f + a * 0.5f); done = t > 0.22; break;
            }
            case Fx::Put:
                l = r = Env(t, 0.002, 0.05) * Sin(t, 55) * 0.5f; done = t > 0.15; break;
            case Fx::Bottle: // swish into the bottle, cork pop
                l = r = Hann(t, 0.18) * Grain() * 0.35f + Env(t - 0.2, 0.0005, 0.02) * Sin(t, 280) * 0.5f;
                spk = Env(t - 0.2, 0.0005, 0.03) * Sin(t, 1500) * 0.3f; done = t > 0.3; break;
            case Fx::DinFire: // Din's Fire: a shockwave from the centre out to both edges, burning as it goes
            {
                float ring = Env(t, 0.002, 0.25) * Sin(t, 36) * 1.1f;
                if (Click(t, 120 * std::exp(-t / 0.4))) _burst = 1;
                _burst *= 0.99f;
                l = r = ring + _burst * Sin(t, 220) * 0.4f + Hann(t, 1.0) * Grain() * 0.2f; done = t > 1.0; break;
            }
            case Fx::Nayru: // Nayru's Love: a crystal forms around you, ringing
                l = r = Hann(t, 1.2) * (Sin(t, 300) * 0.25f + Sin(t, 450) * 0.12f) * (0.6f + 0.4f * Sin(t, 7));
                spk = Hann(t, 1.2) * (Sin(t, 2400) + 0.5f * Sin(t, 3600)) * 0.12f; done = t > 1.2; break;
            case Fx::Farore: // Farore's Wind / warp: a swirl going round and rising
            {
                float g = Hann(t, 1.0) * Chirp(t, 80, 300, 1.0) * 0.45f;
                float a = (float)(0.5 + 0.5 * std::sin(2 * Pi * 3 * t));
                l = g * a; r = g * (1 - a); done = t > 1.0; break;
            }
            case Fx::Perfect: // a perfect guard: a bright clean ring on the shield hand and a snap
                l = r = Env(t, 0.0003, 0.12) * (Sin(t, 260) + 0.6f * Sin(t, 520)) * 0.7f + Env(t, 0.001, 0.02) * Sin(t, 120) * 0.6f;
                spk = Env(t, 0.0003, 0.15) * Sin(t, 3200) * 0.25f; done = t > 0.35; break;
            case Fx::Repelled: // the blade bounces off something hard: ping and a kick back into the sword hand
                l = r = Env(t, 0.0003, 0.07) * (Sin(t, 310) + 0.5f * Sin(t, 470)) * 0.6f + Env(t, 0.002, 0.04) * Sin(t, 70) * 0.5f; done = t > 0.25; break;
            case Fx::GuardBreak: // the guard is smashed open
                l = r = Env(t, 0.002, 0.2) * Sin(t, 34) * 1.2f + Env(t, 0.0005, 0.05) * Grain() * 0.6f; done = t > 0.5; break;
            case Fx::Shatter: // ice breaks: a burst of crystal shards
                if (t < 0.2 && Click(t, 150)) { _burst = 1; _nextPanR = NextDouble() < 0.5; }
                _burst *= 0.985f;
                { float c = _burst * Sin(t, 330 + 150 * Noise()) * 0.55f; if (_nextPanR) r += c; else l += c; }
                l += Env(t, 0.001, 0.05) * Sin(t, 60) * 0.5f; r += Env(t, 0.001, 0.05) * Sin(t, 60) * 0.5f;
                spk = _burst * Sin(t, 3000 + 1500 * Noise()) * 0.15f; done = t > 0.35; break;
            case Fx::Stagger: // knocked off balance: a sway left, right, left
                l = Hann(t, 0.5) * Sin(t, 50) * 0.6f * (float)(0.5 + 0.5 * std::sin(2 * Pi * 4 * t));
                r = Hann(t, 0.5) * Sin(t, 50) * 0.6f * (float)(0.5 - 0.5 * std::sin(2 * Pi * 4 * t)); done = t > 0.5; break;
            case Fx::Down: // thrown to the ground: body thud and a skid
                l = r = Env(t, 0.003, 0.12) * Sin(t, 40) * 1.0f + Hann(t - 0.05, 0.3) * _bp2.Run(Noise()) * 1.2f; done = t > 0.4; break;
            case Fx::Struggle: // shaking free: hard alternating jerks
                l = ((int)(t * 9) % 2 == 0 ? 1.0f : 0) * Sin(t, 65) * 0.7f * Hann(t, 0.6);
                r = ((int)(t * 9) % 2 == 1 ? 1.0f : 0) * Sin(t, 65) * 0.7f * Hann(t, 0.6); done = t > 0.6; break;
            case Fx::Abyss: // falling into the void: a long sinking sweep that fades to nothing
                l = r = Hann(t, 1.6) * Chirp(t, 140, 30, 1.6) * 0.55f * (float)(1 - t / 1.6); done = t > 1.6; break;
            case Fx::Thunder: // a strike close by: crack, then the roll
                l = r = Env(t, 0.0005, 0.05) * Grain() * 0.9f + Hann(t - 0.05, 1.6) * (_bp2.Run(Noise()) * 1.6f + Sin(t, 32) * 0.5f) * (float)(0.6 + 0.4 * std::sin(2 * Pi * 3 * t));
                done = t > 1.7; break;
            case Fx::Boomerang: // thrown: it whirrs away from the right hand, swings round, comes back
            {
                double p = t / 1.2; float dist = (float)std::sin(Pi * Clamp(p, 0, 1)); // 0 near, 1 far
                float whirr = Sin(t, 12) * 0.5f + 0.5f;
                float v = (float)Hann(t, 1.2) * _bp.Run(Noise()) * 3.0f * whirr * (1 - 0.6f * dist);
                r = v; l = v * dist; done = t > 1.2; break;
            }
            case Fx::BoomerangCatch:
                r = Env(t, 0.001, 0.05) * Sin(t, 90) * 0.7f; l = r * 0.3f; done = t > 0.15; break;
            case Fx::HookshotFire: // the chain ratchets out of the right hand, then bites in
            {
                float rat = ((int)(t * 40) % 2 == 0 ? 1.0f : 0.2f) * Sin(t, 240) * 0.4f * Hann(t, 0.4);
                r = rat + Env(t - 0.4, 0.001, 0.04) * Sin(t, 150) * 0.6f; l = rat * 0.3f;
                spk = ((int)(t * 40) % 2 == 0 ? 1.0f : 0) * Sin(t, 2000) * 0.08f * Hann(t, 0.4); done = t > 0.5; break;
            }
            case Fx::NoteOn: // an ocarina note starts: a soft breath tick
                l = r = Hann(t, 0.03) * Sin(t, 180) * 0.3f; done = t > 0.04; break;
            // ---- gear ----
            case Fx::SwordDraw: // the blade slides out of the scabbard over your back: a rising metallic hiss, then the ring
            {
                float hiss = Hann(t, 0.22) * Grain() * 0.4f * (float)Clamp(t / 0.2, 0, 1);
                float ring = Env(t - 0.2, 0.0005, 0.18) * (Sin(t, 330) + 0.5f * Sin(t, 495)) * 0.45f;
                r = hiss + ring; l = hiss * 0.6f + ring * 0.4f;
                spk = Env(t - 0.2, 0.0005, 0.25) * (Sin(t, 2900) + 0.4f * Sin(t, 4350)) * 0.12f; done = t > 0.5; break;
            }
            case Fx::Sheathe: // back in: a short slide and a click
                r = Hann(t, 0.12) * Grain() * 0.3f + Env(t - 0.12, 0.0005, 0.015) * Sin(t, 260) * 0.6f; l = r * 0.6f; done = t > 0.18; break;
            case Fx::EquipSword:
                l = r = Env(t, 0.0005, 0.2) * (Sin(t, 300) + 0.5f * Sin(t, 450)) * 0.45f; spk = Env(t, 0.0005, 0.3) * Sin(t, 3100) * 0.12f; done = t > 0.4; break;
            case Fx::EquipShieldWood:
                l = Env(t, 0.001, 0.04) * Sin(t, 120) * 0.7f; r = l * 0.4f; done = t > 0.12; break;
            case Fx::EquipShieldMetal:
                l = Env(t, 0.0005, 0.09) * (Sin(t, 250) + 0.5f * Sin(t, 380)) * 0.6f; r = l * 0.4f; done = t > 0.25; break;
            case Fx::EquipCloth: // a tunic going on: a soft swish over both hands
            {
                float a = (float)Clamp(t / 0.3, 0, 1);
                float g = Hann(t, 0.3) * _bp2.Run(Noise()) * 1.3f;
                l = g * (1 - a); r = g * a; done = t > 0.31; break;
            }
            case Fx::EquipHeavy: // iron boots: two heavy clanks, like stepping into them
                l = Env(t, 0.001, 0.08) * (Sin(t, 45) + 0.4f * Sin(t, 210)) * 0.9f; r = Env(t - 0.18, 0.001, 0.08) * (Sin(t, 45) + 0.4f * Sin(t, 210)) * 0.9f; done = t > 0.4; break;
            case Fx::EquipHover: // hover boots: an airy lift
                l = r = Hann(t, 0.4) * Chirp(t, 150, 320, 0.4) * 0.3f * (0.6f + 0.4f * Sin(t, 11)); done = t > 0.4; break;
            case Fx::ItemTick:
                l = r = Hann(t, 0.012) * Sin(t, 230) * 0.35f; done = t > 0.015; break;

            // ---- menus (the pause screen is a page you hold) ----
            case Fx::MenuOpen: // the page slides in from the right
            case Fx::MenuClose:
            {
                float a = (float)Clamp(t / 0.22, 0, 1);
                if (Kind == Fx::MenuClose) a = 1 - a;
                float g = Hann(t, 0.22) * _bp2.Run(Noise()) * 1.6f;
                l = g * a; r = g * (1 - a);
                float settle = Env(t - 0.2, 0.001, 0.02) * Sin(t, 140) * 0.35f; l += settle; r += settle;
                done = t > 0.26; break;
            }
            case Fx::MenuTick: // the cursor moves: a crisp tick on the side it moved to
                l = r = Hann(t, 0.008) * Sin(t, 280) * 0.45f; done = t > 0.01; break;
            case Fx::MenuConfirm:
                l = r = Env(t, 0.0005, 0.02) * Sin(t, 220) * 0.6f + Env(t - 0.03, 0.0005, 0.02) * Sin(t, 300) * 0.45f; done = t > 0.08; break;
            case Fx::MenuBack:
                l = r = Hann(t, 0.03) * Chirp(t, 260, 160, 0.03) * 0.4f; done = t > 0.035; break;
            case Fx::PageFlip: // a page turned to the side you asked for (N 1 left, 2 right)
            {
                float a = (float)Clamp(t / 0.15, 0, 1);
                float g = Hann(t, 0.15) * Grain() * 0.4f;
                float toR = N == 2 ? a : 1 - a;
                l = g * (1 - toR); r = g * toR; done = t > 0.16; break;
            }
            case Fx::TextTick: // the next line of dialogue
                l = r = Hann(t, 0.015) * Sin(t, 190) * 0.3f; done = t > 0.02; break;

            // ---- the day ----
            case Fx::Sunrise: // light spreads: a long warm swell rising
                l = r = Hann(t, 2.5) * Chirp(t, 40, 110, 2.5) * 0.4f; done = t > 2.5; break;
            case Fx::Nightfall: // the day sinks
                l = r = Hann(t, 2.5) * Chirp(t, 110, 35, 2.5) * 0.35f; done = t > 2.5; break;

            // ---- the senses ----
            case Fx::Ping: // something is there (sonar for enemies out of sight)
                l = r = (Hann(t, 0.03) + Hann(t - 0.09, 0.03) * 0.6f) * Sin(t, 120) * 0.5f * S; done = t > 0.13; break;
            case Fx::Puddle: // a foot lands in a puddle: a slap and a few drops
                if (t < 0.08 && Click(t, 120)) _burst = 1;
                _burst *= 0.96f;
                l = r = Env(t, 0.001, 0.025) * Sin(t, 85) * 0.5f + _burst * Grain() * 0.25f; done = t > 0.12; break;
            case Fx::HeatPulse: // heat without a Goron tunic: a slow wave of heat through the hands
                l = r = Hann(t, 0.7) * (Sin(t, 38) * 0.5f + Grain() * 0.15f) * S; done = t > 0.7; break;
            case Fx::AirGasp: // surfacing after a long dive: a big breath in
                l = r = Hann(t, 0.5) * Chirp(t, 60, 160, 0.5) * 0.45f; done = t > 0.5; break;

            // ---- what enemies do ----
            case Fx::Telegraph: // an enemy winds up: air drawn in towards you, then a held breath
            {
                float a = (float)Clamp(t / 0.22, 0, 1);
                l = r = a * a * Hann(t, 0.26) * Grain() * 0.55f * S + Env(t - 0.22, 0.001, 0.02) * Sin(t, 150) * 0.4f * S;
                done = t > 0.3; break;
            }
            case Fx::Alert: // it has seen you
                l = r = (Hann(t, 0.012) + Hann(t - 0.06, 0.012)) * Sin(t, 310) * 0.4f * S; done = t > 0.08; break;
            case Fx::Appear: // something breaks out of the ground or drops down
                l = r = Hann(t, 0.5) * (_bp2.Run(Noise()) * 1.6f + Sin(t, 40) * 0.4f) * (float)Clamp(t / 0.35, 0, 1) * S + Env(t - 0.45, 0.002, 0.08) * Sin(t, 45) * 0.8f * S;
                done = t > 0.65; break;
            case Fx::Stun: // a stunned enemy: dizzy stars circling
                l = Hann(t, 0.6) * Sin(t, 280) * 0.3f * (float)(0.5 + 0.5 * std::sin(2 * Pi * 6 * t)) * S;
                r = Hann(t, 0.6) * Sin(t, 280) * 0.3f * (float)(0.5 - 0.5 * std::sin(2 * Pi * 6 * t)) * S; done = t > 0.6; break;
            case Fx::Blocked: // your blade hits its guard: a dull clank that jars the sword hand
                r = Env(t, 0.0005, 0.06) * (Sin(t, 180) + 0.4f * Sin(t, 270)) * 0.7f + Env(t, 0.002, 0.05) * Sin(t, 60) * 0.6f; l = r * 0.4f;
                done = t > 0.2; break;

            // ---- items used up ----
            case Fx::NutFlash: // a Deku Nut bursts: a sharp crack and a bright fizz
                l = r = Env(t, 0.0003, 0.02) * Grain() * 0.9f + Hann(t, 0.35) * Sin(t, 320 + 80 * Noise()) * 0.25f;
                spk = Env(t, 0.0003, 0.03) * Sin(t, 2200) * 0.2f; done = t > 0.35; break;
            case Fx::StickSnap: // a Deku Stick breaks
                l = r = Env(t, 0.0003, 0.015) * Grain() * 0.8f + Env(t - 0.03, 0.0003, 0.01) * Grain() * 0.5f; done = t > 0.08; break;
            case Fx::ChuRoll: // a Bombchu scurries off ahead: its rattle fades away from you
                l = r = (float)Max(0, 1 - t / 2.5) * ((int)(t * 30) % 2 == 0 ? 1.0f : 0.4f) * Sin(t, 45) * 0.45f; done = t > 2.5; break;

            case Fx::Footstep: // iron boot: a heavy stamp with a metal edge
                l = r = Env(t, 0.002, 0.07) * Sin(t, 42) * 0.9f + Env(t, 0.0005, 0.02) * Sin(t, 230) * 0.35f + MatTex(t, 0.7f); done = t > 0.2; break;

            case Fx::SwingHeavy: // a two-handed blade: a slow broad arc with weight behind it (N 1 left->right, 2 right->left)
            {
                double p = Clamp(t / 0.28, 0, 1);
                float g = Hann(t, 0.28) * (Grain() * 0.45f + Sin(t, 70) * 0.35f) * S;
                float toR = (float)(N == 2 ? 1 - p : p);
                l = g * (1 - toR * 0.8f); r = g * (0.2f + toR * 0.8f); done = t > 0.3; break;
            }
            case Fx::BladeRing: // the Master Sword sings a little as it cuts
                spk = Env(t, 0.0005, 0.12) * (Sin(t, 3300) + 0.5f * Sin(t, 4950)) * 0.1f;
                l = r = Env(t, 0.0005, 0.05) * Sin(t, 330) * 0.15f; done = t > 0.25; break;
            case Fx::Finisher: // the last blow of a combo: body thump through both hands
                l = r = Env(t, 0.002, 0.08) * Sin(t, 48) * 0.9f + Env(t, 0.0005, 0.02) * Sin(t, 200) * 0.5f; done = t > 0.25; break;
            case Fx::Sparkle: // something precious nearby: a glint from its side
                l = r = (Hann(t, 0.02) + Hann(t - 0.07, 0.02) * 0.6f + Hann(t - 0.13, 0.02) * 0.35f) * Sin(t, 300) * 0.35f * S;
                spk = (Hann(t, 0.03) + Hann(t - 0.07, 0.03) * 0.6f) * Sin(t, 3800) * 0.08f * S; done = t > 0.17; break;
            case Fx::Arrive: // a flight / ride ends: you land on something solid
                l = r = Env(t, 0.002, 0.06) * Sin(t, 55) * 0.8f + Env(t, 0.0005, 0.015) * Sin(t, 180) * 0.4f; done = t > 0.2; break;
            case Fx::Unlock: // a key turns: two clicks, the bolt slides, a clunk
                l = r = Env(t, 0.0005, 0.01) * Sin(t, 300) * 0.5f + Env(t - 0.08, 0.0005, 0.01) * Sin(t, 280) * 0.5f + Hann(t - 0.15, 0.2) * _bp2.Run(Noise()) * 1.2f + Env(t - 0.35, 0.002, 0.06) * Sin(t, 60) * 0.7f;
                spk = Env(t, 0.0005, 0.02) * Sin(t, 2500) * 0.2f + Env(t - 0.08, 0.0005, 0.02) * Sin(t, 2300) * 0.2f; done = t > 0.45; break;
            case Fx::ProfileTick: // touchpad strength steps: one tick per step, higher means stronger
                l = r = Hann(t, 0.025) * Sin(t, 160 + 60 * N) * 0.5f; done = t > 0.03; break;

            // ---- v2: time, minigames, world, story ----
            case Fx::TimeShift: // seven years pass: four heartbeats fading away under a long white swell
                for (int i = 0; i < 4; i++) { double u = t - i * 0.65; l += (Hann(u, 0.09) * Sin(u, 50) + Hann(u - 0.17, 0.08) * Sin(u, 44) * 0.7f) * (0.9f - 0.2f * i); }
                l += Hann(t, 2.6) * Chirp(t, 35, 150, 2.6) * 0.3f * (float)Clamp(t / 1.5, 0, 1);
                spk = Hann(t - 2.0, 0.6) * Sin(t, 2400) * 0.12f; r = l; done = t > 2.6; break;
            case Fx::TimerTick: // a second goes by (N 2: the last seconds, sharper)
                l = r = Hann(t, 0.02) * Sin(t, N == 2 ? 260 : 200) * (N == 2 ? 0.6f : 0.35f) * S;
                spk = Hann(t, 0.02) * Sin(t, N == 2 ? 2000 : 1500) * 0.12f; done = t > 0.03; break;
            case Fx::TimeUp: // the buzzer
                l = r = Hann(t, 0.6) * ((int)(t * 20) % 2 == 0 ? 1.0f : 0.3f) * Sin(t, 90) * 0.8f; spk = Hann(t, 0.6) * Sin(t, 600) * 0.15f; done = t > 0.6; break;
            case Fx::StartShot: // the starting shot
                l = r = Env(t, 0.0003, 0.03) * Grain() * 1.0f + Env(t, 0.002, 0.08) * Sin(t, 45) * 0.9f; spk = Env(t, 0.0003, 0.04) * Sin(t, 1100) * 0.25f; done = t > 0.3; break;
            case Fx::TargetHit: // a target hit: bright pop and a click in the hand
                l = r = (Env(t, 0.0003, 0.02) * Sin(t, 290) * 0.7f + Env(t, 0.001, 0.04) * Sin(t, 120) * 0.4f) * S;
                spk = Env(t, 0.0003, 0.05) * (Sin(t, 2800) + 0.5f * Sin(t, 4200)) * 0.25f; done = t > 0.15; break;
            case Fx::Miss: // nothing hit: a dull "pfft"
                l = r = Env(t, 0.003, 0.05) * Sin(t, 70) * 0.4f + Hann(t, 0.08) * _bp2.Run(Noise()) * 0.8f; done = t > 0.12; break;
            case Fx::Fanfare: // a record, a prize: three rising pulses and a held bright tone
                for (int i = 0; i < 3; i++) { double u = t - i * 0.13; l += Hann(u, 0.09) * Sin(u, 200 + 60 * i) * 0.6f; spk += Hann(u, 0.1) * Sin(u, 1600 * std::pow(1.26, i)) * 0.25f; }
                l += Hann(t - 0.4, 0.5) * Sin(t, 320) * 0.35f; r = l; done = t > 0.95; break;
            case Fx::Catch: // a fish on land: it flaps from hand to hand (N 1..3: its size)
            {
                double len = 0.6 + 0.3 * N;
                float flap = Hann(std::fmod(t * 9, 1.0), 0.5) * Sin(t, 70) * 0.6f * (float)Max(0, 1 - t / len);
                if ((int)(t * 9) % 2 == 0) l = flap; else r = flap; done = t > len; break;
            }
            case Fx::Bite: // a fish bites / the hook bites: a hard tug
                l = r = Env(t, 0.0005, 0.03) * Sin(t, 160) * 0.9f + Env(t, 0.002, 0.12) * Sin(t, 45) * 0.9f; done = t > 0.3; break;
            case Fx::Cast: // the rod swings forward, the line whistles out, the lure lands
            {
                float a = (float)Clamp(t / 0.35, 0, 1), g = Hann(t, 0.35) * Grain() * 0.5f;
                l = g * (1 - a) + Env(t - 0.6, 0.001, 0.03) * Sin(t, 90) * 0.4f; r = g * a + Env(t - 0.6, 0.001, 0.03) * Sin(t, 90) * 0.4f;
                spk = Hann(t, 0.35) * Chirp(t, 3000, 1200, 0.35) * 0.1f; done = t > 0.7; break;
            }
            case Fx::WaterRise: // the water level changes: a long rush (N 1 rising, 2 sinking)
                l = r = Hann(t, 2.4) * (_bp2.Run(Noise()) * 1.6f + (N == 2 ? Chirp(t, 90, 40, 2.4) : Chirp(t, 40, 90, 2.4)) * 0.4f); done = t > 2.4; break;
            case Fx::Collapse: // a part of the castle comes down
                if (t < 0.8 && Click(t, 40)) _burst = 1;
                _burst *= 0.99f;
                l = r = Env(t, 0.004, 0.4) * Sin(t, 30) * 1.3f + Hann(t, 1.2) * _bp2.Run(Noise()) * 1.6f + _burst * Grain() * 0.4f; done = t > 1.3; break;
            case Fx::Debris: // a chunk of rubble lands nearby
                if (t < 0.25 && Click(t, 40)) _burst = 1;
                _burst *= 0.98f;
                l = r = Env(t, 0.002, 0.06) * Sin(t, 50) * 0.8f + Env(t, 0.0005, 0.015) * Grain() * 0.6f + _burst * Grain() * 0.25f; done = t > 0.35; break;
            case Fx::ShipSink: // the ship goes down under you, creaking
                if (Click(t, 6)) _burst = 1;
                _burst *= 0.995f;
                l = r = Hann(t, 2.5) * Chirp(t, 70, 25, 2.5) * 0.7f + _burst * Sin(t, 180) * 0.3f; done = t > 2.5; break;
            case Fx::Melt: // ice melts: crackling, then drops
                if (t < 0.6 && Click(t, 60)) _burst = 1;
                _burst *= 0.98f;
                l = r = _burst * Sin(t, 330 + 100 * Noise()) * 0.4f;
                for (double d : { 0.8, 1.1, 1.5 }) l += Env(t - d, 0.0005, 0.006) * Sin(t, 260) * 0.6f;
                r = l; done = t > 1.7; break;
            case Fx::Ignite: // something catches fire: a whoomp and crackles
                if (Click(t, 80 * std::exp(-t / 0.3))) _burst = 1;
                _burst *= 0.99f;
                l = r = Hann(t, 0.25) * Chirp(t, 60, 180, 0.25) * 0.6f + _burst * Sin(t, 230) * 0.35f; done = t > 0.6; break;

            // ---- v2: moves and accidents ----
            case Fx::Dash: // pushing off into a sprint
                l = r = Env(t, 0.002, 0.05) * Sin(t, 60) * 0.7f + Hann(t, 0.3) * Grain() * 0.35f * (float)Clamp(t / 0.15, 0, 1) + MatTex(t, 0.6f); done = t > 0.32; break;
            case Fx::Dodge: // a quick step aside (N 1 to the right, 2 to the left)
            {
                float a = (float)Clamp(t / 0.12, 0, 1), g = Hann(t, 0.12) * (Grain() * 0.5f + Sin(t, 80) * 0.2f);
                float toR = N == 2 ? 1 - a : a; l = g * (1 - toR); r = g * toR; done = t > 0.14; break;
            }
            case Fx::WallKick: // kicking off a wall
                l = r = Env(t, 0.0005, 0.02) * Sin(t, 180) * 0.7f + Env(t, 0.002, 0.05) * Sin(t, 55) * 0.8f; done = t > 0.15; break;
            case Fx::ChainJump: // each jump of a chain a step higher (N 1..6)
                l = r = Env(t, 0.002, 0.035) * Sin(t, 70 + 15 * N) * 0.6f + Hann(t, 0.1) * Chirp(t, 110 + 30 * N, 210 + 50 * N, 0.1) * 0.25f;
                spk = Hann(t, 0.05) * Sin(t, 1200 * std::pow(1.12, N)) * 0.08f; done = t > 0.12; break;
            case Fx::SlideLand: // landing in a slide: skidding to a stop
                l = r = Hann(t, 0.35) * _bp2.Run(Noise()) * 1.4f * (float)(1 - t / 0.35) + Env(t, 0.002, 0.05) * Sin(t, 50) * 0.5f + MatTex(t, 0.8f); done = t > 0.36; break;
            case Fx::QuickTurn: // spinning round on the spot
            {
                float a = (float)Clamp(t / 0.1, 0, 1), g = Hann(t, 0.1) * Grain() * 0.35f;
                l = g * (N == 2 ? a : 1 - a); r = g * (N == 2 ? 1 - a : a); done = t > 0.11; break;
            }
            case Fx::Crash: // running head-first into something
                l = r = Env(t, 0.0005, 0.03) * Grain() * 0.9f + Env(t, 0.003, 0.15) * Sin(t, 38) * 1.2f + MatTex(t, 0.6f); done = t > 0.45; break;
            case Fx::Slip: // the foot slides away (N 1 to the right, 2 to the left)
            {
                float a = (float)Clamp(t / 0.3, 0, 1), g = Hann(t, 0.3) * _bp2.Run(Noise()) * 1.3f;
                float toR = N == 2 ? 1 - a : a; l = g * (1 - toR); r = g * toR; done = t > 0.31; break;
            }
            case Fx::Hammer: // the Megaton Hammer: the blow, then the shockwave rings out three times
                l = r = Env(t, 0.003, 0.25) * Sin(t, 34) * 1.3f + Env(t, 0.0005, 0.03) * Sin(t, 170) * 0.8f;
                for (int i = 1; i <= 3; i++) { double u = t - 0.15 * i; l += Env(u, 0.003, 0.08) * Sin(u, 40) * (0.7f - 0.18f * i); }
                r = l; done = t > 0.8; break;
            case Fx::PickUp:
                l = r = Env(t, 0.002, 0.03) * Sin(t, 140) * 0.4f; done = t > 0.06; break;
            case Fx::Terror: // a ReDead's scream: frozen in place, a cold high tremble
                l = r = Hann(t, 1.0) * Sin(t, 300 + 40 * Noise()) * 0.5f * (0.6f + 0.4f * Sin(t, 23)) + Env(t, 0.001, 0.08) * Sin(t, 45) * 0.8f; done = t > 1.0; break;
            case Fx::Grabbed: // something clamps onto you
                l = r = Env(t, 0.002, 0.1) * Sin(t, 42) * 1.0f + Hann(t, 0.4) * Sin(t, 65) * 0.4f * (0.5f + 0.5f * Sin(t, 6)); done = t > 0.5; break;
            case Fx::Breakfree: // free again: a snap and a release
                l = r = Env(t, 0.0005, 0.03) * Sin(t, 200) * 0.6f + Hann(t, 0.25) * Chirp(t, 80, 220, 0.25) * 0.4f; done = t > 0.3; break;
            case Fx::ChainRattle: // the hookshot found nothing: the chain rattles back in
                r = ((int)(t * 50) % 2 == 0 ? 1.0f : 0.2f) * Sin(t, 220) * 0.35f * Hann(t, 0.45); l = r * 0.3f;
                spk = ((int)(t * 50) % 2 == 0 ? 1.0f : 0) * Sin(t, 1800) * 0.06f * Hann(t, 0.45); done = t > 0.45; break;
            case Fx::Detent: // a click point under the finger (full draw, full charge)
                r = Hann(t, 0.006) * Sin(t, 300) * 0.6f; l = r * 0.3f; done = t > 0.01; break;
            case Fx::Nock: // an arrow laid on the string
                r = Env(t, 0.0003, 0.01) * Sin(t, 280) * 0.5f; l = r * 0.3f; spk = Env(t, 0.0003, 0.015) * Sin(t, 2600) * 0.25f; done = t > 0.04; break;
            case Fx::AimTick: // the aim moves a notch (gyro aiming)
                r = Hann(t, 0.004) * Sin(t, 320) * 0.3f; l = r * 0.5f; done = t > 0.006; break;
            case Fx::Compass: // Navi points the way: three pings from where it is
                for (int i = 0; i < 3; i++) { double u = t - i * 0.11; l += Hann(u, 0.03) * Sin(u, 200 + 40 * i) * 0.5f; spk += Hann(u, 0.03) * Sin(u, 2200 + 300 * i) * 0.1f; }
                r = l; done = t > 0.35; break;

            // ---- v2: enemies ----
            case Fx::ArmorBreak: // an Iron Knuckle's armour bursts: a clang and shards flying
                if (t < 0.3 && Click(t, 120 * std::exp(-t / 0.15))) { _burst = 1; _nextPanR = NextDouble() < 0.5; }
                _burst *= 0.985f;
                l = r = Env(t, 0.0005, 0.12) * (Sin(t, 230) + 0.5f * Sin(t, 345)) * 0.7f;
                { float c = _burst * Sin(t, 330 + 150 * Noise()) * 0.5f; if (_nextPanR) r += c; else l += c; }
                done = t > 0.5; break;
            case Fx::WeakSpot: // a weak point hit: a critical spark
                l = r = Env(t, 0.0003, 0.05) * (Sin(t, 320) + 0.6f * Sin(t, 480)) * 0.7f + Env(t, 0.002, 0.08) * Sin(t, 50) * 0.8f;
                spk = Env(t, 0.0003, 0.08) * Sin(t, 3500) * 0.2f; done = t > 0.3; break;
            case Fx::Projectile: // something flies at you and past
                l = r = Hann(t, 0.4) * Chirp(t, 260, 120, 0.4) * 0.4f; done = t > 0.4; break;
            case Fx::Rage: // an enemy roars / gets angry: a wave from its side
                l = r = Hann(t, 0.7) * (_bp2.Run(Noise()) * 1.5f + Sin(t, 36) * 0.5f) * (0.7f + 0.3f * Sin(t, 11)) * S; done = t > 0.7; break;
            case Fx::Relief: // the pressure lets go (an enemy flees, danger passes)
                l = r = Hann(t, 0.6) * Chirp(t, 110, 50, 0.6) * 0.3f * S; done = t > 0.6; break;
            case Fx::Caught: // a guard caught you: three hard alarm beats
                for (int i = 0; i < 3; i++) { double u = t - i * 0.18; l += Env(u, 0.001, 0.06) * Sin(u, 60) * 1.0f + Env(u, 0.0005, 0.02) * Sin(u, 250) * 0.5f; spk += Hann(u, 0.1) * Sin(u, 1800) * 0.2f; }
                r = l; done = t > 0.6; break;
            case Fx::SwordLost: // the sword is knocked out of your hand and flies away
                r = Env(t, 0.0005, 0.05) * Sin(t, 280) * 0.6f + Hann(t, 0.6) * Chirp(t, 300, 80, 0.6) * 0.35f;
                l = Hann(t, 0.6) * Chirp(t, 300, 80, 0.6) * 0.35f * (float)Clamp(t / 0.6, 0, 1); done = t > 0.6; break;

            // ---- v2: things in the hand ----
            case Fx::MaskOn: // a mask goes on (N 2: comes off)
                l = r = Hann(t, 0.15) * _bp2.Run(Noise()) * 1.2f + Env(t - 0.15, 0.0005, 0.015) * Sin(t, N == 2 ? 180 : 240) * 0.5f; done = t > 0.2; break;
            case Fx::BottleCatch: // into the bottle, then what's inside moves (N 1 fairy, 2 bug, 3 fish, 4 poe, 5 blue fire, 6 liquid)
            {
                l = r = Hann(t, 0.15) * Grain() * 0.35f;
                double u = t - 0.15;
                if (u > 0 && u < 0.75)
                {
                    float c = 0;
                    switch (N)
                    {
                        case 1: if (Click(u, 40)) _burst = 1; _burst *= 0.993f; c = _burst * Sin(t, 300 + 60 * Noise()) * 0.5f; spk = _burst * Sin(t, 2600 + 900 * _burst) * 0.3f; break;
                        case 2: c = ((int)(u * 25) % 2 == 0 ? 1.0f : 0) * Sin(t, 260) * 0.25f; break;
                        case 3: c = Hann(std::fmod(u * 8, 1.0), 0.5) * Sin(t, 70) * 0.5f; break;
                        case 4: c = Hann(u, 0.75) * Chirp(u, 320, 90, 0.75) * 0.35f; break;
                        case 5: if (Click(u, 80)) _burst = 1; _burst *= 0.99f; c = _burst * Sin(t, 230) * 0.4f; break;
                        default: c = Sin(t, 55) * (0.5f + 0.5f * Sin(t, 6)) * 0.4f * Hann(u, 0.75); break;
                    }
                    if ((int)(u * 8) % 2 == 0) l += c; else r += c;
                }
                float pop = Env(t - 0.9, 0.0005, 0.02) * Sin(t, 280) * 0.5f; l += pop; r += pop;
                spk += Env(t - 0.9, 0.0005, 0.03) * Sin(t, 1500) * 0.3f; done = t > 1.0; break;
            }
            case Fx::Gulp: // drinking: N gulps
                for (int i = 0; i < N; i++) { double u = t - i * 0.28; l += Hann(u, 0.12) * Chirp(u, 90, 50, 0.12) * 0.6f; }
                r = l; done = t > N * 0.28 + 0.05; break;
            case Fx::Peck: // a cucco's beak
                l = r = Env(t, 0.0003, 0.012) * Sin(t, 260) * 0.6f + Env(t, 0.001, 0.02) * Sin(t, 110) * 0.3f; done = t > 0.05; break;

            case Fx::Chime:
                for (int i = 0; i < 3; i++) { double u = t - i * 0.09; l += Hann(u, 0.08) * Sin(u, 220 + 50 * i) * 0.3f; spk += Hann(u, 0.1) * Sin(u, 1800 + 450 * i) * 0.15f; }
                r = l; done = t > 0.35; break;

            default: done = true; break;
        }

        (void)spk;
        if (pan != 0) {
            l *= static_cast<float>(Clamp(1 - pan, 0.15, 1));
            r *= static_cast<float>(Clamp(1 + pan, 0.15, 1));
        }
        out_l = l * G;
        out_r = r * G;
    }

private:
    static bool IsBody(Fx k) {
        return k == Fx::Land || k == Fx::Jump || k == Fx::Roll || k == Fx::Flip || k == Fx::Footstep || k == Fx::Down ||
               k == Fx::Slam || k == Fx::Stagger || k == Fx::Grip || k == Fx::Swing || k == Fx::SwingHeavy || k == Fx::Hit ||
               k == Fx::ChainJump || k == Fx::SlideLand || k == Fx::Dash || k == Fx::Dodge || k == Fx::WallKick || k == Fx::Crash ||
               k == Fx::Slip || k == Fx::Puddle || k == Fx::QuickTurn;
    }
    // the ground's own texture on top of an impact
    float MatTex(double t, float a) {
        float v = 0;
        switch (_mat) {
        case Mat::Grass: v = Hann(t, 0.09) * _bp2.Run(Noise()) * 0.9f; break;
        case Mat::Stone: v = Env(t, 0.0005, 0.012) * Sin(t, 210) * 0.35f; break;
        case Mat::Wood: v = Env(t, 0.001, 0.04) * Sin(t, 120) * 0.4f; break;
        case Mat::Sand: v = Hann(t, 0.12) * Grain() * 0.3f; break;
        case Mat::Water: v = Env(t, 0.001, 0.03) * Sin(t, 85) * 0.3f + Hann(t, 0.1) * Grain() * 0.2f; break;
        case Mat::Ice: v = Env(t, 0.0003, 0.03) * Sin(t, 330) * 0.3f; break;
        case Mat::Flesh:
            v = Hann(t, 0.15) * static_cast<float>(std::sin(2 * Pi * (45 * t + 20 / (2 * Pi * 14) * std::sin(2 * Pi * 14 * t)))) * 0.35f;
            break;
        case Mat::Metal: v = Env(t, 0.0005, 0.08) * (Sin(t, 250) + 0.5f * Sin(t, 375)) * 0.3f; break;
        default: break;
        }
        return a * v;
    }
    static float Env(double t, double a, double d) {
        return t < 0 ? 0.0f : static_cast<float>((t < a ? t / a : 1) * std::exp(-(t - Min(t, a)) / d));
    }
    static float Sin(double t, double f) {
        return static_cast<float>(std::sin(2 * Pi * f * t));
    }
    static float Hann(double t, double len) {
        return t < 0 || t > len ? 0.0f : static_cast<float>(0.5 - 0.5 * std::cos(2 * Pi * t / len));
    }
    static float Chirp(double t, double f0, double f1, double len) {
        return static_cast<float>(std::sin(2 * Pi * (f0 * t + (f1 - f0) * t * t / (2 * len))));
    }
    double NextDouble() {
        return _uniform(_rng);
    }
    float Noise() {
        return static_cast<float>(NextDouble() * 2 - 1);
    }
    float Grain() {
        return _bp.Run(Noise()) * 4.0f;
    }
    // random clicks at `rate` per second: true for the sample a click starts
    bool Click(double t, double rate) {
        if (t < _nextClick) return false;
        _nextClick = t + -std::log(1 - NextDouble()) / Max(rate, 0.1);
        return true;
    }

    double T = 0;
    std::mt19937 _rng;
    std::uniform_real_distribution<double> _uniform{0.0, 1.0};
    Biquad _bp = Biquad::BandPass(230, 1.2), _bp2 = Biquad::BandPass(140, 1.0);
    float _burst = 0;
    double _nextClick = 0;
    double _speed = 1;
    Mat _mat = Mat::Default;
    bool _nextPanR = false;
};

struct GrainT {
    double t0, f;
    float amp, pan;
    double len;
    GrainT(double t0_, double f_, double amp_, double pan_, double len_)
        : t0{t0_}, f{f_}, amp{static_cast<float>(amp_)}, pan{static_cast<float>(pan_)}, len{len_} {}
};

class Engine {
public:
    void Play(Fx fx, float strength, int n, float pan) {
        const float g = static_cast<float>(Clamp(FxGain[static_cast<int>(fx)], 0, 3)) * g_params.fx_scale.load() * Habit(fx);
        std::scoped_lock lock{pending_mutex};
        HVoice v{fx, static_cast<float>(Clamp(strength, 0.1, 1.5)), static_cast<int>(Clamp(n, 1, 6)), static_cast<float>(Clamp(pan, -1, 1)), ++seed};
        v.G = g;
        pending.push_back(std::move(v));
    }
    void Duck(int ms) { duck_until = (std::max)(duck_until.load(), NowMs() + ms); }
    void Hush(int ms) { hush_until = (std::max)(hush_until.load(), NowMs() + ms); }

    void Render(float& out_l, float& out_r) {
        if (block_left-- <= 0) { // per ~10 ms block, as Haptics.Render runs per buffer
            block_left = 480;
            {
                std::scoped_lock lock{pending_mutex};
                for (auto& v : pending) voices.push_back(std::move(v));
                pending.clear();
            }
            while (voices.size() > 14) voices.pop_front(); // the oldest goes first
            for (int i = 0; i < static_cast<int>(Loop::Count); i++) {
                Loops[i] = g_params.loops[i].load(std::memory_order_relaxed);
                _loopGain[i] = LoopGain[i];
            }
            SwimRate = g_params.swim_rate; HoofRate = g_params.hoof_rate; NoteHz = g_params.note_hz; LoopSpeed = g_params.loop_speed;
            MotorUp = g_params.motor_up; WarmPan = g_params.warm_pan; DangerPan = g_params.danger_pan; MistPan = g_params.mist_pan;
            GhostPan = g_params.ghost_pan; DragPan = g_params.drag_pan; HoofPan = g_params.hoof_pan; RainRoof = g_params.rain_roof;
            g_params.hoof_phase = HoofPhase;
            g_params.swim_phase = SwimPhase;
            UpdateDuck();
            if (trace && ++trace_blocks >= 500) { // FRITCHII_PULSE_TRACE=1: every 5 s, what plays
                trace_blocks = 0;
                std::string on;
                for (int i = 0; i < static_cast<int>(Loop::Count); i++) {
                    if (_loopLvl[i] > 0.01f) on += " loop" + std::to_string(i) + "=" + std::to_string(_loopLvl[i]).substr(0, 4);
                }
                LOG_INFO(Input, "Pulse haptics: {} voices, peak {:.2f}, tension {:.2f}, charge {:.2f}, duck {:.2f}{}", voices.size(),
                         trace_peak, _tension, g_params.charge.load(), _duckLvl, on);
                trace_peak = 0;
            }
        }
        float l = 0, r = 0;
        _t += 1.0 / Rate;
        // bow draw: a creaking string under the right hand; tension follows the real R2 pull when known
        const float pull = g_params.pull.load(std::memory_order_relaxed);
        const bool aiming = g_params.aiming.load(std::memory_order_relaxed);
        const float target = pull >= 0 ? pull : 1.0f;
        _tension = aiming ? (_tension < target ? (std::min)(target, _tension + 1.0f / static_cast<float>(0.25 * Rate))
                                               : (std::max)(target, _tension - 1.0f / static_cast<float>(0.05 * Rate)))
                          : (std::max)(0.0f, _tension - 1.0f / static_cast<float>(0.08 * Rate));
        if (_tension > 0) {
            _creakPhase += (14 + 22 * _tension) / Rate;
            const double g = _creakPhase - std::floor(_creakPhase);
            const double pitch = g_params.rubber.load(std::memory_order_relaxed) ? 140 + 60 * _tension : 230;
            const float grain = g < 0.12 ? static_cast<float>(std::sin(Pi * g / 0.12) * std::sin(2 * Pi * pitch * _t)) : 0.0f;
            r += grain * (0.12f + 0.4f * _tension) * CreakGain;
            l += grain * 0.08f * _tension * CreakGain;
        }
        // the spin attack charging: a buzz that climbs in pitch and strength
        const float c0 = g_params.charge.load(std::memory_order_relaxed);
        if (c0 > 0) {
            _chargePhase += (60 + 140 * c0) / Rate;
            const float buzz = static_cast<float>(std::sin(2 * Pi * _chargePhase)) * (0.08f + 0.35f * c0) *
                               (0.75f + 0.25f * static_cast<float>(std::sin(2 * Pi * (8 + 14 * c0) * _t))) * ChargeGain;
            l += buzz;
            r += buzz;
        }
        RenderLoops(l, r);
        for (size_t v = voices.size(); v-- > 0;) {
            float vl, vr;
            bool done = false;
            voices[v].Next(vl, vr, done);
            l += vl;
            r += vr;
            if (done) voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(v));
        }
        out_l = l;
        out_r = r;
        trace_peak = (std::max)(trace_peak, (std::max)(std::abs(l), std::abs(r)));
    }

private:
    // The same effect again within 2 s is 8 % softer each time (down to 65 %); rhythms, meters and combat stay
    float Habit(Fx fx) {
        switch (fx) {
        case Fx::Grip: case Fx::Footstep: case Fx::Puddle: case Fx::NoteOn: case Fx::MenuTick: case Fx::ItemTick:
        case Fx::TextTick: case Fx::Heartbeat: case Fx::Ping: case Fx::Sparkle: case Fx::Coin: case Fx::ProfileTick:
        case Fx::TimerTick: case Fx::Detent: case Fx::AimTick: case Fx::Swing: case Fx::SwingHeavy: case Fx::ChainJump:
        case Fx::Peck: case Fx::Debris: case Fx::Hello: case Fx::EnemyHit: case Fx::Hit: case Fx::ShieldBlock:
        case Fx::Blocked: case Fx::Finisher:
            return 1;
        default:
            break;
        }
        const long long now = NowMs();
        std::scoped_lock lock{habit_mutex};
        auto& h = habit[static_cast<int>(fx)];
        const int k = now - h.first < 2000 ? h.second + 1 : 0;
        h = {now, k};
        return (std::max)(0.65f, 1 - 0.08f * k);
    }
    void UpdateDuck() {
        const long long now = NowMs();
        const float tgt = now < hush_until ? 0.03f : now < duck_until ? 0.45f : 1.0f;
        _duckLvl += (tgt - _duckLvl) * 0.15f;
    }
    float N() { return static_cast<float>(NextDouble() * 2 - 1); }
    double NextDouble() { return _uniform(_rng); }
    double Exp(double rate) { return -std::log(1 - NextDouble()) / Max(rate, 0.01); }
    static float Hann(double t, double len) {
        return t < 0 || t > len ? 0.0f : static_cast<float>(0.5 - 0.5 * std::cos(2 * Pi * t / len));
    }

    void RenderLoops(float& l, float& r) {
        const float up = 1.0f / (0.06f * Rate), down = 1.0f / (0.25f * Rate);
        bool any = false;
        for (int i = 0; i < static_cast<int>(Loop::Count); i++)
        {
            float tgt = Clamp(Loops[i] * _loopGain[i], 0, 2.5f);
            _loopLvl[i] += Clamp(tgt - _loopLvl[i], -down, up);
            any |= _loopLvl[i] > 0.001f;
        }
        if (!any && static_cast<int>(_grains.size()) == 0) return;
        double t = _t;
        float L = 0, R = 0;

        float sw = _loopLvl[static_cast<int>(Loop::Swim)];
        if (sw > 0)
        {
            float flowL = _wFlowL.Run(N()) * 2.2f, flowR = _wFlowR.Run(N()) * 2.2f;
            _swimPh += SwimRate / Rate;
            SwimPhase = _swimPh;
            double ph = std::fmod(_swimPh, 1.0); bool odd = (int)_swimPh % 2 == 1;
            float wave = (float)std::pow(std::sin(Pi * ph), 2) * (0.3f + 0.15f * (float)std::sin(2 * Pi * 65 * t));
            float pos = (float)(odd ? 1 - ph : ph); // the wave washes from one hand to the other
            L += sw * (flowL * 0.35f + wave * (1 - pos) * 0.9f);
            R += sw * (flowR * 0.35f + wave * pos * 0.9f);
        }

        float uw = _loopLvl[static_cast<int>(Loop::Underwater)];
        if (uw > 0)
        {
            float hum = (float)std::sin(2 * Pi * 34 * t) * 0.22f * (0.8f + 0.2f * (float)std::sin(2 * Pi * 0.3 * t));
            L += uw * hum; R += uw * hum;
            if (t >= _bubNext) { _grains.push_back({t, -1, 0.3f * uw, N(), 0.07}); _bubNext = t + Exp(7); } // f<0: rising chirp
        }

        float rn = _loopLvl[static_cast<int>(Loop::Rain)];
        if (rn > 0 && t >= _rainNext)
        {
            float pan = Clamp(N() * 1.15f, -1, 1);
            if (RainRoof) _grains.push_back({t, 110 + 30 * NextDouble(), (0.12f + 0.12f * (float)NextDouble()) * rn, pan, 0.012});
            else
            {
                bool big = NextDouble() < 0.08;
                _grains.push_back({t, 230 + 110 * NextDouble(), (big ? 0.45f : 0.08f + 0.18f * (float)NextDouble()) * rn, pan, big ? 0.008 : 0.003});
            }
            _rainNext = t + Exp((RainRoof ? 18 : 30) + 70 * rn);
        }

        float fw = _loopLvl[static_cast<int>(Loop::FallWind)];
        if (fw > 0)
        {
            float wob = 0.5f + 0.5f * (float)std::sin(2 * Pi * 1.7 * t);
            L += fw * _wAirL.Run(N()) * 3.0f * (0.6f + 0.4f * wob); R += fw * _wAirR.Run(N()) * 3.0f * (1 - 0.4f * wob);
        }

        float bu = _loopLvl[static_cast<int>(Loop::Burn)];
        if (bu > 0)
        {
            if (t >= _fireNext) { _grains.push_back({t, 200 + 120 * NextDouble(), 0.5f * bu, N(), 0.004}); _fireNext = t + Exp(45); }
            float hum = (float)std::sin(2 * Pi * 70 * t) * 0.18f * bu * (0.7f + 0.3f * N());
            L += hum; R += hum;
        }

        float sh = _loopLvl[static_cast<int>(Loop::Shock)];
        if (sh > 0)
        {
            float chop = ((int)(t * 15) % 2 == 0 ? 1.0f : 0.15f);
            float z = sh * chop * ((float)std::sin(2 * Pi * 290 * t) * 0.5f + (float)std::sin(2 * Pi * 50 * t) * 0.3f);
            L += z; R += z * ((int)(t * 7) % 2 == 0 ? 1.0f : 0.6f);
        }

        float fr = _loopLvl[static_cast<int>(Loop::Frozen)];
        if (fr > 0)
        {
            if (t >= _crackNext) { _grains.push_back({t, 330 + 160 * NextDouble(), 0.4f * fr, N(), 0.01}); _crackNext = t + Exp(3); }
            float stiff = (float)std::sin(2 * Pi * 25 * t) * 0.12f * fr;
            L += stiff; R += stiff;
        }

        float sq = _loopLvl[static_cast<int>(Loop::Squeeze)];
        if (sq > 0)
        {
            _sqPh += 1.2 / Rate;
            double p = std::fmod(_sqPh, 1.0);
            float push = (float)std::pow(Max(0, std::sin(2 * Pi * p)), 2) * (float)std::sin(2 * Pi * (55 + 15 * std::sin(2 * Pi * 9 * t)) * t) * 0.6f;
            L += sq * push * (float)(1 - p); R += sq * push * (float)p;
        }

        float st = _loopLvl[static_cast<int>(Loop::Strain)];
        if (st > 0)
        {
            float trem = (0.5f + 0.5f * (float)std::sin(2 * Pi * 11 * t)) * (float)std::sin(2 * Pi * 140 * t) * 0.14f * st;
            L += trem; R += trem * 0.9f;
        }

        float gr = _loopLvl[static_cast<int>(Loop::Grind)];
        if (gr > 0)
        {
            float g = _wGrind.Run(N()) * 3.5f * gr * (0.5f + LoopSpeed);
            L += g; R += g;
        }

        float sl = _loopLvl[static_cast<int>(Loop::Slide)];
        if (sl > 0) { float g = _wSlide.Run(N()) * 2.5f * sl; L += g; R += g * 0.9f; }

        float wi = _loopLvl[static_cast<int>(Loop::Wind)];
        if (wi > 0)
        {
            float g = _wWind.Run(N()) * 3.0f * wi, a = 0.5f + 0.5f * (float)std::sin(2 * Pi * 0.4 * t);
            L += g * (1 - a); R += g * a;
        }

        float ho = _loopLvl[static_cast<int>(Loop::Hooves)];
        if (ho > 0)
        {
            _hoofPh += HoofRate / Rate;
            HoofPhase = _hoofPh;
            double p = std::fmod(_hoofPh, 1.0);
            // canter: hind left (left palm), hind right (right palm), the fore hooves land in the triggers
            // (the daemon reads HoofPhase and pulses L2 at 0.18, R2 at 0.36); the palms only echo them
            float h1 = Hann(p, 0.07) * 0.6f, h2 = Hann(p - 0.18, 0.07) * 0.5f, h3 = Hann(p - 0.36, 0.08) * 0.3f;
            float body = (float)std::sin(2 * Pi * 58 * t);
            float lean = Clamp(HoofPan, -1, 1) * 0.45f; // the weight goes to the outside of a turn
            L += ho * body * (h1 + h3 * 0.5f) * (1 - lean); R += ho * body * (h2 + h3) * (1 + lean);
        }

        float nt = _loopLvl[static_cast<int>(Loop::Note)];
        if (nt > 0)
        {
            _notePh += NoteHz * (1 + 0.012 * std::sin(2 * Pi * 5.5 * t)) / Rate; // a little vibrato, like breath
            float v = (float)std::sin(2 * Pi * _notePh) * 0.35f * nt;
            L += v; R += v;
        }

        float cr = _loopLvl[static_cast<int>(Loop::Crawl)];
        if (cr > 0)
        {
            _crawlPh += (0.8 + 1.5 * LoopSpeed) / Rate;
            double p = std::fmod(_crawlPh, 1.0);
            float scrape = Hann(p, 0.3) * _wSlide.Run(N()) * 2.0f * cr;
            if ((int)_crawlPh % 2 == 0) L += scrape; else R += scrape;
        }

        float fu = _loopLvl[static_cast<int>(Loop::Fuse)];
        if (fu > 0)
        {
            if (t >= _fireNext) { _grains.push_back({t, 280 + 120 * NextDouble(), 0.35f * fu, 0.7f + 0.3f * N(), 0.003}); _fireNext = t + Exp(90); }
            float hiss = _wSlide.Run(N()) * 1.2f * fu;
            R += hiss; L += hiss * 0.25f;
        }

        float mo = _loopLvl[static_cast<int>(Loop::Motor)];
        if (mo > 0)
        {
            _motorPh += (55 + 22 * MotorUp) / Rate;
            float hum = ((float)std::sin(2 * Pi * _motorPh) + 0.3f * (float)std::sin(4 * Pi * _motorPh)) * 0.3f * mo * (0.85f + 0.15f * (float)std::sin(2 * Pi * 7 * t));
            L += hum; R += hum;
        }

        float wa = _loopLvl[static_cast<int>(Loop::Warmth)];
        if (wa > 0)
        {
            float swell = (0.55f + 0.45f * (float)std::sin(2 * Pi * 0.7 * t)) * (float)std::sin(2 * Pi * 36 * t) * 0.3f * wa;
            if (t >= _fireNext && NextDouble() < 0.3) { _grains.push_back({t, 210 + 80 * NextDouble(), 0.2f * wa, WarmPan, 0.004}); _fireNext = t + Exp(6); }
            L += swell * Clamp(1 - WarmPan, 0.2f, 1); R += swell * Clamp(1 + WarmPan, 0.2f, 1);
        }

        float da = _loopLvl[static_cast<int>(Loop::Danger)];
        if (da > 0)
        {
            float rl = _wRumL.Run(N()) * 4.0f * da, rr = _wRumR.Run(N()) * 4.0f * da;
            L += rl * Clamp(1 - DangerPan, 0.15f, 1); R += rr * Clamp(1 + DangerPan, 0.15f, 1);
        }

        float mi = _loopLvl[static_cast<int>(Loop::Mist)];
        if (mi > 0 && t >= _mistNext)
        {
            _grains.push_back({t, 240 + 120 * NextDouble(), (0.06f + 0.1f * (float)NextDouble()) * mi, Clamp(MistPan + 0.4f * N(), -1, 1), 0.003});
            _mistNext = t + Exp(25 + 50 * mi);
        }

        float si = _loopLvl[static_cast<int>(Loop::Sink)];
        if (si > 0)
        {
            float pull = (float)std::sin(2 * Pi * 28 * t) * 0.25f * si * (0.6f + 0.4f * (float)std::sin(2 * Pi * 0.5 * t)) + _wSlide.Run(N()) * 0.6f * si;
            L += pull; R += pull;
        }

        float qk = _loopLvl[static_cast<int>(Loop::Quake)];
        if (qk > 0)
        {
            float wob = 0.5f + 0.5f * (float)std::sin(2 * Pi * 3.3 * t);
            float body = (float)std::sin(2 * Pi * (34 + 6 * std::sin(2 * Pi * 9 * t)) * t) * 0.5f;
            L += qk * (_wRumL.Run(N()) * 3.0f + body) * (0.6f + 0.4f * wob); R += qk * (_wRumR.Run(N()) * 3.0f + body) * (1 - 0.4f * wob);
        }

        float sm = _loopLvl[static_cast<int>(Loop::Swarm)];
        if (sm > 0)
        {   // pecks land anywhere, faster as the swarm grows; wings flutter all around
            if (t >= _swarmNext) { _grains.push_back({t, 250 + 40 * NextDouble(), 0.4f * sm, N(), 0.006}); _swarmNext = t + Exp(6 + 16 * sm); }
            float flutter = _wAirL.Run(N()) * 1.2f * sm * (0.5f + 0.5f * (float)std::sin(2 * Pi * 13 * t));
            L += flutter; R += _wAirR.Run(N()) * 1.2f * sm * (0.5f - 0.5f * (float)std::sin(2 * Pi * 13 * t));
        }

        float fl = _loopLvl[static_cast<int>(Loop::Flap)];
        if (fl > 0)
        {   // wing beats, one hand then the other
            _flapPh += 4.5 / Rate;
            float beat = Hann(std::fmod(_flapPh, 1.0), 0.35) * (_wWind.Run(N()) * 2.5f + (float)std::sin(2 * Pi * 60 * t) * 0.2f) * fl;
            if ((int)_flapPh % 2 == 0) L += beat; else R += beat;
        }

        float rk = _loopLvl[static_cast<int>(Loop::Rock)];
        if (rk > 0)
        {   // the deck tilts slowly from side to side; the planks creak at the turn
            float a = (float)std::sin(2 * Pi * 0.22 * t);
            float body = (_wRumL.Run(N()) * 2.0f + (float)std::sin(2 * Pi * 30 * t) * 0.15f) * rk;
            L += body * (0.5f - 0.5f * a); R += body * (0.5f + 0.5f * a);
            if (t >= _creakNext && std::abs(a) > 0.9f) { _grains.push_back({t, 170 + 30 * NextDouble(), 0.25f * rk, a, 0.012}); _creakNext = t + 0.6 + NextDouble(); }
        }

        float gh = _loopLvl[static_cast<int>(Loop::Ghost)];
        if (gh > 0)
        {
            float v = (float)std::sin(2 * Pi * (300 * t + 30 / (2 * Pi * 3) * std::sin(2 * Pi * 3 * t))) * 0.12f * gh * (0.5f + 0.5f * (float)std::sin(2 * Pi * 0.7 * t))
                    + _wMist.Run(N()) * 0.4f * gh;
            L += v * Clamp(1 - GhostPan, 0.15f, 1); R += v * Clamp(1 + GhostPan, 0.15f, 1);
        }

        float dr = _loopLvl[static_cast<int>(Loop::Drag)];
        if (dr > 0)
        {
            float v = (_wRumR.Run(N()) * 3.0f + (float)std::sin(2 * Pi * 45 * t) * 0.2f) * dr * (0.6f + 0.4f * (float)std::sin(2 * Pi * 1.3 * t));
            L += v * Clamp(1 - DragPan, 0.15f, 1); R += v * Clamp(1 + DragPan, 0.15f, 1);
        }

        // short grains scattered across the hands (droplets, bubbles, crackles, ice cracks)
        for (int i = static_cast<int>(_grains.size()) - 1; i >= 0; i--)
        {
            const auto g = _grains[i];
            double u = t - g.t0;
            if (u > g.len) { _grains.erase(_grains.begin() + i); continue; }
            float env = Hann(u, g.len);
            float s = g.f < 0 ? (float)std::sin(2 * Pi * (90 * u + 900 * u * u)) : (float)std::sin(2 * Pi * g.f * u);
            float v = s * env * g.amp;
            L += v * Clamp(1 - g.pan, 0, 1); R += v * Clamp(1 + g.pan, 0, 1);
        }
        l += L * _duckLvl; r += R * _duckLvl;

    }

    std::mutex pending_mutex, habit_mutex;
    std::vector<HVoice> pending;
    std::deque<HVoice> voices;
    int seed = 0, block_left = 0, trace_blocks = 0;
    const bool trace = [] {
        const char* e = std::getenv("FRITCHII_PULSE_TRACE");
        return e && e[0] == '1';
    }();
    float trace_peak = 0;
    std::array<std::pair<long long, int>, static_cast<int>(Fx::Chime) + 1> habit{};
    std::atomic<long long> duck_until{0}, hush_until{0};
    float _duckLvl = 1, _tension = 0;
    double _creakPhase = 0, _chargePhase = 0, _t = 0;
    std::mt19937 _rng{1};
    std::uniform_real_distribution<double> _uniform{0.0, 1.0};

    // HapticLoops.cs's state
    float Loops[static_cast<int>(Loop::Count)]{};
    float _loopLvl[static_cast<int>(Loop::Count)]{};
    float _loopGain[static_cast<int>(Loop::Count)]{};
    float SwimRate = 0.8f, HoofRate = 2, NoteHz = 200, LoopSpeed = 0;
    bool RainRoof = false;
    double HoofPhase = 0, SwimPhase = 0;
    float MotorUp = 0, WarmPan = 0, DangerPan = 0, MistPan = 0, GhostPan = 0, DragPan = 0, HoofPan = 0;
    double _swarmNext = 0, _flapPh = 0, _creakNext = 0, _motorPh = 0, _mistNext = 0;
    Biquad _wMist = Biquad::BandPass(260, 1.2), _wRumL = Biquad::BandPass(38, 0.9), _wRumR = Biquad::BandPass(38, 0.9);
    Biquad _wFlowL = Biquad::BandPass(55, 0.9), _wFlowR = Biquad::BandPass(55, 0.9);
    Biquad _wAirL = Biquad::BandPass(120, 0.8), _wAirR = Biquad::BandPass(120, 0.8);
    Biquad _wGrind = Biquad::BandPass(70, 1.2), _wSlide = Biquad::BandPass(180, 1.0), _wWind = Biquad::BandPass(100, 0.7);
    double _swimPh = 0, _hoofPh = 0, _notePh = 0, _sqPh = 0, _rainNext = 0, _bubNext = 0, _crackNext = 0, _fireNext = 0, _crawlPh = 0;
    std::vector<GrainT> _grains;
};

Engine& GetEngine() {
    static Engine engine;
    return engine;
}

} // namespace

Params& GetParams() {
    return g_params;
}
void Play(Fx fx, float strength, int n, float pan) {
    GetEngine().Play(fx, strength, n, pan);
}
void Duck(int ms) {
    GetEngine().Duck(ms);
}
void Hush(int ms) {
    GetEngine().Hush(ms);
}
void Render(float& l, float& r) {
    if (!g_params.active.load(std::memory_order_relaxed)) {
        l = r = 0;
        return;
    }
    GetEngine().Render(l, r);
}

} // namespace Common::PulseHaptics
