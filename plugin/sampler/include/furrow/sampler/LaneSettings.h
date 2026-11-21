#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace furrow::sampler
{

inline constexpr int outputs = 16;      // stereo output buses: the main one and 15 more
inline constexpr int chokeGroups = 16;
inline constexpr int tuneMax = 4800;    // cents
inline constexpr double attackMax = 10000.0;
inline constexpr double decayMax = 60000.0;
inline constexpr double releaseMax = 10000.0;
inline constexpr double filterMinHz = 20.0;
inline constexpr double filterMaxHz = 20000.0;
inline constexpr double filterResMax = 0.95;

// Where a lane's notes go: out of the plugin as MIDI, or to its sample inside the plugin.
enum class Target : std::uint8_t
{
    Midi = 0,
    Sample = 1,
};

// Gate ends a sample voice on note-off (RS5K default); OneShot plays to end/decay/choke.
enum class PlayMode : std::uint8_t
{
    Gate = 0,
    OneShot = 1,
};

enum class FilterMode : std::uint8_t
{
    Off = 0,
    LowPass = 1,
    HighPass = 2,
};

// What the plugin adds to an engine lane. The engine's own lane fields (note, volume, pan, bend
// range and the rest) stay in the engine's state.
struct LaneSettings
{
    Target target = Target::Midi;
    int channel = 1;            // MIDI channel 1 to 16, for a MIDI lane
    int output = 1;             // output bus 1 to 16, for a sample lane
    double start = 0.0;         // play from here to end, as fractions of the sample's length
    double end = 1.0;
    int tune = 0;               // cents, added to the note's pitch
    // Envelope times in milliseconds. Attack rises in a straight line. Decay falls to -60 dB
    // while the note is held, and 0 turns it off. Release falls to -60 dB after the note-off.
    // A 1 ms attack and release are RS5K's defaults.
    double attack = 1.0;
    double decay = 0.0;
    double release = 1.0;
    int choke = 0;              // 0 for none, or a group from 1 to 16
    PlayMode playMode = PlayMode::Gate;
    bool reverse = false;
    FilterMode filterMode = FilterMode::Off;
    double filterCutoffHz = filterMaxHz;
    double filterResonance = 0.0;

    bool operator== (const LaneSettings& o) const
    {
        auto eq = [] (double a, double b) { return std::bit_cast<std::uint64_t> (a) == std::bit_cast<std::uint64_t> (b); };
        return target == o.target && channel == o.channel && output == o.output
            && eq (start, o.start) && eq (end, o.end) && tune == o.tune
            && eq (attack, o.attack) && eq (decay, o.decay) && eq (release, o.release)
            && choke == o.choke && playMode == o.playMode && reverse == o.reverse
            && filterMode == o.filterMode && eq (filterCutoffHz, o.filterCutoffHz) && eq (filterResonance, o.filterResonance);
    }
};

inline bool valid (const LaneSettings& s)
{
    auto within = [] (double v, double lo, double hi) { return std::isfinite (v) && v >= lo && v <= hi; };
    return (s.target == Target::Midi || s.target == Target::Sample) && s.channel >= 1 && s.channel <= 16
        && s.output >= 1 && s.output <= outputs && within (s.start, 0.0, 1.0) && within (s.end, 0.0, 1.0) && s.start < s.end
        && s.tune >= -tuneMax && s.tune <= tuneMax && within (s.attack, 0.0, attackMax) && within (s.decay, 0.0, decayMax)
        && within (s.release, 0.0, releaseMax) && s.choke >= 0 && s.choke <= chokeGroups
        && (s.playMode == PlayMode::Gate || s.playMode == PlayMode::OneShot)
        && (s.filterMode == FilterMode::Off || s.filterMode == FilterMode::LowPass || s.filterMode == FilterMode::HighPass)
        && within (s.filterCutoffHz, filterMinHz, filterMaxHz) && within (s.filterResonance, 0.0, filterResMax);
}

} // namespace furrow::sampler
