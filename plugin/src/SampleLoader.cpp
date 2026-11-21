#include "SampleLoader.h"

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstring>

SampleLoader::SampleLoader (std::function<void (Result&&)> done, std::function<void()> tick)
    : juce::Thread ("Furrow sample loader"), done_ (std::move (done)), tick_ (std::move (tick))
{
    formats_.registerFormat (new juce::WavAudioFormat(), true);
    formats_.registerFormat (new juce::AiffAudioFormat(), false);
    formats_.registerFormat (new juce::FlacAudioFormat(), false);
    startThread();
}

SampleLoader::~SampleLoader()
{
    // A decoder must finish before its callbacks and format readers are destroyed.
    // A timed stop may forcibly kill the thread inside a decoder or while holding a mutex.
    signalThreadShouldExit();
    notify();
    stopThread (-1);
}

void SampleLoader::request (int lane, std::uint64_t generation, const juce::String& path)
{
    {
        const std::lock_guard<std::mutex> lock (lock_);
        if (lane < 0 || lane >= furrow::lanes)
            return;
        std::erase_if (jobs_, [lane] (const Job& job) { return job.lane == lane; });
        jobs_.push_back ({ lane, generation, path });
    }
    notify();
}

void SampleLoader::run()
{
    while (! threadShouldExit())
    {
        Job job;
        bool have = false;
        {
            const std::lock_guard<std::mutex> lock (lock_);
            if (! jobs_.empty())
            {
                job = std::move (jobs_.front());
                jobs_.pop_front();
                have = true;
            }
        }
        if (have)
        {
            Result result;
            try
            {
                result = load (formats_, job.path);
            }
            catch (const std::exception&)
            {
                result.error = "The sample could not be loaded. Try a smaller file.";
            }
            result.lane = job.lane;
            result.generation = job.generation;
            done_ (std::move (result));
        }
        tick_();
        if (! have)
            wait (50);
    }
}

std::array<float, SampleLoader::peakBins> SampleLoader::computePeaks (const furrow::sampler::SampleBuffer& buffer)
{
    std::array<float, peakBins> peaks {};
    const auto frames = buffer.frames();
    const int channels = buffer.channels();
    if (frames < 1 || channels < 1)
        return peaks;
    float maxPeak = 0.0f;
    for (int bin = 0; bin < peakBins; ++bin)
    {
        const auto start = (frames * bin) / peakBins;
        const auto end = std::max (start + 1, (frames * (bin + 1)) / peakBins);
        float binPeak = 0.0f;
        for (int c = 0; c < channels; ++c)
        {
            const float* data = buffer.channel (c);
            for (std::int64_t f = start; f < std::min (frames, end); ++f)
                binPeak = std::max (binPeak, std::abs (data[f]));
        }
        peaks[static_cast<std::size_t> (bin)] = binPeak;
        maxPeak = std::max (maxPeak, binPeak);
    }
    if (maxPeak > 0.0f)
        for (float& p : peaks)
            p /= maxPeak;
    return peaks;
}

SampleLoader::Result SampleLoader::load (juce::AudioFormatManager& formats, const juce::String& path)
{
    using furrow::sampler::SampleBuffer;
    Result result;
    const juce::File file (path);
    if (! juce::File::isAbsolutePath (path) || ! file.existsAsFile())
    {
        result.error = "The file was not found: " + path;
        return result;
    }
    const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        result.error = "The file is not a WAV, AIFF or FLAC file Furrow can read: " + path;
        return result;
    }
    if (reader->numChannels < 1 || reader->lengthInSamples < 1 || reader->lengthInSamples > SampleBuffer::maxFrames
        || ! (reader->sampleRate > 0.0))
    {
        result.error = "The file is empty, longer than ten minutes at 192 kHz, or has no sample rate: " + path;
        return result;
    }

    // A file with more than two channels plays its first two.
    const int channels = std::min (2, static_cast<int> (reader->numChannels));
    const auto frames = static_cast<int> (reader->lengthInSamples);
    juce::AudioBuffer<float> audio (channels, frames);
    if (! reader->read (&audio, 0, frames, 0, true, channels > 1))
    {
        result.error = "The file could not be read to its end: " + path;
        return result;
    }
    const float* pointers[2] = { audio.getReadPointer (0), audio.getReadPointer (channels > 1 ? 1 : 0) };
    result.sample = SampleBuffer::make (std::span (pointers, static_cast<std::size_t> (channels)), frames, reader->sampleRate);
    if (result.sample == nullptr)
    {
        result.error = "The file's audio could not be used: " + path;
        return result;
    }
    result.peaks = computePeaks (*result.sample);
    result.hasPeaks = true;

    const auto digest = juce::SHA256 (file).getRawData();
    if (digest.getSize() == result.hash.size())
        std::memcpy (result.hash.data(), digest.getData(), result.hash.size());
    return result;
}
