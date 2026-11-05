#pragma once

#include <algorithm>
#include <cmath>

// How a lane's messages set its level, balance and pitch. Each curve copies RS5K 7.82 as the
// REAPER edition sets it up, measured in probes/results/reaper-7.82-linux/rs5k-v3-results.txt,
// so a rack sounds the same in both editions.
namespace furrow::sampler
{

// RS5K's Gain for minimum velocity, as the REAPER edition sets it. A product choice there.
inline constexpr double velocityFloorDb = -30.0;

// Amplitude for a note-on velocity from 1 to 127: a straight line from the floor at 1 to full
// level at 127. With the floor at -inf, RS5K gave exactly (velocity - 1) / 126.
inline double velocityGain (int velocity)
{
    const double floor = std::pow (10.0, velocityFloorDb / 20.0);
    const int v = std::clamp (velocity, 1, 127);
    return floor + (1.0 - floor) * (v - 1) / 126.0;
}

// Amplitude for CC 7, linked to RS5K's Volume: silence at 0 and +6.02 dB at 127.
inline double volumeGain (int value) { return 2.0 * std::clamp (value, 0, 127) / 127.0; }

struct PanGains
{
    double left = 1.0;
    double right = 1.0;
};

// Channel gains for CC 10, linked to RS5K's Pan. It is a balance control: both sides are at full
// level in the centre, and moving away from it lowers only the far side.
inline PanGains panGains (int value)
{
    const double p = std::clamp (value, 0, 127) / 127.0;
    return { std::min (1.0, 2.0 * (1.0 - p)), std::min (1.0, 2.0 * p) };
}

// Semitones for a 14-bit pitch bend value, centre 8192, over the lane's bend range.
inline double bendSemitones (int value, int range) { return (std::clamp (value, 0, 16383) - 8192) / 8192.0 * range; }

} // namespace furrow::sampler
