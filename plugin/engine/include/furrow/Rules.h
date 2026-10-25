#pragma once

#include <array>
#include <cstdint>

// SeqCore's V1 limits, field numbers, defaults and ranges. Every value here is copied from
// jsfx/furrow_seqcore.jsfx. The golden tests replay SeqCore's output, so a value changed on
// one side only fails them.
namespace furrow
{

inline constexpr int lanes = 64;
inline constexpr int patterns = 256;
inline constexpr int maxSteps = 512;
inline constexpr int pageSteps = 16;
inline constexpr int pagesPerLane = maxSteps / pageSteps;
inline constexpr int stepFields = 16;
inline constexpr int pageValues = pageSteps * stepFields;
inline constexpr int poolPages = 16384;
inline constexpr int defaultLength = 16;
inline constexpr double stepBeats = 0.25;
// The most non-default step values a rack may hold, and the most commands in one transaction.
inline constexpr int nonDefaultMax = 824000;
inline constexpr int transactionMax = 4096;
inline constexpr int idMax = 16777215;

// Step fields. Shift and gate are in 1/10,000 of a step. Curve and the condition fields are
// reserved for V2: they are stored and saved, and V1 playback ignores them.
enum Field : int
{
    On, Velocity, Release, Pitch, Fine, Pan, ModA, ModB, Shift, Gate, Ratchet, Curve,
    Probability, Condition, ConditionA, ConditionB
};

inline constexpr std::array<int, stepFields> fieldDefault { 0, 100, 64, 0, 0, 0, 64, 64, 0, 5000, 1, 0, 100, 0, 0, 0 };
inline constexpr std::array<int, stepFields> fieldMin { 0, 1, 0, -24, -100, -64, 0, 0, 0, 500, 1, 0, 0, 0, 0, 0 };
inline constexpr std::array<int, stepFields> fieldMax { 1, 127, 127, 24, 100, 63, 127, 127, 9900, 160000, 8, 2, 100, 6, 64, 64 };

// Lane fields. Volume is 0 to 10,000; pan and swing are in 1/10,000. V1 fixes the rate to 1/16
// (1) and the direction to forward (0). Mod A and Mod B hold controller numbers.
enum LaneField : int
{
    Active, Id, ProbabilityKey, Row, Note, Mute, Solo, Volume, LanePan, BendRange, Swing, Rate,
    Direction, ModACc, ModBCc
};
inline constexpr int laneFields = 15;

inline constexpr std::array<int, laneFields> laneDefault { 0, 0, 0, 0, 60, 0, 0, 5000, 0, 2, 10000, 1, 0, 74, 71 };
inline constexpr std::array<int, laneFields> laneMin { 0, 1, 0, 0, 0, 0, 0, 0, -10000, 0, 0, 1, 0, 0, 0 };
inline constexpr std::array<int, laneFields> laneMax { 1, idMax, idMax, lanes - 1, 127, 1, 1, 10000, 10000, 48, 10000, 1, 0, 127, 127 };

// Rack settings. Mode 1 is the REAPER edition's Items mode, which plays nothing live. The
// plugin never selects it, but keeps the value so a saved state round-trips.
enum RackField : int
{
    RackId, Seed, CurrentPattern, Mode, GlobalSwing, Truncate
};

inline constexpr bool inRange (std::int64_t v, std::int64_t lo, std::int64_t hi) { return v >= lo && v <= hi; }

} // namespace furrow
