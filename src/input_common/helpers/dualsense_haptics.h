// SPDX-FileCopyrightText: Copyright 2026 Fritchii Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace InputCommon {

/// The game's HD rumble on a wired DualSense's voice coils (Windows). The controller's USB sound
/// device has 4 channels; channels 3 and 4 drive the left and right actuators (as Pulse of Time and
/// Longshot / RenHDrum use them). Each side plays what HD rumble is: a low and a high band, each a
/// sine at the game's frequency and amplitude, phase-continuous across updates with the amplitude
/// ramped, so the controller plays the game's own vibration instead of two motors' approximation. Levels follow
/// RenHDrum's (github.com/trzz1214/RenHDrum, MIT OR Apache-2.0) Switch 2 law and 0.8 output ceiling.
/// FRITCHII_DUALSENSE_HAPTICS=0 turns it off; FRITCHII_DUALSENSE_HAPTICS_GAIN scales it (default 1).
class DualSenseHaptics {
public:
    static DualSenseHaptics& Instance();

    /// Starts looking for the controller's sound device (once). False when turned off or not Windows.
    bool Start();

    /// True while the sound device plays.
    bool Running() const;

    /// side: 0 left, 1 right, anything else both. Amplitudes 0..1, frequencies in Hz.
    void Set(int side, float low_amplitude, float low_frequency, float high_amplitude,
             float high_frequency);
};

} // namespace InputCommon
