#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace furrow::sampler
{

// A decoded sample, ready for the audio thread. It never changes once made. Each channel has
// silent padding before and after, so interpolation near either end reads silence instead of
// leaving the buffer.
class SampleBuffer
{
public:
    static constexpr int padding = 4;
    static constexpr int maxChannels = 2;
    // Ten minutes at 192 kHz.
    static constexpr std::int64_t maxFrames = 192000LL * 600;

    // Copies 1 or 2 planar channels. Frames that are not finite become silence. Returns nothing
    // for a channel count, length or sample rate it cannot play.
    static std::unique_ptr<SampleBuffer> make (std::span<const float* const> channels, std::int64_t frames, double sampleRate)
    {
        if (channels.empty() || channels.size() > maxChannels || frames < 1 || frames > maxFrames
            || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return nullptr;
        for (const float* c : channels)
            if (c == nullptr)
                return nullptr;
        std::unique_ptr<SampleBuffer> buffer (new SampleBuffer (static_cast<int> (channels.size()), frames, sampleRate));
        for (std::size_t c = 0; c < channels.size(); ++c)
        {
            float* out = buffer->data_.data() + c * static_cast<std::size_t> (buffer->stride_) + padding;
            for (std::int64_t f = 0; f < frames; ++f)
            {
                const float v = channels[c][f];
                out[f] = std::isfinite (v) ? v : 0.0f;
            }
        }
        return buffer;
    }

    int channels() const { return channels_; }
    std::int64_t frames() const { return frames_; }
    double sampleRate() const { return sampleRate_; }

    // Frame 0 of a channel. Indexes from -padding to frames() + padding - 1 can be read.
    const float* channel (int c) const { return data_.data() + static_cast<std::size_t> (c) * static_cast<std::size_t> (stride_) + padding; }

private:
    SampleBuffer (int channels, std::int64_t frames, double sampleRate)
        : channels_ (channels), frames_ (frames), stride_ (frames + 2 * padding), sampleRate_ (sampleRate),
          data_ (static_cast<std::size_t> (channels) * static_cast<std::size_t> (frames + 2 * padding), 0.0f)
    {
    }

    int channels_;
    std::int64_t frames_;
    std::int64_t stride_;
    double sampleRate_;
    std::vector<float> data_;
};

} // namespace furrow::sampler
