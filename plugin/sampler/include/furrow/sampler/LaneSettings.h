#pragma once

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

// Where a lane's notes go: out of the plugin as MIDI, or to its sample inside the plugin.
enum class Target : std::uint8_t
{
    Midi = 0,
    Sample = 1,
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

    bool operator== (const LaneSettings&) const = default;
};

inline bool valid (const LaneSettings& s)
{
    auto within = [] (double v, double lo, double hi) { return std::isfinite (v) && v >= lo && v <= hi; };
    return (s.target == Target::Midi || s.target == Target::Sample) && s.channel >= 1 && s.channel <= 16
        && s.output >= 1 && s.output <= outputs && within (s.start, 0.0, 1.0) && within (s.end, 0.0, 1.0) && s.start < s.end
        && s.tune >= -tuneMax && s.tune <= tuneMax && within (s.attack, 0.0, attackMax) && within (s.decay, 0.0, decayMax)
        && within (s.release, 0.0, releaseMax) && s.choke >= 0 && s.choke <= chokeGroups;
}

} // namespace furrow::sampler
