#pragma once

#include "furrow/PluginState.h"

namespace furrow
{
struct KitState
{
    std::vector<sampler::LaneSetup> setups;
    Metadata metadata;
    std::array<std::array<std::int32_t, furrow::laneFields>, lanes> laneFields {};
    bool operator== (const KitState& other) const
    {
        return setups == other.setups && laneFields == other.laneFields
            && metadata.laneNames == other.metadata.laneNames
            && metadata.laneColors == other.metadata.laneColors
            && metadata.windowWidth == other.metadata.windowWidth
            && metadata.windowHeight == other.metadata.windowHeight;
    }
};
inline void encodeKitState (const KitState& kit, std::vector<std::uint8_t>& out)
{
    out = {'F', 'R', 'W', 'K', 1};
    state_detail::Writer writer {out};
    std::vector<std::uint8_t> setups;
    sampler::encodeSetups (kit.setups, setups);
    state_detail::section (writer, setups);
    for (const auto& setup : kit.setups)
        for (const auto field : kit.laneFields[static_cast<std::size_t> (setup.slot)])
            writer.u32 (static_cast<std::uint32_t> (field));
    for (const auto& name : kit.metadata.laneNames) state_detail::text (writer, name);
    for (const auto color : kit.metadata.laneColors) writer.u32 (color);
    writer.u32 (static_cast<std::uint32_t> (kit.metadata.windowWidth));
    writer.u32 (static_cast<std::uint32_t> (kit.metadata.windowHeight));
}
inline bool decodeKitState (std::span<const std::uint8_t> bytes, KitState& out)
{
    if (bytes.size() < 9 || std::memcmp (bytes.data(), "FRWK", 4) != 0 || bytes[4] != 1) return false;
    KitState result;
    state_detail::Reader reader {bytes.subspan (5)};
    const auto setups = state_detail::section (reader);
    if (! reader.ok || ! sampler::decodeSetups (setups, result.setups)) return false;
    for (const auto& setup : result.setups)
    {
        if (setup.slot < 0 || setup.slot >= lanes || setup.id < 1 || setup.id > idMax) return false;
        auto& fields = result.laneFields[static_cast<std::size_t> (setup.slot)];
        for (int f = 0; f < furrow::laneFields; ++f)
        {
            const auto word = reader.u32();
            const auto signedWord = std::bit_cast<std::int32_t> (word);
            // Kit fields use zero as the compact representation for an unset
            // value. This matters for fields whose engine default is non-zero,
            // such as rate and bend range.
            const bool unset = signedWord == 0;
            if (! reader.ok || (! unset && (signedWord < laneMin[static_cast<std::size_t> (f)] || signedWord > laneMax[static_cast<std::size_t> (f)]))) return false;
            fields[static_cast<std::size_t> (f)] = signedWord;
        }
        for (const auto& other : result.setups)
            if (other.slot != setup.slot && other.id == setup.id) return false;
    }
    for (auto& name : result.metadata.laneNames) if (! state_detail::text (reader, name)) return false;
    for (auto& color : result.metadata.laneColors) color = reader.u32();
    const auto width = reader.u32();
    const auto height = reader.u32();
    if (! reader.ok || width < 720 || width > 4096 || height < 420 || height > 2160 || reader.at != reader.in.size()) return false;
    result.metadata.windowWidth = static_cast<int> (width);
    result.metadata.windowHeight = static_cast<int> (height);
    out = std::move (result);
    return true;
}
}
