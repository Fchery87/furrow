#pragma once

#include "furrow/Rules.h"
#include "furrow/sampler/BuiltinKit.h"
#include "furrow/sampler/LaneSettings.h"

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// What the plugin saves for each lane beside the engine's state: the lane's sampler settings
// and which sample it plays. The engine's state stays byte for byte SeqCore's, so this lives in
// a separate section of the plugin state (see furrow/PluginState.h).
namespace furrow::sampler
{

struct SampleRef
{
    enum class Kind : std::uint8_t
    {
        None = 0,
        Builtin = 1,
        File = 2,
    };

    Kind kind = Kind::None;
    Builtin builtin = Builtin::Kick;
    std::string path;                       // a file's absolute path, UTF-8
    std::array<std::uint8_t, 32> hash {};   // SHA-256 of the file's bytes; all zero when unknown

    bool operator== (const SampleRef&) const = default;
};

struct LaneSetup
{
    int slot = 0;                           // the engine lane slot, 0 to 63
    std::int32_t id = 0;                    // the engine lane's id, so a setup cannot land on another lane
    LaneSettings settings;
    SampleRef sample;

    bool operator== (const LaneSetup&) const = default;
};

inline constexpr std::uint8_t setupFormat = 1;
inline constexpr std::size_t pathMax = 4096;

namespace detail
{
struct Writer
{
    std::vector<std::uint8_t>& out;
    void u8 (std::uint32_t v) { out.push_back (static_cast<std::uint8_t> (v)); }
    void u32 (std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            u8 ((v >> (8 * i)) & 0xFFu);
    }
    void f64 (double v)
    {
        const auto bits = std::bit_cast<std::uint64_t> (v);
        for (int i = 0; i < 8; ++i)
            u8 (static_cast<std::uint32_t> ((bits >> (8 * i)) & 0xFFu));
    }
};

struct Reader
{
    std::span<const std::uint8_t> in;
    std::size_t at = 0;
    bool ok = true;

    bool has (std::size_t n) const { return ok && in.size() - at >= n; }
    std::uint32_t u8()
    {
        if (! has (1))
            return ok = false, 0;
        return in[at++];
    }
    std::uint32_t u32()
    {
        if (! has (4))
            return ok = false, 0;
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t> (in[at++]) << (8 * i);
        return v;
    }
    double f64()
    {
        if (! has (8))
            return ok = false, 0.0;
        std::uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits |= static_cast<std::uint64_t> (in[at++]) << (8 * i);
        return std::bit_cast<double> (bits);
    }
};

// Strict UTF-8: no overlong forms, surrogates or values past U+10FFFF, and no NUL.
inline bool validUtf8 (const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const auto c = static_cast<unsigned char> (s[i]);
        if (c == 0)
            return false;
        int more = 0;
        std::uint32_t cp = 0;
        if (c < 0x80)
            cp = c;
        else if ((c & 0xE0) == 0xC0)
            more = 1, cp = c & 0x1Fu;
        else if ((c & 0xF0) == 0xE0)
            more = 2, cp = c & 0x0Fu;
        else if ((c & 0xF8) == 0xF0)
            more = 3, cp = c & 0x07u;
        else
            return false;
        // The continuation bytes must all be inside the string.
        if (i + static_cast<std::size_t> (more) >= s.size() && more > 0)
            return false;
        for (int k = 1; k <= more; ++k)
        {
            const auto n = static_cast<unsigned char> (s[i + static_cast<std::size_t> (k)]);
            if ((n & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (n & 0x3Fu);
        }
        const std::uint32_t least[] = { 0, 0x80, 0x800, 0x10000 };
        if (cp < least[more] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += static_cast<std::size_t> (more) + 1;
    }
    return true;
}
} // namespace detail

// Appends the setups to out: a format byte, a count, then one record per lane.
inline void encodeSetups (std::span<const LaneSetup> setups, std::vector<std::uint8_t>& out)
{
    detail::Writer w { out };
    w.u8 (setupFormat);
    w.u8 (static_cast<std::uint32_t> (setups.size()));
    for (const auto& s : setups)
    {
        w.u8 (static_cast<std::uint32_t> (s.slot));
        w.u32 (static_cast<std::uint32_t> (s.id));
        w.u8 (static_cast<std::uint32_t> (s.settings.target));
        w.u8 (static_cast<std::uint32_t> (s.settings.channel));
        w.u8 (static_cast<std::uint32_t> (s.settings.output));
        w.u32 (static_cast<std::uint32_t> (s.settings.tune));
        w.u8 (static_cast<std::uint32_t> (s.settings.choke));
        w.f64 (s.settings.start);
        w.f64 (s.settings.end);
        w.f64 (s.settings.attack);
        w.f64 (s.settings.decay);
        w.f64 (s.settings.release);
        w.u8 (static_cast<std::uint32_t> (s.sample.kind));
        if (s.sample.kind == SampleRef::Kind::Builtin)
            w.u8 (static_cast<std::uint32_t> (s.sample.builtin));
        else if (s.sample.kind == SampleRef::Kind::File)
        {
            w.u32 (static_cast<std::uint32_t> (s.sample.path.size()));
            out.insert (out.end(), s.sample.path.begin(), s.sample.path.end());
            out.insert (out.end(), s.sample.hash.begin(), s.sample.hash.end());
        }
    }
}

// Reads what encodeSetups wrote. Everything is checked; on any fault it returns false and
// leaves out empty.
inline bool decodeSetups (std::span<const std::uint8_t> bytes, std::vector<LaneSetup>& out)
{
    out.clear();
    detail::Reader r { bytes };
    if (r.u8() != setupFormat)
        return false;
    const auto count = r.u8();
    if (! r.ok || count > static_cast<std::uint32_t> (furrow::lanes))
        return false;
    std::array<bool, furrow::lanes> seen {};
    std::vector<LaneSetup> setups;
    for (std::uint32_t n = 0; n < count; ++n)
    {
        LaneSetup s;
        s.slot = static_cast<int> (r.u8());
        const auto id = r.u32();
        const auto target = r.u8();
        s.settings.channel = static_cast<int> (r.u8());
        s.settings.output = static_cast<int> (r.u8());
        s.settings.tune = static_cast<std::int32_t> (r.u32());
        s.settings.choke = static_cast<int> (r.u8());
        s.settings.start = r.f64();
        s.settings.end = r.f64();
        s.settings.attack = r.f64();
        s.settings.decay = r.f64();
        s.settings.release = r.f64();
        const auto kind = r.u8();
        if (! r.ok || s.slot >= furrow::lanes || seen[static_cast<std::size_t> (s.slot)] || id < 1
            || id > static_cast<std::uint32_t> (furrow::idMax) || target > 1 || kind > 2)
            return false;
        seen[static_cast<std::size_t> (s.slot)] = true;
        s.id = static_cast<std::int32_t> (id);
        s.settings.target = static_cast<Target> (target);
        if (! valid (s.settings))
            return false;
        s.sample.kind = static_cast<SampleRef::Kind> (kind);
        if (s.sample.kind == SampleRef::Kind::Builtin)
        {
            const auto builtin = r.u8();
            if (! r.ok || builtin >= static_cast<std::uint32_t> (builtinCount))
                return false;
            s.sample.builtin = static_cast<Builtin> (builtin);
        }
        else if (s.sample.kind == SampleRef::Kind::File)
        {
            const auto length = r.u32();
            if (! r.ok || length < 1 || length > pathMax || ! r.has (length + s.sample.hash.size()))
                return false;
            s.sample.path.assign (reinterpret_cast<const char*> (bytes.data() + r.at), length);
            r.at += length;
            std::copy_n (bytes.begin() + static_cast<std::ptrdiff_t> (r.at), s.sample.hash.size(), s.sample.hash.begin());
            r.at += s.sample.hash.size();
            if (! detail::validUtf8 (s.sample.path))
                return false;
        }
        setups.push_back (std::move (s));
    }
    if (r.at != bytes.size())
        return false;
    out = std::move (setups);
    return true;
}

} // namespace furrow::sampler
