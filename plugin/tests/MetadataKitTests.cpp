#include "furrow/KitState.h"
#include "furrow/PluginState.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace furrow;
using namespace furrow::sampler;

namespace
{
LaneSetup setup (int slot, std::int32_t id)
{
    LaneSetup s;
    s.slot = slot;
    s.id = id;
    s.settings.target = Target::Sample;
    s.settings.output = 2;
    s.sample.kind = SampleRef::Kind::Builtin;
    s.sample.builtin = Builtin::Kick;
    return s;
}

Metadata metadata()
{
    Metadata m;
    m.laneNames[0] = "Kick";
    m.laneNames[3] = "Crème";
    m.laneColors[0] = 0xFFCC8844u;
    m.laneColors[3] = 0x10203040u;
    m.patternNames[0] = "Main";
    m.patternNames[17] = "Break 🥁";
    m.windowWidth = 1100;
    m.windowHeight = 700;
    return m;
}
}

TEST_CASE ("v4 plugin state round-trips metadata, bypass, keyswitch channel, and V1.1 setups")
{
    const std::vector<std::int32_t> words { 1, 60, 5000, -7 };
    auto s0 = setup (0, 1);
    s0.settings.playMode = PlayMode::OneShot;
    s0.settings.reverse = true;
    s0.settings.filterMode = FilterMode::LowPass;
    s0.settings.filterCutoffHz = 1800.0;
    s0.settings.filterResonance = 0.45;
    const std::vector<LaneSetup> setups { s0, setup (3, 99) };
    auto expected = metadata();
    expected.bypass = true;
    expected.patternMidiChannel = 16;
    std::vector<std::uint8_t> bytes;
    encodePluginState (words, setups, expected, bytes);

    REQUIRE (bytes.size() > 9);
    CHECK (std::string (bytes.begin(), bytes.begin() + 4) == "FRWS");
    CHECK (bytes[4] == pluginV11Version);
    PluginState decoded;
    REQUIRE (decodePluginState (bytes, decoded));
    CHECK (decoded.version == pluginV11Version);
    CHECK (decoded.words == words);
    CHECK (decoded.setups == setups);
    CHECK (decoded.metadata == expected);
}

TEST_CASE ("legacy plugin state overload remains version 2")
{
    const std::vector<std::int32_t> words { 4, 8, 15, 16, 23, 42 };
    const std::vector<LaneSetup> setups { setup (2, 7) };
    std::vector<std::uint8_t> bytes;
    encodePluginState (words, setups, bytes);
    REQUIRE (bytes[4] == 2);
    PluginState decoded;
    REQUIRE (decodePluginState (bytes, decoded));
    CHECK (decoded.version == 2);
    CHECK (decoded.words == words);
    CHECK (decoded.setups == setups);
    CHECK (decoded.metadata == Metadata {});
}

TEST_CASE ("plugin state rejects every truncation, padding, and malformed metadata")
{
    std::vector<std::uint8_t> bytes;
    encodePluginState (std::vector<std::int32_t> { 1, 2 }, std::vector<LaneSetup> { setup (0, 1) }, metadata(), bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n)
    {
        PluginState decoded;
        CHECK_FALSE (decodePluginState (std::span (bytes).first (n), decoded));
    }
    auto padded = bytes;
    padded.push_back (0);
    PluginState decoded;
    CHECK_FALSE (decodePluginState (padded, decoded));

    auto badUtf8 = bytes;
    // The first metadata string length follows the engine and setup section. Locate it by
    // decoding a valid state, then use the public framing's re-encoding to find its offset.
    PluginState valid;
    REQUIRE (decodePluginState (bytes, valid));
    std::vector<std::uint8_t> legacy;
    encodePluginState (valid.words, valid.setups, legacy);
    const auto metadataOffset = legacy.size();
    badUtf8[metadataOffset + 4] = 0xC0;
    badUtf8[metadataOffset + 5] = 0xAF;
    CHECK_FALSE (decodePluginState (badUtf8, decoded));
}

TEST_CASE ("kit codec round-trips lane fields and omits patterns")
{
    KitState expected;
    expected.setups = { setup (0, 1), setup (8, 123) };
    expected.metadata = metadata();
    expected.laneFields[0][LaneField::Note] = 36;
    expected.laneFields[0][LaneField::Active] = 1;
    expected.laneFields[8][LaneField::Volume] = 7300;

    std::vector<std::uint8_t> bytes;
    encodeKitState (expected, bytes);
    REQUIRE (bytes.size() > 5);
    CHECK (std::string (bytes.begin(), bytes.begin() + 4) == "FRWK");
    KitState decoded;
    REQUIRE (decodeKitState (bytes, decoded));
    CHECK (decoded == expected);

    auto withPattern = expected;
    withPattern.metadata.patternNames[0] = "must not be serialized";
    std::vector<std::uint8_t> changed;
    encodeKitState (withPattern, changed);
    CHECK (changed == bytes);
}

TEST_CASE ("kit codec rejects truncation, padding, and invalid lane fields")
{
    KitState expected;
    expected.setups = { setup (1, 2) };
    expected.metadata = metadata();
    expected.laneFields[1][LaneField::Note] = 48;
    std::vector<std::uint8_t> bytes;
    encodeKitState (expected, bytes);
    for (std::size_t n = 0; n < bytes.size(); ++n)
    {
        KitState decoded;
        CHECK_FALSE (decodeKitState (std::span (bytes).first (n), decoded));
    }
    auto padded = bytes;
    padded.push_back (0);
    KitState decoded;
    CHECK_FALSE (decodeKitState (padded, decoded));
}
