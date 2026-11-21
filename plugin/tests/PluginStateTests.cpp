#include "furrow/Engine.h"
#include "furrow/PluginState.h"
#include "furrow/sampler/LaneSetup.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <vector>

// The plugin's saved state: the engine's words, which stay SeqCore's bytes, plus each lane's
// sampler setup.

using namespace furrow;
using namespace furrow::sampler;

namespace
{
std::vector<LaneSetup> someSetups()
{
    LaneSetup kick;
    kick.slot = 0;
    kick.id = 1;
    kick.settings.target = Target::Sample;
    kick.settings.output = 3;
    kick.settings.start = 0.125;
    kick.settings.end = 0.75;
    kick.settings.tune = -250;
    kick.settings.attack = 0.0;
    kick.settings.decay = 300.5;
    kick.settings.release = 12.25;
    kick.settings.choke = 2;
    kick.sample.kind = SampleRef::Kind::Builtin;
    kick.sample.builtin = Builtin::Kick;

    LaneSetup file;
    file.slot = 9;
    file.id = idMax;
    file.settings.target = Target::Sample;
    file.sample.kind = SampleRef::Kind::File;
    file.sample.path = "/home/someone/Samples/Breaks/ámen brother.flac";
    for (std::size_t i = 0; i < file.sample.hash.size(); ++i)
        file.sample.hash[i] = static_cast<std::uint8_t> (i * 7 + 1);

    LaneSetup midi;
    midi.slot = 63;
    midi.id = 77;
    midi.settings.channel = 10;
    return { kick, file, midi };
}

std::vector<std::int32_t> engineWords()
{
    Engine engine;
    std::vector<Command> commands { { Op::AddLane, 0, 1, 1, 36, 0 }, { Op::SetStep, 0, 0, 3, On, 1 } };
    engine.apply (commands, [] (const MidiEvent&) {});
    std::vector<std::int32_t> words;
    engine.saveState (words);
    return words;
}
} // namespace

TEST_CASE ("Lane setups round-trip, including file paths and hashes")
{
    const auto setups = someSetups();
    std::vector<std::uint8_t> bytes;
    encodeSetups (setups, bytes);
    std::vector<LaneSetup> back;
    REQUIRE (decodeSetups (bytes, back));
    CHECK (back == setups);

    bytes.clear();
    encodeSetups ({}, bytes);
    REQUIRE (decodeSetups (bytes, back));
    CHECK (back.empty());
}

TEST_CASE ("Lane setups refuse truncated, padded or out-of-range data")
{
    const auto setups = someSetups();
    std::vector<std::uint8_t> bytes;
    encodeSetups (setups, bytes);
    std::vector<LaneSetup> back;

    // Every truncation, and one byte too many.
    for (std::size_t n = 0; n < bytes.size(); ++n)
    {
        INFO ("length " << n);
        CHECK_FALSE (decodeSetups (std::span (bytes).first (n), back));
        CHECK (back.empty());
    }
    auto padded = bytes;
    padded.push_back (0);
    CHECK_FALSE (decodeSetups (padded, back));

    auto encodeOne = [] (LaneSetup s) {
        std::vector<std::uint8_t> out;
        encodeSetups (std::vector<LaneSetup> { s }, out);
        return out;
    };
    const LaneSetup good = setups[0];
    auto refuses = [&] (LaneSetup s) {
        std::vector<LaneSetup> ignored;
        return ! decodeSetups (encodeOne (s), ignored);
    };
    CHECK_FALSE (refuses (good));
    LaneSetup s = good;
    s.slot = lanes;
    CHECK (refuses (s));
    s = good, s.id = 0;
    CHECK (refuses (s));
    s = good, s.id = idMax + 1;
    CHECK (refuses (s));
    s = good, s.settings.output = 17;
    CHECK (refuses (s));
    s = good, s.settings.channel = 0;
    CHECK (refuses (s));
    s = good, s.settings.start = 0.8;
    CHECK (refuses (s));
    s = good, s.settings.end = std::numeric_limits<double>::quiet_NaN();
    CHECK (refuses (s));
    s = good, s.settings.tune = tuneMax + 1;
    CHECK (refuses (s));
    s = good, s.settings.release = -1.0;
    CHECK (refuses (s));
    s = good, s.settings.choke = chokeGroups + 1;
    CHECK (refuses (s));
    s = good, s.sample.builtin = static_cast<Builtin> (builtinCount);
    CHECK (refuses (s));
    s = good, s.sample.kind = SampleRef::Kind::File, s.sample.path = "";
    CHECK (refuses (s));
    s.sample.path = std::string (pathMax + 1, 'a');
    CHECK (refuses (s));
    s.sample.path = std::string ("bad\0nul", 7);
    CHECK (refuses (s));
    s.sample.path = "\xC0\xAF overlong";
    CHECK (refuses (s));
    s.sample.path = "cut \xE2\x82";
    CHECK (refuses (s));
    s.sample.path = "\xED\xA0\x80 surrogate";
    CHECK (refuses (s));
    s.sample.path = "/ok/\xE2\x82\xAC/\xF0\x9F\xA5\x81.wav";
    CHECK_FALSE (refuses (s));

    // Two setups for one slot.
    std::vector<std::uint8_t> twice;
    encodeSetups (std::vector<LaneSetup> { good, good }, twice);
    CHECK_FALSE (decodeSetups (twice, back));

    // An unknown format.
    auto future = bytes;
    future[0] = setupFormat + 1;
    CHECK_FALSE (decodeSetups (future, back));
}

TEST_CASE ("The plugin state keeps the engine's bytes and adds the lane setups")
{
    const auto words = engineWords();
    const auto setups = someSetups();
    std::vector<std::uint8_t> bytes;
    encodePluginState (words, setups, bytes);

    REQUIRE (bytes.size() > 9);
    CHECK (std::string (bytes.begin(), bytes.begin() + 4) == "FRWS");
    CHECK (bytes[4] == 2);
    std::vector<std::uint8_t> engineBytes;
    state::toBytes (words, engineBytes);
    const std::uint32_t length = static_cast<std::uint32_t> (bytes[5])
        | (static_cast<std::uint32_t> (bytes[6]) << 8)
        | (static_cast<std::uint32_t> (bytes[7]) << 16)
        | (static_cast<std::uint32_t> (bytes[8]) << 24);
    REQUIRE (length == engineBytes.size());
    CHECK (std::equal (engineBytes.begin(), engineBytes.end(), bytes.begin() + 9));

    PluginState back;
    REQUIRE (decodePluginState (bytes, back));
    CHECK (back.version == 2);
    CHECK (back.words == words);
    CHECK (back.setups == setups);

    for (std::size_t n = 0; n < bytes.size(); ++n)
    {
        INFO ("length " << n);
        PluginState ignored;
        CHECK_FALSE (decodePluginState (std::span (bytes).first (n), ignored));
    }
    auto wrongLength = bytes;
    wrongLength[5] = static_cast<std::uint8_t> (wrongLength[5] + 4);
    CHECK_FALSE (decodePluginState (wrongLength, back));
    auto wrongTag = bytes;
    wrongTag[0] = 'X';
    CHECK_FALSE (decodePluginState (wrongTag, back));
    auto future = bytes;
    future[4] = pluginV11Version + 1;
    CHECK_FALSE (decodePluginState (future, back));
}

TEST_CASE ("A version 1 state from the development builds still loads, with MIDI lanes")
{
    const auto words = engineWords();
    std::vector<std::uint8_t> engineBytes;
    state::toBytes (words, engineBytes);
    std::vector<std::uint8_t> v1 { 'F', 'R', 'W', 'S', 1, 1 };
    v1.insert (v1.end(), engineBytes.begin(), engineBytes.end());

    PluginState back;
    REQUIRE (decodePluginState (v1, back));
    CHECK (back.version == 1);
    CHECK (back.words == words);
    CHECK (back.setups.empty());

    auto badTones = v1;
    badTones[5] = 2;
    CHECK_FALSE (decodePluginState (badTones, back));
    auto ragged = v1;
    ragged.pop_back();
    CHECK_FALSE (decodePluginState (ragged, back));
}
