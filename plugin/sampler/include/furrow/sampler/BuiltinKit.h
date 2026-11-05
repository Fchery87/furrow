#pragma once

#include "furrow/sampler/SampleBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <vector>

// Four drum sounds made in code, so a new instance makes sound without shipping sample files.
// They are synthesised the same way every time, from fixed formulas and a fixed noise seed, and
// they are part of the plugin's source, so no separate sample licence applies.
namespace furrow::sampler
{

enum class Builtin : std::uint8_t
{
    Kick = 0,
    Snare = 1,
    ClosedHat = 2,
    OpenHat = 3,
};
inline constexpr int builtinCount = 4;

inline const char* builtinName (Builtin kind)
{
    switch (kind)
    {
        case Builtin::Kick: return "Kick";
        case Builtin::Snare: return "Snare";
        case Builtin::ClosedHat: return "Closed hat";
        case Builtin::OpenHat: return "Open hat";
    }
    return "";
}

namespace detail
{
inline constexpr double kitRate = 48000.0;

// White noise from a fixed linear congruential generator, from -1 to 1.
struct Noise
{
    std::uint32_t state = 0x2545F491u;
    double next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<double> (state >> 8) / 8388608.0 - 1.0;
    }
};

// Scales to the given peak, then fades the last 5% to silence so the end makes no click.
inline std::unique_ptr<SampleBuffer> finish (std::vector<double>& x, double peak)
{
    double top = 0.0;
    for (const double v : x)
        top = std::max (top, std::abs (v));
    const std::size_t fade = x.size() / 20;
    std::vector<float> out (x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        double v = top > 0.0 ? x[i] * peak / top : 0.0;
        const std::size_t left = x.size() - 1 - i;
        if (left < fade)
            v *= static_cast<double> (left) / static_cast<double> (fade);
        out[i] = static_cast<float> (v);
    }
    const float* channels[] = { out.data() };
    return SampleBuffer::make (channels, static_cast<std::int64_t> (out.size()), kitRate);
}

// Noise with its low end removed by two first differences, which leaves the hiss of a cymbal.
inline std::vector<double> hiss (std::size_t frames, double decaySeconds)
{
    Noise noise;
    std::vector<double> x (frames);
    double a = 0.0, b = 0.0;
    for (std::size_t i = 0; i < frames; ++i)
    {
        const double n = noise.next();
        const double d1 = n - a;
        const double d2 = d1 - b;
        a = n;
        b = d1;
        x[i] = d2 * std::exp (-static_cast<double> (i) / kitRate / decaySeconds);
    }
    return x;
}
} // namespace detail

// Makes one of the built-in sounds: mono, at 48 kHz.
inline std::unique_ptr<SampleBuffer> makeBuiltin (Builtin kind)
{
    using detail::kitRate;
    constexpr double twoPi = 2.0 * std::numbers::pi;
    switch (kind)
    {
        case Builtin::Kick:
        {
            // A sine that falls from 155 Hz to 45 Hz, with a short click on top.
            std::vector<double> x (static_cast<std::size_t> (0.5 * kitRate));
            detail::Noise noise;
            double phase = 0.0;
            for (std::size_t i = 0; i < x.size(); ++i)
            {
                const double t = static_cast<double> (i) / kitRate;
                const double hz = 45.0 + 110.0 * std::exp (-t / 0.035);
                x[i] = std::sin (phase) * std::exp (-t / 0.16) + 0.25 * noise.next() * std::exp (-t / 0.0015);
                phase += twoPi * hz / kitRate;
            }
            return detail::finish (x, 0.85);
        }
        case Builtin::Snare:
        {
            // Two drum-head tones under a burst of hiss.
            std::vector<double> x = detail::hiss (static_cast<std::size_t> (0.35 * kitRate), 0.1);
            for (std::size_t i = 0; i < x.size(); ++i)
            {
                const double t = static_cast<double> (i) / kitRate;
                x[i] = 0.5 * x[i] + 0.6 * std::sin (twoPi * 180.0 * t) * std::exp (-t / 0.06)
                     + 0.3 * std::sin (twoPi * 330.0 * t) * std::exp (-t / 0.04);
            }
            return detail::finish (x, 0.85);
        }
        case Builtin::ClosedHat:
        {
            auto x = detail::hiss (static_cast<std::size_t> (0.15 * kitRate), 0.02);
            return detail::finish (x, 0.6);
        }
        case Builtin::OpenHat:
        {
            auto x = detail::hiss (static_cast<std::size_t> (0.7 * kitRate), 0.16);
            return detail::finish (x, 0.6);
        }
    }
    return nullptr;
}

} // namespace furrow::sampler
