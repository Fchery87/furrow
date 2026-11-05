#include "furrow/Engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

// Replays every trace in plugin/tests/golden. Each was recorded from SeqCore running in ysfx
// (see make_golden.py): the commands, loads and host blocks it was given, and every MIDI event,
// transaction result, playhead and saved word it produced. The engine gets the same input and
// must give the same output, event for event and sample for sample.

using namespace furrow;

namespace
{
// One event as SeqCore routes it: lane n on MIDI bus n / 16, channel n % 16.
using Wire = std::tuple<int, int, int, int, int>; // offset, bus, status, data1, data2

Wire wireOf (const MidiEvent& e)
{
    return { e.offset, e.lane / 16, e.status | (e.lane % 16), e.data1, e.data2 };
}

std::string show (const std::vector<Wire>& events)
{
    std::ostringstream out;
    for (const auto& [offset, bus, status, d1, d2] : events)
        out << "  " << offset << " " << bus << " " << status << " " << d1 << " " << d2 << "\n";
    return out.str();
}

std::vector<std::string> traceNames()
{
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator (FURROW_REPO_ROOT "/plugin/tests/golden"))
        if (entry.path().extension() == ".txt")
            names.push_back (entry.path().stem().string());
    std::sort (names.begin(), names.end());
    return names;
}

struct Expected
{
    bool seen = false;
    int pattern = 0;
    int pending = -1;
    std::vector<int> playheads;
};
} // namespace

TEST_CASE ("The engine replays every SeqCore golden trace exactly")
{
    static const auto names = traceNames();
    REQUIRE (names.size() >= 17);
    const auto name = GENERATE (from_range (names));
    INFO ("trace " << name);

    std::ifstream file (FURROW_REPO_ROOT "/plugin/tests/golden/" + name + ".txt");
    REQUIRE (file.good());

    Engine engine (2);
    double sampleRate = 44100.0;
    std::vector<Command> staged;
    Result lastResult = Result::Ok;
    std::vector<Wire> pending, got, want;
    Expected expected;
    bool inBlock = false;
    int blockLine = 0;
    int mismatches = 0;
    int blocks = 0;
    int events = 0;
    int blockFrames = 0;
    int disordered = 0;

    auto toPending = [&] (const MidiEvent& e) { pending.push_back (wireOf (e)); };
    auto toGot = [&] (const MidiEvent& e) { got.push_back (wireOf (e)); };

    auto finishBlock = [&] {
        if (! inBlock)
            return;
        inBlock = false;
        ++blocks;
        events += static_cast<int> (want.size());
        // The plugin renders samples up to each event as the engine emits it, so a block's events
        // must come in time order and inside the block.
        for (std::size_t i = 0; i < got.size(); ++i)
        {
            const int offset = std::get<0> (got[i]);
            const bool inside = offset >= 0 && (offset < blockFrames || offset == 0);
            if ((! inside || (i > 0 && offset < std::get<0> (got[i - 1]))) && ++disordered <= 3)
                FAIL_CHECK ("block at line " << blockLine << " sent an event out of order or outside the block\n" << show (got));
        }
        bool same = got == want;
        if (same && expected.seen)
        {
            same = engine.pattern() == expected.pattern && engine.pendingPattern() == expected.pending;
            for (std::size_t s = 0; same && s < expected.playheads.size(); ++s)
                same = engine.playhead (static_cast<int> (s)) == expected.playheads[s];
        }
        if (! same && ++mismatches <= 3)
        {
            std::ostringstream heads;
            for (std::size_t s = 0; s < expected.playheads.size(); ++s)
                heads << engine.playhead (static_cast<int> (s)) << "/" << expected.playheads[s] << " ";
            FAIL_CHECK ("block at line " << blockLine << " differs\nengine sent:\n" << show (got) << "SeqCore sent:\n"
                                         << show (want) << "pattern " << engine.pattern() << "/" << expected.pattern
                                         << " pending " << engine.pendingPattern() << "/" << expected.pending
                                         << " playheads (engine/SeqCore) " << heads.str());
        }
    };

    std::string line;
    int lineNo = 0;
    while (std::getline (file, line))
    {
        ++lineNo;
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream in (line);
        std::string tag;
        in >> tag;

        if (tag == "E")
        {
            int offset, bus, status, d1, d2;
            in >> offset >> bus >> status >> d1 >> d2;
            want.emplace_back (offset, bus, status, d1, d2);
            continue;
        }
        if (tag == "P")
        {
            expected.seen = true;
            expected.playheads.clear();
            in >> expected.pattern >> expected.pending;
            for (int p; in >> p;)
                expected.playheads.push_back (p);
            continue;
        }
        finishBlock();

        if (tag == "rate")
        {
            int block;
            in >> sampleRate >> block;
            engine.prepare (sampleRate);
        }
        else if (tag == "C")
        {
            int op;
            Command c;
            in >> op >> c.a >> c.b >> c.c >> c.d >> c.v >> c.beat;
            c.op = static_cast<Op> (op);
            staged.push_back (c);
        }
        else if (tag == "T")
        {
            lastResult = engine.apply (staged, toPending);
            staged.clear();
        }
        else if (tag == "A")
        {
            int code;
            in >> code;
            INFO ("line " << lineNo);
            CHECK (static_cast<int> (lastResult) == code);
        }
        else if (tag == "I")
        {
            int op, a, b;
            in >> op >> a >> b;
            if (op == 8)
                engine.audition (a, b, toPending);
            else
                engine.panic (toPending);
        }
        else if (tag == "L")
        {
            std::vector<std::int32_t> words;
            for (std::int32_t w; in >> w;)
                words.push_back (w);
            engine.loadState (words);
        }
        else if (tag == "B")
        {
            int playing, frames;
            double tempo, beat;
            in >> playing >> tempo >> beat >> frames;
            got = pending;
            pending.clear();
            want.clear();
            engine.process (BlockInfo { playing != 0, beat, tempo, sampleRate, frames }, toGot);
            blockFrames = frames;
            inBlock = true;
            blockLine = lineNo;
        }
        else if (tag == "S")
        {
            std::vector<std::int32_t> words, saved;
            for (std::int32_t w; in >> w;)
                words.push_back (w);
            engine.saveState (saved);
            CHECK (saved == words);
        }
        else
        {
            FAIL ("unknown record at line " << lineNo << ": " << line);
        }
    }
    finishBlock();
    INFO (blocks << " blocks, " << events << " events");
    CHECK (mismatches == 0);
    CHECK (blocks > 0);
}
