#pragma once

#include <cmath>

// The shared probability hash. It uses only double arithmetic on integers below 2^53, so
// SeqCore, the Lua port and this one give the same bits. The modulus is the prime 2^26 - 5.
namespace furrow::probability
{

inline constexpr double m = 67108859.0;
inline constexpr double a = 40503817.0;
inline constexpr double b = 60251237.0;
inline constexpr double c = 9301841.0;

inline double mod (double x)
{
    const double q = std::floor (x / m);
    double r = x - q * m;
    if (r < 0)
        r += m;
    if (r >= m)
        r -= m;
    return r;
}

inline double mix (double h, double x)
{
    h = mod (h + x + 1);
    h = mod (h * a);
    h = mod (h * h + c);
    return mod (h * b);
}

// A value from 0 to m - 1 for a seed, pattern key, lane key and project step number.
inline double roll (double seed, double patternKey, double laneKey, double step)
{
    double h = mix (0, seed);
    h = mix (h, patternKey);
    h = mix (h, laneKey);
    h = mix (h, mod (step));
    return mod (h * h + c);
}

// A step with probability p percent plays when its roll is below p percent of m.
inline bool plays (int percent, double seed, double patternKey, double laneKey, double step)
{
    if (percent >= 100)
        return true;
    if (percent <= 0)
        return false;
    return roll (seed, patternKey, laneKey, step) * 100 < percent * m;
}

} // namespace furrow::probability
