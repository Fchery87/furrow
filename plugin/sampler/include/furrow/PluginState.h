#pragma once

#include "furrow/StateBytes.h"
#include "furrow/sampler/LaneSetup.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace furrow
{
inline constexpr std::array<char, 4> pluginStateTag {'F', 'R', 'W', 'S'};
inline constexpr std::uint8_t pluginStateVersion = 2;
inline constexpr std::uint8_t pluginMetadataVersion = 3;
struct Metadata
{
    std::array<std::string, lanes> laneNames {};
    std::array<std::uint32_t, lanes> laneColors {};
    std::array<std::string, patterns> patternNames {};
    int windowWidth = 1100;
    int windowHeight = 700;
    bool operator== (const Metadata&) const = default;
};
struct PluginState
{
    int version = pluginStateVersion;
    std::vector<std::int32_t> words;
    std::vector<sampler::LaneSetup> setups;
    Metadata metadata;
};
namespace state_detail
{
using Writer = sampler::detail::Writer;
using Reader = sampler::detail::Reader;
inline void text (Writer& writer, const std::string& value)
{
    writer.u32 (static_cast<std::uint32_t> (value.size()));
    writer.out.insert (writer.out.end(), value.begin(), value.end());
}
inline bool text (Reader& reader, std::string& value)
{
    const auto length = reader.u32();
    if (! reader.ok || length > 512 || ! reader.has (length)) return false;
    value.assign (reinterpret_cast<const char*> (reader.in.data() + reader.at), length);
    reader.at += length;
    return value.find ('\0') == std::string::npos && sampler::detail::validUtf8 (value);
}
inline void metadata (Writer& writer, const Metadata& value)
{
    for (const auto& name : value.laneNames) text (writer, name);
    for (const auto color : value.laneColors) writer.u32 (color);
    for (const auto& name : value.patternNames) text (writer, name);
    writer.u32 (static_cast<std::uint32_t> (value.windowWidth));
    writer.u32 (static_cast<std::uint32_t> (value.windowHeight));
}
inline bool metadata (Reader& reader, Metadata& value)
{
    for (auto& name : value.laneNames) if (! text (reader, name)) return false;
    for (auto& color : value.laneColors) color = reader.u32();
    for (auto& name : value.patternNames) if (! text (reader, name)) return false;
    const auto width = reader.u32(), height = reader.u32();
    if (! reader.ok || width < 720 || width > 4096 || height < 420 || height > 2160) return false;
    value.windowWidth = static_cast<int> (width);
    value.windowHeight = static_cast<int> (height);
    return true;
}
inline std::span<const std::uint8_t> section (Reader& reader)
{
    const auto size = reader.u32();
    if (! reader.has (size)) { reader.ok = false; return {}; }
    const auto result = reader.in.subspan (reader.at, size);
    reader.at += size;
    return result;
}
inline void section (Writer& writer, std::span<const std::uint8_t> bytes)
{
    writer.u32 (static_cast<std::uint32_t> (bytes.size()));
    writer.out.insert (writer.out.end(), bytes.begin(), bytes.end());
}
}
inline void encodePluginState (std::span<const std::int32_t> words, std::span<const sampler::LaneSetup> setups, std::vector<std::uint8_t>& out)
{
    std::vector<std::uint8_t> engine;
    state::toBytes (words, engine);
    out.assign (pluginStateTag.begin(), pluginStateTag.end());
    out.push_back (pluginStateVersion);
    state_detail::Writer writer {out};
    state_detail::section (writer, engine);
    sampler::encodeSetups (setups, out);
}
inline void encodePluginState (std::span<const std::int32_t> words, std::span<const sampler::LaneSetup> setups, const Metadata& metadata, std::vector<std::uint8_t>& out)
{
    std::vector<std::uint8_t> engine, lanesBytes;
    state::toBytes (words, engine);
    sampler::encodeSetups (setups, lanesBytes);
    out.assign (pluginStateTag.begin(), pluginStateTag.end());
    out.push_back (pluginMetadataVersion);
    state_detail::Writer writer {out};
    state_detail::section (writer, engine);
    state_detail::section (writer, lanesBytes);
    state_detail::metadata (writer, metadata);
}
inline bool decodePluginState (std::span<const std::uint8_t> bytes, PluginState& out)
{
    if (bytes.size() < 6 || std::memcmp (bytes.data(), pluginStateTag.data(), 4) != 0) return false;
    PluginState result;
    result.version = bytes[4];
    if (result.version == 1)
    {
        if (bytes[5] > 1 || ! state::fromBytes (bytes.subspan (6), result.words)) return false;
    }
    else if (result.version == pluginStateVersion || result.version == pluginMetadataVersion)
    {
        state_detail::Reader reader {bytes.subspan (5)};
        const auto engine = state_detail::section (reader);
        if (! reader.ok || ! state::fromBytes (engine, result.words)) return false;
        if (result.version == pluginStateVersion)
        {
            if (! sampler::decodeSetups (reader.in.subspan (reader.at), result.setups)) return false;
        }
        else
        {
            const auto setups = state_detail::section (reader);
            if (! reader.ok || ! sampler::decodeSetups (setups, result.setups)
                || ! state_detail::metadata (reader, result.metadata) || reader.at != reader.in.size()) return false;
        }
    }
    else return false;
    out = std::move (result);
    return true;
}
}
