#include "furrow/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <compare>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using furrow::BlockInfo;
using furrow::Command;
using furrow::Engine;
using furrow::MidiEvent;
using furrow::Op;

namespace
{
// One event at an absolute sample, in the columns probes/expected/*.txt uses:
// sample, order (0 for off, 1 for on), lane, channel, note, velocity. Rows sort by those columns.
struct Row
{
    std::tuple<std::int64_t, int, int, int, int, int> fields;
    auto operator<=> (const Row&) const = default;
};

// Lane n plays on channel n here, as in the captures. The capture logger and the expected
// schedules record a note-off's velocity as 0.
Row rowOf (std::int64_t blockStart, const MidiEvent& e)
{
    const bool on = e.status == 0x90;
    return { { blockStart + e.offset, on ? 1 : 0, e.lane, e.lane, e.data1, on ? e.data2 : 0 } };
}

// The pattern in probes/pattern-4x16.txt: four lanes of 16 steps, gate half a step, velocity 100.
void loadProbePattern (Engine& engine)
{
    const int notes[] = { 36, 38, 42, 46 };
    const char* grid[] = { "1000100010001000", "0010001000100010", "1010101010101010", "0000100000001001" };
    std::vector<Command> commands;
    for (int lane = 0; lane < 4; ++lane)
        commands.push_back ({ Op::AddLane, lane, lane + 1, lane + 1, notes[lane], lane });
    for (int lane = 0; lane < 4; ++lane)
        for (int step = 0; step < 16; ++step)
            if (grid[lane][step] == '1')
                commands.push_back ({ Op::SetStep, 0, lane, step, furrow::On, 1 });
    REQUIRE (engine.apply (commands, [] (const MidiEvent&) {}) == furrow::Result::Ok);
}

// Reads an event file. Expected schedules and REAPER captures order their middle
// columns differently, so only the columns they share are read: sample first, then channel, note,
// velocity and kind last. In this fixture each lane plays on the channel with its own number.
std::vector<Row> readEvents (const std::string& path)
{
    std::ifstream file (path);
    REQUIRE (file.good());
    std::vector<Row> rows;
    std::string line;
    while (std::getline (file, line))
    {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in (line);
        std::int64_t sample;
        int skip, channel, note, velocity;
        std::string kind;
        in >> sample >> skip >> skip >> skip >> skip >> channel >> note >> velocity >> kind;
        REQUIRE ((kind == "on" || kind == "off"));
        rows.push_back ({ { sample, kind == "on" ? 1 : 0, channel, channel, note, velocity } });
    }
    std::sort (rows.begin(), rows.end());
    return rows;
}

// True when both lists hold the same events in the same order for each lane and kind, and
// `samplesAgree` accepts the sample positions of every pair.
template <typename Agree>
bool pairsMatch (const std::vector<Row>& a, const std::vector<Row>& b, Agree&& samplesAgree)
{
    auto byLane = [] (std::vector<Row> rows) {
        std::stable_sort (rows.begin(), rows.end(), [] (const Row& x, const Row& y) {
            const auto& [xs, xo, xl, xc, xn, xv] = x.fields;
            const auto& [ys, yo, yl, yc, yn, yv] = y.fields;
            return std::tie (xl, xo, xs) < std::tie (yl, yo, ys);
        });
        return rows;
    };
    const auto x = byLane (a);
    const auto y = byLane (b);
    if (x.size() != y.size())
        return false;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto& [xs, xo, xl, xc, xn, xv] = x[i].fields;
        const auto& [ys, yo, yl, yc, yn, yv] = y[i].fields;
        if (std::tie (xo, xl, xc, xn, xv) != std::tie (yo, yl, yc, yn, yv))
            return false;
        if (! samplesAgree (xs, ys))
        {
            UNSCOPED_INFO ("lane " << xl << (xo ? " note-on" : " note-off") << " at " << xs << " against " << ys);
            return false;
        }
    }
    return true;
}

bool withinSamples (const std::vector<Row>& a, const std::vector<Row>& b, std::int64_t tolerance)
{
    return pairsMatch (a, b, [=] (std::int64_t x, std::int64_t y) { return std::abs (x - y) <= tolerance; });
}

// Exact agreement, except where the true time lies exactly halfway between two samples. There the
// rounding direction depends on the last bits of the host's beat position, so either neighbour is
// correct. Every hit and release in this pattern falls on a multiple of an eighth of a beat, so a
// one-sample difference is accepted only when its midpoint is such a position.
bool sameOrHalfSampleTie (const std::vector<Row>& a, const std::vector<Row>& b, double bpm, double sampleRate)
{
    return pairsMatch (a, b, [=] (std::int64_t x, std::int64_t y) {
        if (x == y)
            return true;
        if (std::abs (x - y) != 1)
            return false;
        const double eighths = (static_cast<double> (x + y) / 2.0) * bpm / (60.0 * sampleRate) * 8.0;
        return std::abs (eighths - std::round (eighths)) < 1.0e-9;
    });
}

// Drives the engine the way a host would at a constant tempo from sample 0, and collects every
// note event before `endSample`.
std::vector<Row> play (double bpm, double sampleRate, int blockSize, std::int64_t endSample)
{
    Engine engine;
    loadProbePattern (engine);
    std::vector<Row> rows;
    const double samplesPerBeat = 60.0 * sampleRate / bpm;
    for (std::int64_t start = 0; start < endSample; start += blockSize)
    {
        const BlockInfo block { true, static_cast<double> (start) / samplesPerBeat, bpm, sampleRate, blockSize };
        engine.process (block, [&] (const MidiEvent& e) {
            if ((e.status == 0x80 || e.status == 0x90) && start + e.offset < endSample)
                rows.push_back (rowOf (start, e));
        });
    }
    std::sort (rows.begin(), rows.end());
    return rows;
}

} // namespace

TEST_CASE ("The engine plays exactly what SeqCore played in REAPER")
{
    // The 18 October 2026 audit logged SeqCore's MIDI in REAPER 7.82 for Linux with the current
    // receiver logger, for one pass of this pattern at every sample rate, block size and tempo
    // below. Each capture passed the comparator. This engine must send the same events at the
    // same samples, apart from exact half-sample ties. The audit logged REAPER's beat positions to
    // 12 decimal places, which is not enough to replay the bits that decide a tie.
    const double sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const int blockSize = GENERATE (64, 256, 1024);
    const int bpm = GENERATE (60, 120, 200);
    const std::string name = "constant-" + std::to_string (static_cast<int> (sampleRate)) + "-"
                           + std::to_string (blockSize) + "-" + std::to_string (bpm);
    INFO (name);

    const auto seqcore = readEvents (FURROW_REPO_ROOT "/.audit/results/2026-10-18/fresh-timing/" + name + "/capture-events.txt");
    REQUIRE (seqcore.size() == 38);
    const auto pass = static_cast<std::int64_t> (std::llround (4.0 * 60.0 * sampleRate / bpm));
    CHECK (sameOrHalfSampleTie (play (bpm, sampleRate, blockSize, pass), seqcore, bpm, sampleRate));
}

TEST_CASE ("The engine stays within one sample of the reference schedule at any block size")
{
    // One pass at 120 BPM and 44.1 kHz: 16 steps, 4 beats, 88,200 samples. probes/schedule.py
    // works in absolute time and rounds half to even, so it puts step 15 at 82,688. The spec's
    // timing target is one sample.
    const auto reference = readEvents (FURROW_REPO_ROOT "/probes/expected/constant-120.txt");
    REQUIRE (reference.size() == 38);

    const int blockSize = GENERATE (1024, 256, 64, 1000, 4096, 1);
    INFO ("block size " << blockSize);
    CHECK (withinSamples (play (120.0, 44100.0, blockSize, 88200), reference, 1));
}
