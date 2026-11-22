// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>

/// Pulse of Time's procedural haptics (github.com/Balemuni/pulse-of-time, GPL-3.0-or-later, commit 6d2cf17):
/// every effect of HapticFx.cs and every texture of HapticLoops.cs, with the bow string's creak and the spin
/// charge's buzz (Haptics.Render), synthesised per sample for the DualSense's two voice coils. A game feature
/// raises them (core/oot_dualsense.cpp); the voice-coil haptics (input_common/helpers/dualsense_haptics.cpp)
/// mix them over the game's own HD rumble. Not ported: Haptics.cs's game-sound layer.
namespace Common::PulseHaptics {

enum class Fx : int { Hello, Hit, Heal, Heartbeat, Land, Jump, Roll, Swing, SpinRelease, ShieldBlock, LockOn, Grip, Spur, Revive, Death, DeathFire, DeathDrown, DeathShock, BowRelease, SlingRelease, DryFire, EnemyHit, KillPop, KillCrunch, KillBones, KillArmor, KillSquish, KillGhost, KillHeavy, KillMidBoss, KillBoss, Token, Coin, CoinSpend, AmmoGet, MagicUse, MagicFill, ItemGet, SongGet, KeyGet, BossKeyGet, MapGet, MedalGet, HeartContainer, AmbLava, AmbDrip, AmbOrganic, AmbEerie, AmbThunder, AmbSand, AmbCrystal, Splash, Slam, Thrust, Flip, DoorOpen, BootsClunk, Lift, Throw, Put, Bottle, DinFire, Nayru, Farore, Perfect, Repelled, GuardBreak, Shatter, Stagger, Down, Struggle, Abyss, Thunder, Boomerang, BoomerangCatch, HookshotFire, NoteOn, Chime, SwordDraw, Sheathe, EquipSword, EquipShieldWood, EquipShieldMetal, EquipCloth, EquipHeavy, EquipHover, ItemTick, MenuOpen, MenuClose, MenuTick, MenuConfirm, MenuBack, PageFlip, TextTick, Sunrise, Nightfall, Ping, Puddle, HeatPulse, AirGasp, Telegraph, Alert, Appear, Stun, Blocked, NutFlash, StickSnap, ChuRoll, Footstep, SwingHeavy, BladeRing, Finisher, Sparkle, Arrive, Unlock, ProfileTick, TimeShift, TimerTick, TimeUp, StartShot, TargetHit, Miss, Fanfare, Catch, Bite, Cast, WaterRise, Collapse, Debris, ShipSink, Melt, Ignite, Dash, Dodge, WallKick, ChainJump, SlideLand, QuickTurn, Crash, Slip, Hammer, PickUp, Terror, Grabbed, Breakfree, ChainRattle, Detent, Nock, AimTick, Compass, ArmorBreak, WeakSpot, Projectile, Rage, Relief, Caught, SwordLost, MaskOn, BottleCatch, Gulp, Peck };
enum class Loop : int { Swim, Underwater, Rain, FallWind, Burn, Shock, Frozen, Squeeze, Strain, Grind, Slide, Wind, Hooves, Note, Crawl, Fuse, Motor, Warmth, Danger, Mist, Sink, Quake, Swarm, Flap, Rock, Ghost, Drag, Count };
enum class Mat : int { Default, Grass, Stone, Wood, Sand, Water, Ice, Flesh, Metal };

/// strength 0.1..1.5; n: repeat count / variant; pan -1 left .. 1 right
void Play(Fx fx, float strength = 1.0f, int n = 1, float pan = 0.0f);
/// While something important lands, the textures step back; Hush: near silence before a big moment
void Duck(int ms);
void Hush(int ms);

/// What the game feature sets each frame
struct Params {
    std::array<std::atomic<float>, static_cast<int>(Loop::Count)> loops{};
    std::atomic<float> swim_rate{0.8f}, hoof_rate{2.0f}, note_hz{200.0f}, loop_speed{0}, motor_up{0}, warm_pan{0};
    std::atomic<float> danger_pan{0}, mist_pan{0}, ghost_pan{0}, drag_pan{0}, hoof_pan{0};
    std::atomic<float> charge{0}, pull{-1}, low_boost{1}, fx_scale{1}, body_rate{1}, body_weight{1};
    std::atomic<bool> rain_roof{false}, muffle{false}, aiming{false}, rubber{false};
    std::atomic<int> material{0};
    std::atomic<double> hoof_phase{0}, swim_phase{0}; // cycles so far (the triggers can follow them)
    std::atomic<bool> active{false}; // a game feature drives it (else Render adds nothing)
};
Params& GetParams();

/// One sample (48 kHz) of the effects and textures for the left and right coil (audio thread)
void Render(float& l, float& r);

} // namespace Common::PulseHaptics
