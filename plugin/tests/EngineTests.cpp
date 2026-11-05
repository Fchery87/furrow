#include "furrow/Engine.h"
#include "furrow/StateBytes.h"
#include "support/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

// Engine behaviour the golden traces cannot reach: the hash's published vectors, byte
// compatibility of saved state, limits that need thousands of commands, behaviour that exists
// only in the plugin, and the real-time rule that playback never allocates.

using namespace furrow;

namespace
{
struct Sink
{
    std::vector<MidiEvent> events;
    void operator() (const MidiEvent& e) { events.push_back (e); }
};

void discard (const MidiEvent&) {}

Result apply (Engine& engine, std::vector<Command> commands)
{
    return engine.apply (commands, discard);
}

std::vector<MidiEvent> process (Engine& engine, BlockInfo block)
{
    Sink sink;
    engine.process (block, sink);
    return sink.events;
}

std::vector<MidiEvent> notes (const std::vector<MidiEvent>& events)
{
    std::vector<MidiEvent> out;
    for (const auto& e : events)
        if (e.status == 0x80 || e.status == 0x90)
            out.push_back (e);
    return out;
}

// One lane on slot 0 playing note 60 on every step.
void everyStep (Engine& engine)
{
    std::vector<Command> commands { { Op::AddLane, 0, 1, 1, 60, 0 } };
    for (int step = 0; step < 16; ++step)
        commands.push_back ({ Op::SetStep, 0, 0, step, On, 1 });
    REQUIRE (apply (engine, commands) == Result::Ok);
}
} // namespace

TEST_CASE ("The probability hash matches the shared spec's test vectors")
{
    struct Vector
    {
        double seed, patternKey, laneKey, step;
        long long roll;
    };
    const Vector vectors[] = {
        { 0, 0, 0, 0, 59528724 }, { 0, 0, 0, 1, 30014588 }, { 1, 0, 0, 0, 10390278 }, { 0, 1, 0, 0, 63793128 },
        { 0, 0, 1, 0, 27661453 }, { 12345, 7, 999999, 123456, 58092903 }, { 16777215, 255, 16777215, 67108858, 58439087 },
    };
    for (const auto& v : vectors)
        CHECK (static_cast<long long> (probability::roll (v.seed, v.patternKey, v.laneKey, v.step)) == v.roll);
    CHECK (probability::plays (100, 0, 0, 0, 0));
    CHECK_FALSE (probability::plays (0, 0, 0, 0, 0));
}

TEST_CASE ("Saved state bytes are the REAPER edition's bytes")
{
    // SeqCore's @serialize output for an empty rack in ysfx: magic and format, then 13 zeros.
    Engine engine;
    std::vector<std::int32_t> words;
    engine.saveState (words);
    std::vector<std::uint8_t> bytes;
    state::toBytes (words, bytes);
    REQUIRE (bytes.size() == 60);
    const std::uint8_t head[] = { 0xe4, 0xea, 0x8c, 0x4a, 0x00, 0x00, 0x80, 0x3f };
    CHECK (std::equal (std::begin (head), std::end (head), bytes.begin()));
    CHECK (std::all_of (bytes.begin() + 8, bytes.end(), [] (std::uint8_t b) { return b == 0; }));

    std::vector<std::int32_t> back;
    REQUIRE (state::fromBytes (bytes, back));
    CHECK (back == words);
}

TEST_CASE ("State bytes round-trip every value a state holds and reject anything else")
{
    const std::vector<std::int32_t> words { 0, 1, -1, -24, 127, 160000, 65535, 16777215, -10000 };
    std::vector<std::uint8_t> bytes;
    state::toBytes (words, bytes);
    std::vector<std::int32_t> back;
    REQUIRE (state::fromBytes (bytes, back));
    CHECK (back == words);

    auto floatBytes = [] (float f) {
        std::vector<std::uint8_t> out;
        const auto bits = std::bit_cast<std::uint32_t> (f);
        for (int shift = 0; shift < 32; shift += 8)
            out.push_back (static_cast<std::uint8_t> (bits >> shift));
        return out;
    };
    CHECK_FALSE (state::fromBytes (std::vector<std::uint8_t> { 1, 2, 3 }, back));
    CHECK (back.empty());
    CHECK_FALSE (state::fromBytes (floatBytes (1.5f), back));
    CHECK_FALSE (state::fromBytes (floatBytes (std::numeric_limits<float>::quiet_NaN()), back));
    CHECK_FALSE (state::fromBytes (floatBytes (std::numeric_limits<float>::infinity()), back));
    CHECK_FALSE (state::fromBytes (floatBytes (3.0e9f), back));
}

TEST_CASE ("A state round-trips through save and load")
{
    Engine engine;
    std::vector<Command> commands { { Op::AddLane, 3, 9, 77, 40, 5 }, { Op::AddLane, 63, 10, 0, 127, 0 } };
    commands.push_back ({ Op::SetLane, 3, Volume, 9999 });
    commands.push_back ({ Op::SetLane, 63, LanePan, -10000 });
    commands.push_back ({ Op::SetRack, Seed, 16777215 });
    commands.push_back ({ Op::SetRack, GlobalSwing, 2500 });
    commands.push_back ({ Op::SetLength, 255, 63, 512 });
    for (int f = 0; f < stepFields; ++f)
        commands.push_back ({ Op::SetStep, 200, 3, 511 - f, f, fieldMax[static_cast<std::size_t> (f)] });
    REQUIRE (apply (engine, commands) == Result::Ok);
    // An origin that is not a whole beat, from a stopped switch at a fractional cursor.
    REQUIRE (apply (engine, { { Op::QueuePattern, 200, 0, 0, 0, 0, 0.0 } }) == Result::Ok);
    process (engine, { false, 3.141592653589793, 120, 48000, 256 });

    std::vector<std::int32_t> saved;
    engine.saveState (saved);
    Engine other;
    REQUIRE (other.loadState (saved) == LoadError::None);
    std::vector<std::int32_t> again;
    other.saveState (again);
    CHECK (again == saved);
    CHECK (other.store().get (200, 3, 511, On) == 1);
    CHECK (other.store().length (255, 63) == 512);
    CHECK (other.rackField (Seed) == 16777215);
    CHECK (other.pattern() == 200);
}

TEST_CASE ("Origin words must be 16-bit and give a finite beat")
{
    Engine engine;
    std::vector<std::int32_t> words;
    engine.saveState (words);
    auto bad = words;
    bad[8] = 65536;
    CHECK (engine.loadState (bad) == LoadError::Range);
    bad = words;
    bad[11] = 0x7FF0; // infinity
    CHECK (engine.loadState (bad) == LoadError::Range);
    CHECK (engine.loadState (words) == LoadError::None);
}

TEST_CASE ("Exhausting the page pool fails with Capacity and changes nothing")
{
    Engine engine;
    REQUIRE (apply (engine, { { Op::AddLane, 0, 1, 1, 60, 0 }, { Op::AddLane, 1, 2, 2, 60, 1 },
                              { Op::AddLane, 2, 3, 3, 60, 2 } }) == Result::Ok);
    // One value in each page: pattern, lane and page pick a fresh page every time. Three lanes
    // in every pattern have more pages than the pool.
    int page = 0;
    auto nextBatch = [&] (int count) {
        std::vector<Command> commands;
        for (int i = 0; i < count; ++i, ++page)
            commands.push_back ({ Op::SetStep, page / (3 * pagesPerLane), (page / pagesPerLane) % 3, (page % pagesPerLane) * pageSteps, On, 1 });
        return commands;
    };
    for (int i = 0; i < poolPages / transactionMax; ++i)
        REQUIRE (apply (engine, nextBatch (transactionMax)) == Result::Ok);
    CHECK (engine.store().freePages() == 0);
    std::vector<std::int32_t> before;
    engine.saveState (before);

    CHECK (apply (engine, nextBatch (1)) == Result::Capacity);
    // A value in a page that already exists still fits.
    CHECK (apply (engine, { { Op::SetStep, 0, 0, 1, On, 1 } }) == Result::Ok);
    CHECK (apply (engine, { { Op::SetStep, 0, 0, 1, On, 0 } }) == Result::Ok);
    std::vector<std::int32_t> after;
    engine.saveState (after);
    CHECK (after == before);
}

TEST_CASE ("The non-default value limit fails with Capacity")
{
    Engine engine;
    std::vector<Command> lanesUp;
    for (int s = 0; s < lanes; ++s)
        lanesUp.push_back ({ Op::AddLane, s, s + 1, s + 1, 60, s });
    REQUIRE (apply (engine, lanesUp) == Result::Ok);
    // Every field of a step non-default: 16 values in each step.
    auto nonDefault = [] (int field) { return fieldDefault[static_cast<std::size_t> (field)] == fieldMax[static_cast<std::size_t> (field)]
                                                  ? fieldMin[static_cast<std::size_t> (field)] : fieldMax[static_cast<std::size_t> (field)]; };
    long values = 0;
    std::vector<Command> batch;
    for (int key = 0; values < nonDefaultMax; ++key)
        for (int step = 0; step < maxSteps && values < nonDefaultMax; ++step)
            for (int f = 0; f < stepFields && values < nonDefaultMax; ++f, ++values)
            {
                batch.push_back ({ Op::SetStep, key / lanes, key % lanes, step, f, nonDefault (f) });
                if (batch.size() == static_cast<std::size_t> (transactionMax))
                {
                    REQUIRE (apply (engine, batch) == Result::Ok);
                    batch.clear();
                }
            }
    REQUIRE (apply (engine, batch) == Result::Ok);
    CHECK (engine.store().nonDefaultCount() == nonDefaultMax);
    CHECK (apply (engine, { { Op::SetStep, 255, 63, 0, Velocity, 5 } }) == Result::Capacity);
    // Putting a value back to its default is always allowed.
    CHECK (apply (engine, { { Op::SetStep, 0, 0, 0, Velocity, fieldDefault[Velocity] } }) == Result::Ok);
}

TEST_CASE ("A transaction longer than the limit fails with Capacity")
{
    Engine engine;
    everyStep (engine);
    std::vector<Command> commands (static_cast<std::size_t> (transactionMax) + 1, Command { Op::SetStep, 0, 0, 0, Velocity, 50 });
    CHECK (apply (engine, commands) == Result::Capacity);
    CHECK (engine.store().get (0, 0, 0, Velocity) == fieldDefault[Velocity]);
}

TEST_CASE ("The first call sends all-notes-off on every lane")
{
    Engine engine;
    Sink sink;
    engine.process ({ false, 0.0, 120, 48000, 64 }, sink);
    REQUIRE (sink.events.size() == lanes);
    for (int s = 0; s < lanes; ++s)
    {
        CHECK (sink.events[static_cast<std::size_t> (s)].lane == s);
        CHECK (sink.events[static_cast<std::size_t> (s)].status == 0xB0);
        CHECK (sink.events[static_cast<std::size_t> (s)].data1 == 123);
    }
    CHECK (process (engine, { false, 0.0, 120, 48000, 64 }).empty());
}

TEST_CASE ("Without a live hold, the first block plays its first step")
{
    Engine engine;
    everyStep (engine);
    const auto events = process (engine, { true, 0.0, 120, 48000, 64 });
    // Lane 0's volume and pan, then its note, at offset 0.
    REQUIRE (events.size() == 3);
    CHECK (events[2].status == 0x90);
    CHECK (events[2].offset == 0);
    CHECK (events[2].data1 == 60);
}

TEST_CASE ("An empty block is ignored and does not count as a seek")
{
    Engine engine;
    everyStep (engine);
    process (engine, { true, 0.0, 120, 48000, 480 });
    CHECK (process (engine, { true, 0.01, 120, 48000, 0 }).empty());
    // 480 samples at 120 BPM and 48 kHz is 0.02 beats. Step 0's note is still held, so a false
    // seek would release it here.
    CHECK (notes (process (engine, { true, 0.02, 120, 48000, 480 })).empty());
}

TEST_CASE ("A block without a usable tempo or position counts as stopped")
{
    Engine engine;
    everyStep (engine);
    process (engine, { true, 0.0, 120, 48000, 64 });
    const auto events = notes (process (engine, { true, 0.001, 0.0, 48000, 64 }));
    REQUIRE (events.size() == 1);
    CHECK (events[0].status == 0x80);
    CHECK_FALSE (engine.playing());
    CHECK (notes (process (engine, { true, std::numeric_limits<double>::quiet_NaN(), 120, 48000, 64 })).empty());
    CHECK_FALSE (engine.playing());
}

TEST_CASE ("A host reset releases held notes at the start of the next block")
{
    Engine engine;
    everyStep (engine);
    process (engine, { true, 0.0, 120, 48000, 64 });
    engine.reset();
    const auto events = process (engine, { true, 0.001333, 120, 48000, 64 });
    REQUIRE (! events.empty());
    CHECK (events[0].status == 0x80);
    CHECK (events[0].offset == 0);
}

TEST_CASE ("Playback, edits, audition and panic never allocate")
{
    Engine engine;
    std::vector<Command> setup;
    for (int s = 0; s < lanes; ++s)
        setup.push_back ({ Op::AddLane, s, s + 1, s * 7919, 36 + s, s });
    for (int s = 0; s < lanes; ++s)
        for (int step = 0; step < 16; ++step)
        {
            setup.push_back ({ Op::SetStep, 0, s, step, On, 1 });
            setup.push_back ({ Op::SetStep, 0, s, step, Ratchet, 1 + (step + s) % 8 });
            setup.push_back ({ Op::SetStep, 1, s, step, On, (step + s) % 3 == 0 ? 1 : 0 });
        }
    setup.push_back ({ Op::SetRack, GlobalSwing, 10000 });
    REQUIRE (apply (engine, setup) == Result::Ok);
    std::vector<Command> edits { { Op::SetStep, 0, 5, 3, Probability, 50 }, { Op::SetLane, 7, Mute, 1 },
                                 { Op::QueuePattern, 1, 0, 0, 0, 0, 8.0 }, { Op::SetLength, 0, 9, 7 } };
    std::vector<Command> undo { { Op::SetStep, 0, 5, 3, Probability, 100 }, { Op::SetLane, 7, Mute, 0 },
                                { Op::QueuePattern, 0, 0, 0, 0, 0, 16.0 } };

    std::array<MidiEvent, 65536> buffer {};
    std::size_t used = 0;
    long sent = 0;
    auto emit = [&] (const MidiEvent& e) {
        buffer[used++ % buffer.size()] = e;
        ++sent;
    };

    double beat = 0.0;
    {
        test::CountAllocations count;
        for (int block = 0; block < 4000; ++block)
        {
            const int frames = 1 + (block * 37) % 1024;
            if (block == 100)
                engine.apply (edits, emit);
            if (block == 900)
                engine.apply (undo, emit);
            if (block == 2000)
                engine.panic (emit);
            const bool playing = block < 3000;
            engine.process ({ playing, beat, 133.0, 44100, frames }, emit);
            if (playing)
                beat += 133.0 / 60.0 * frames / 44100;
            if (block == 3100)
                engine.audition (3, 100, emit);
        }
    }
    CHECK (test::allocations == 0);
    CHECK (sent > 1000);
}
