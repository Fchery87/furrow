#pragma once

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// State words as bytes. SeqCore's @serialize writes each word as a 32-bit float, little-endian,
// so these bytes are the same as the REAPER edition's saved state. Every state word is an
// integer below 2^24 in size, which a float holds exactly.
namespace furrow::state
{

inline void toBytes (std::span<const std::int32_t> words, std::vector<std::uint8_t>& bytes)
{
    bytes.clear();
    bytes.reserve (words.size() * 4);
    for (const auto w : words)
    {
        const auto bits = std::bit_cast<std::uint32_t> (static_cast<float> (w));
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back (static_cast<std::uint8_t> ((bits >> shift) & 0xFF));
    }
}

// False, with `words` empty, unless the size is a multiple of four and every float is a whole
// number that fits in an int32.
inline bool fromBytes (std::span<const std::uint8_t> bytes, std::vector<std::int32_t>& words)
{
    words.clear();
    if (bytes.size() % 4 != 0)
        return false;
    words.reserve (bytes.size() / 4);
    for (std::size_t i = 0; i < bytes.size(); i += 4)
    {
        std::uint32_t bits = 0;
        for (std::size_t k = 0; k < 4; ++k)
            bits |= static_cast<std::uint32_t> (bytes[i + k]) << (8 * k);
        const double value = std::bit_cast<float> (bits);
        if (! std::isfinite (value) || std::floor (value) < value || value < -2147483648.0 || value > 2147483647.0)
        {
            words.clear();
            return false;
        }
        words.push_back (static_cast<std::int32_t> (value));
    }
    return true;
}

} // namespace furrow::state
