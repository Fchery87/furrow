#include "furrow/sampler/BuiltinKit.h"
#include "furrow/sampler/Curves.h"
#include "furrow/sampler/Sampler.h"
#include "support/AllocationCounter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

// The sampler plays a lane's sample from the engine's messages for that lane, as RS5K does for a
// REAPER edition lane. Its curves come from probes/results/reaper-7.82-linux/rs5k-v3-results.txt.

using namespace furrow;
using namespace furrow::sampler;
using Catch::Approx;

namespace
{
constexpr double rate = 48000.0;
constexpr int root = 60;
constexpr int bendRange = 2;

double db (double ratio) { return 20.0 * std::log10 (ratio); }

std::unique_ptr<SampleBuffer> mono (const std::vector<float>& frames, double sampleRate = rate)
{
    const float* channels[] = { frames.data() };
    return SampleBuffer::make (channels, static_cast<std::int64_t> (frames.size()), sampleRate);
}

// frames[i] = (i + 1) / 1000, so every frame is distinct and none is zero.
std::vector<float> ramp (int length)
{
    std::vector<float> frames (static_cast<std::size_t> (length));
    for (int i = 0; i < length; ++i)
        frames[static_cast<std::size_t> (i)] = static_cast<float> (i + 1) / 1000.0f;
    return frames;
}

std::vector<float> constant (int length, float value) { return std::vector<float> (static_cast<std::size_t> (length), value); }

MidiEvent noteOn (int lane, int note, int velocity, int offset = 0) { return { offset, lane, 0x90, static_cast<std::uint8_t> (note), static_cast<std::uint8_t> (velocity) }; }
MidiEvent noteOff (int lane, int note, int offset = 0) { return { offset, lane, 0x80, static_cast<std::uint8_t> (note), 0 }; }
MidiEvent cc (int lane, int number, int value) { return { 0, lane, 0xB0, static_cast<std::uint8_t> (number), static_cast<std::uint8_t> (value) }; }
MidiEvent bend (int lane, int value) { return { 0, lane, 0xE0, static_cast<std::uint8_t> (value % 128), static_cast<std::uint8_t> (value / 128) }; }

// Settings that make the output easy to predict: no attack or release ramps.
LaneSettings plain()
{
    LaneSettings s;
    s.target = Target::Sample;
    s.attack = 0.0;
    s.release = 0.0;
    return s;
}

// Stereo output buffers for the 16 buses. Only the buses asked for are connected.
struct Outputs
{
    explicit Outputs (int frames, std::initializer_list<int> connected = { 0 })
    {
        for (const int b : connected)
        {
            left[static_cast<std::size_t> (b)].assign (static_cast<std::size_t> (frames), 0.0f);
            right[static_cast<std::size_t> (b)].assign (static_cast<std::size_t> (frames), 0.0f);
            buses[static_cast<std::size_t> (b)] = { left[static_cast<std::size_t> (b)].data(), right[static_cast<std::size_t> (b)].data() };
        }
    }
    std::array<std::vector<float>, outputs> left, right;
    std::array<Bus, outputs> buses {};
    float l (int frame, int b = 0) const { return left[static_cast<std::size_t> (b)][static_cast<std::size_t> (frame)]; }
    float r (int frame, int b = 0) const { return right[static_cast<std::size_t> (b)][static_cast<std::size_t> (frame)]; }
};

// A sampler at 48 kHz with lane 0 set up and holding the sample.
struct Rig
{
    explicit Rig (std::unique_ptr<SampleBuffer> sample, LaneSettings settings = plain(), int lane = 0)
    {
        sampler.prepare (rate);
        add (lane, std::move (sample), settings);
    }
    void add (int lane, std::unique_ptr<SampleBuffer> sample, LaneSettings settings)
    {
        REQUIRE (sampler.setLane (lane, settings));
        REQUIRE (sampler.setSample (lane, sample));
        sampler.update();
    }
    void on (int lane, int note, int velocity = 127) { sampler.handle (noteOn (lane, note, velocity), root, bendRange); }
    Sampler sampler;
};
} // namespace

TEST_CASE ("The level curves match RS5K as the REAPER edition sets it up")
{
    // Velocity with Gain for minimum velocity at -30 dB. RS5K's floor read -30.05 dB, from the
    // REAPER edition's search for the parameter position.
    const std::array<std::pair<int, double>, 8> velocity { { { 1, -30.0497 }, { 2, -28.1487 }, { 16, -16.6683 }, { 32, -11.3809 },
                                                             { 64, -5.7508 }, { 96, -2.3636 }, { 126, -0.0661 }, { 127, 0.0 } } };
    for (const auto& [v, measured] : velocity)
    {
        INFO ("velocity " << v);
        CHECK (db (velocityGain (v)) == Approx (measured).margin (0.06));
    }

    // CC 7 linked to Volume: silence at 0, +6.02 dB at 127, linear in between.
    CHECK (volumeGain (0) == 0.0);
    CHECK (db (volumeGain (127)) == Approx (6.0214).margin (0.002));
    CHECK (db (volumeGain (100)) == Approx (3.94).margin (0.01));

    // CC 10 linked to Pan, as left and right levels in dB.
    const std::array<std::tuple<int, double, double>, 11> pan { { { 1, 0.0, -36.0542 }, { 16, 0.0, -11.9717 }, { 32, 0.0, -5.9511 },
                                                                  { 48, 0.0, -2.4294 }, { 63, 0.0, -0.0672 }, { 64, -0.0677, 0.0 },
                                                                  { 65, -0.2068, 0.0 }, { 80, -2.6120, 0.0 }, { 96, -6.2280, 0.0 },
                                                                  { 112, -12.5324, 0.0 }, { 126, -36.0542, 0.0 } } };
    for (const auto& [value, left, right] : pan)
    {
        INFO ("CC 10 = " << value);
        const auto gains = panGains (value);
        CHECK (db (gains.left) == Approx (left).margin (0.01));
        CHECK (db (gains.right) == Approx (right).margin (0.01));
    }
    CHECK (panGains (0).right == 0.0);
    CHECK (panGains (127).left == 0.0);

    CHECK (bendSemitones (8192, 2) == 0.0);
    CHECK (bendSemitones (0, 2) == -2.0);
    CHECK (bendSemitones (0, 12) == -12.0);
    CHECK (bendSemitones (16383, 2) == Approx (2.0 * 8191.0 / 8192.0));
}

TEST_CASE ("A sample buffer copies finite frames and refuses what it cannot play")
{
    const std::vector<float> frames { 0.5f, std::nanf (""), INFINITY, -0.25f };
    auto buffer = mono (frames);
    REQUIRE (buffer);
    CHECK (buffer->frames() == 4);
    CHECK (buffer->channels() == 1);
    CHECK (buffer->channel (0)[0] == 0.5f);
    CHECK (buffer->channel (0)[1] == 0.0f);
    CHECK (buffer->channel (0)[2] == 0.0f);
    CHECK (buffer->channel (0)[3] == -0.25f);
    // Reading just past either end gives silence.
    CHECK (buffer->channel (0)[-1] == 0.0f);
    CHECK (buffer->channel (0)[4 + SampleBuffer::padding - 1] == 0.0f);

    const float* three[] = { frames.data(), frames.data(), frames.data() };
    CHECK_FALSE (SampleBuffer::make (three, 4, rate));
    CHECK_FALSE (SampleBuffer::make (std::span (three, 1), 0, rate));
    CHECK_FALSE (SampleBuffer::make (std::span (three, 1), 4, 0.0));
    CHECK_FALSE (SampleBuffer::make (std::span (three, 1), 4, std::nan ("")));
}

TEST_CASE ("A note plays the sample from its first frame at the event's offset")
{
    Rig rig (mono (ramp (100)));
    Outputs out (64);
    rig.sampler.render (out.buses, 0, 10);
    rig.sampler.handle (noteOn (0, root, 127, 10), root, bendRange);
    rig.sampler.render (out.buses, 10, 64);
    for (int i = 0; i < 10; ++i)
        CHECK (out.l (i) == 0.0f);
    for (int k = 0; k < 54; ++k)
    {
        INFO ("frame " << k);
        CHECK (out.l (10 + k) == Approx ((k + 1) / 1000.0).epsilon (1e-6));
        CHECK (out.r (10 + k) == out.l (10 + k));
    }
}

TEST_CASE ("The default attack and release ramp over 1 ms, as RS5K's do")
{
    LaneSettings s = plain();
    s.attack = 1.0;
    s.release = 1.0;
    Rig rig (mono (constant (48000, 0.5f)), s);
    Outputs out (200);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 100);
    // 1 ms is 48 frames. The first frame is already audible, so a hit lands on its sample.
    CHECK (out.l (0) == Approx (0.5 / 48.0));
    CHECK (out.l (23) == Approx (0.5 * 24.0 / 48.0));
    CHECK (out.l (47) == Approx (0.5));
    CHECK (out.l (99) == Approx (0.5));

    rig.sampler.handle (noteOff (0, root, 100), root, bendRange);
    rig.sampler.render (out.buses, 100, 200);
    CHECK (out.l (100) < 0.5f);
    CHECK (out.l (100 + 47) <= 0.5f * 0.001f);
    CHECK (out.l (100 + 48) == 0.0f);
    CHECK (rig.sampler.activeVoices() == 0);
}

TEST_CASE ("A voice without a note-off stops at the end of the sample")
{
    Rig rig (mono (constant (100, 0.5f)));
    Outputs out (200);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 200);
    CHECK (out.l (99) == 0.5f);
    CHECK (out.l (100) == 0.0f);
    CHECK (rig.sampler.activeVoices() == 0);
}

TEST_CASE ("Decay fades a held voice to silence over its time")
{
    LaneSettings s = plain();
    s.decay = 10.0;
    Rig rig (mono (constant (48000, 0.5f)), s);
    Outputs out (1000);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 1000);
    // 10 ms is 480 frames, to -60 dB.
    CHECK (out.l (0) == Approx (0.5));
    CHECK (db (out.l (240) / 0.5) == Approx (-30.0).margin (0.2));
    CHECK (out.l (481) == 0.0f);
    CHECK (rig.sampler.activeVoices() == 0);
}

TEST_CASE ("Start and end trim the sample")
{
    LaneSettings s = plain();
    s.start = 0.25;
    s.end = 0.5;
    Rig rig (mono (ramp (1000)), s);
    Outputs out (400);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 400);
    CHECK (out.l (0) == Approx (251 / 1000.0));
    CHECK (out.l (249) == Approx (500 / 1000.0));
    CHECK (out.l (250) == 0.0f);
}

TEST_CASE ("Pitch follows the note against the lane's root, plus the lane's tune")
{
    SECTION ("an octave up plays every other frame")
    {
        Rig rig (mono (ramp (1000)));
        Outputs out (100);
        rig.on (0, root + 12);
        rig.sampler.render (out.buses, 0, 100);
        for (int k = 0; k < 100; ++k)
            CHECK (out.l (k) == Approx ((2 * k + 1) / 1000.0).epsilon (1e-6));
    }
    SECTION ("tune adds cents")
    {
        LaneSettings s = plain();
        s.tune = -1200;
        Rig rig (mono (ramp (1000)), s);
        Outputs out (100);
        rig.on (0, root + 24);
        rig.sampler.render (out.buses, 0, 100);
        for (int k = 0; k < 100; ++k)
            CHECK (out.l (k) == Approx ((2 * k + 1) / 1000.0).epsilon (1e-6));
    }
    SECTION ("the root is the lane's note, so a drum lane's note does not repitch it")
    {
        Rig rig (mono (ramp (1000)));
        Outputs out (100);
        rig.sampler.handle (noteOn (0, 36, 127), 36, bendRange);
        rig.sampler.render (out.buses, 0, 100);
        CHECK (out.l (99) == Approx (100 / 1000.0));
    }
}

TEST_CASE ("A sample at another rate is resampled to the host's rate")
{
    // A 24 kHz ramp at 48 kHz: whole frames on even outputs, and the cubic interpolation gives
    // the exact midpoint of a straight line on odd ones.
    Rig rig (mono (ramp (1000), 24000.0));
    Outputs out (100);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 100);
    for (int k = 1; k < 49; ++k)
    {
        CHECK (out.l (2 * k) == Approx ((k + 1) / 1000.0).epsilon (1e-6));
        CHECK (out.l (2 * k + 1) == Approx ((k + 1.5) / 1000.0).epsilon (1e-6));
    }
}

TEST_CASE ("Between frames, the sampler interpolates with a Catmull-Rom cubic")
{
    // A curved sample at half speed. The expected values use the textbook form of the
    // Catmull-Rom spline, written independently of the sampler's.
    std::vector<float> frames (200);
    for (std::size_t i = 0; i < frames.size(); ++i)
        frames[i] = static_cast<float> (std::sin (static_cast<double> (i) * 0.3));
    Rig rig (mono (frames, 24000.0));
    Outputs out (300);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 300);
    for (int k = 2; k < 150; ++k)
    {
        const auto at = [&] (int i) { return static_cast<double> (frames[static_cast<std::size_t> (i)]); };
        const double xm1 = at (k - 1), x0 = at (k), x1 = at (k + 1), x2 = at (k + 2), t = 0.5;
        const double expected = x0 + 0.5 * t * (x1 - xm1 + t * (2.0 * xm1 - 5.0 * x0 + 4.0 * x1 - x2 + t * (3.0 * (x0 - x1) + x2 - xm1)));
        INFO ("frame " << k);
        CHECK (out.l (2 * k + 1) == Approx (expected).margin (1e-6));
    }
}

TEST_CASE ("Pitch bend follows the lane's range and moves held voices")
{
    Rig rig (mono (ramp (4000)));
    Outputs out (200);
    rig.sampler.handle (bend (0, 0), root, 12);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 100);
    // An octave down: half speed.
    CHECK (out.l (98) == Approx (50 / 1000.0).epsilon (1e-6));
    rig.sampler.handle (bend (0, 8192), root, 12);
    rig.sampler.render (out.buses, 100, 200);
    CHECK (out.l (199) - out.l (198) == Approx (1 / 1000.0).epsilon (1e-4));
}

TEST_CASE ("Velocity, volume and pan scale the output as RS5K's curves do")
{
    Rig rig (mono (constant (48000, 0.5f)));
    Outputs out (40);
    rig.sampler.handle (cc (0, 7, 127), root, bendRange);
    rig.sampler.handle (cc (0, 10, 0), root, bendRange);
    rig.on (0, root, 64);
    rig.sampler.render (out.buses, 0, 10);
    CHECK (out.l (5) == Approx (0.5 * velocityGain (64) * 2.0));
    CHECK (out.r (5) == 0.0f);
    // Controllers act on the lane, so a held voice follows them.
    rig.sampler.handle (cc (0, 10, 127), root, bendRange);
    rig.sampler.handle (cc (0, 7, 0), root, bendRange);
    rig.sampler.render (out.buses, 10, 20);
    CHECK (out.l (15) == 0.0f);
    CHECK (out.r (15) == 0.0f);
}

TEST_CASE ("A stereo sample keeps its channels, and a mono bus gets their average")
{
    const auto left = constant (1000, 0.25f), right = constant (1000, 0.75f);
    const float* channels[] = { left.data(), right.data() };
    SECTION ("stereo bus")
    {
        Rig rig (SampleBuffer::make (channels, 1000, rate));
        Outputs out (10);
        rig.on (0, root);
        rig.sampler.render (out.buses, 0, 10);
        CHECK (out.l (5) == 0.25f);
        CHECK (out.r (5) == 0.75f);
    }
    SECTION ("mono bus")
    {
        Rig rig (SampleBuffer::make (channels, 1000, rate));
        Outputs out (10);
        out.buses[0].right = nullptr;
        rig.on (0, root);
        rig.sampler.render (out.buses, 0, 10);
        CHECK (out.l (5) == 0.5f);
    }
}

TEST_CASE ("A lane plays on its output bus, or on the main bus when that bus is off")
{
    LaneSettings s = plain();
    s.output = 3;
    SECTION ("bus 3 connected")
    {
        Rig rig (mono (constant (1000, 0.5f)), s);
        Outputs out (10, { 0, 2 });
        rig.on (0, root);
        rig.sampler.render (out.buses, 0, 10);
        CHECK (out.l (5, 2) == 0.5f);
        CHECK (out.l (5, 0) == 0.0f);
    }
    SECTION ("bus 3 off")
    {
        Rig rig (mono (constant (1000, 0.5f)), s);
        Outputs out (10, { 0 });
        rig.on (0, root);
        rig.sampler.render (out.buses, 0, 10);
        CHECK (out.l (5, 0) == 0.5f);
    }
}

TEST_CASE ("A hit in a choke group silences the group's other lanes within 5 ms")
{
    LaneSettings a = plain(), b = plain(), c = plain();
    a.choke = b.choke = 1;
    c.choke = 2;
    b.output = 2;
    c.output = 3;
    Rig rig (mono (constant (48000, 0.5f)), a, 0);
    rig.add (1, mono (constant (48000, 0.5f)), b);
    rig.add (2, mono (constant (48000, 0.5f)), c);
    Outputs out (1000, { 0, 1, 2 });
    rig.on (0, root);
    rig.on (2, root);
    rig.sampler.render (out.buses, 0, 100);
    rig.on (1, root);
    rig.sampler.render (out.buses, 100, 1000);
    CHECK (out.l (99, 0) == 0.5f);
    CHECK (out.l (100, 0) < 0.5f);
    CHECK (out.l (100 + 240, 0) == 0.0f);
    CHECK (out.l (999, 1) == 0.5f);
    CHECK (out.l (999, 2) == 0.5f);
    // Lane 1's own new voice is not choked.
    CHECK (rig.sampler.activeVoices() == 2);
}

TEST_CASE ("A choke group leaves a lane's own earlier hit to its release")
{
    LaneSettings s = plain();
    s.choke = 1;
    s.release = 1000.0;
    Rig rig (mono (constant (48000, 0.5f)), s);
    Outputs out (1000);
    rig.on (0, root);
    rig.sampler.handle (noteOff (0, root), root, bendRange);
    rig.sampler.render (out.buses, 0, 100);
    rig.on (0, root);
    rig.sampler.render (out.buses, 100, 1000);
    // The first voice is still in its one-second release, well above silence.
    CHECK (rig.sampler.activeVoices() == 2);
}

TEST_CASE ("All-notes-off releases a lane's voices and a reset silences everything")
{
    Rig rig (mono (constant (48000, 0.5f)));
    rig.add (1, mono (constant (48000, 0.5f)), plain());
    Outputs out (100);
    rig.on (0, root);
    rig.on (1, root);
    rig.sampler.handle (cc (0, 123, 0), root, bendRange);
    rig.sampler.render (out.buses, 0, 10);
    CHECK (out.l (5) == 0.5f);
    CHECK (rig.sampler.activeVoices() == 1);
    rig.sampler.reset();
    CHECK (rig.sampler.activeVoices() == 0);
}

TEST_CASE ("A lane keeps at most eight voices")
{
    LaneSettings s = plain();
    s.release = 10000.0;
    Rig rig (mono (constant (480000, 0.5f)), s);
    Outputs out (2000);
    for (int i = 0; i < Sampler::laneVoices + 3; ++i)
    {
        rig.on (0, root);
        rig.sampler.handle (noteOff (0, root), root, bendRange);
        rig.sampler.render (out.buses, i * 10, i * 10 + 10);
    }
    rig.sampler.render (out.buses, 200, 2000);
    CHECK (rig.sampler.activeVoices() == Sampler::laneVoices);
}

TEST_CASE ("A lane without a sample plays nothing")
{
    Sampler sampler;
    sampler.prepare (rate);
    REQUIRE (sampler.setLane (0, plain()));
    sampler.update();
    Outputs out (10);
    sampler.handle (noteOn (0, root, 127), root, bendRange);
    sampler.render (out.buses, 0, 10);
    CHECK (sampler.activeVoices() == 0);
    CHECK (out.l (5) == 0.0f);
}

TEST_CASE ("A replaced sample fades its voices and is freed off the audio thread")
{
    Rig rig (mono (constant (48000, 0.5f)));
    Outputs out (1000);
    rig.on (0, root);
    rig.sampler.render (out.buses, 0, 100);

    auto next = mono (constant (48000, 0.25f));
    REQUIRE (rig.sampler.setSample (0, next));
    CHECK_FALSE (next);
    rig.sampler.update();
    // The old sample's voice is still fading, so it is not handed back yet.
    rig.sampler.render (out.buses, 100, 110);
    rig.sampler.update();
    CHECK (rig.sampler.collect() == 0);
    rig.sampler.render (out.buses, 110, 1000);
    CHECK (out.l (999) == 0.0f);
    rig.sampler.update();
    CHECK (rig.sampler.collect() == 1);

    Outputs after (10);
    rig.on (0, root);
    rig.sampler.render (after.buses, 0, 10);
    CHECK (after.l (5) == 0.25f);
}

TEST_CASE ("A full change queue refuses changes and leaves the sample with the caller")
{
    Sampler sampler;
    sampler.prepare (rate);
    int accepted = 0;
    while (sampler.setLane (0, plain()))
        ++accepted;
    CHECK (accepted == Sampler::queueSize);
    auto sample = mono (constant (10, 0.5f));
    CHECK_FALSE (sampler.setSample (0, sample));
    CHECK (sample);
    sampler.update();
    CHECK (sampler.setSample (0, sample));
}

TEST_CASE ("Samples still playing count against the queue, so the hand-back path cannot overflow")
{
    Sampler sampler;
    sampler.prepare (rate);
    for (int lane = 0; lane < furrow::lanes; ++lane)
        REQUIRE (sampler.setLane (lane, plain()));
    sampler.update();
    // Replace each lane's sample under a playing voice, without rendering, so every replaced
    // sample is still fading and cannot be handed back yet.
    int held = 0;
    for (int i = 0; i < Sampler::voices; ++i)
    {
        const int lane = i % furrow::lanes;
        auto sample = mono (constant (100, 0.5f));
        REQUIRE (sampler.setSample (lane, sample));
        sampler.update();
        sampler.handle (noteOn (lane, root, 127), root, bendRange);
        ++held;
    }
    int accepted = 0;
    for (;;)
    {
        auto sample = mono (constant (10, 0.5f));
        if (! sampler.setSample (accepted % furrow::lanes, sample))
            break;
        ++accepted;
    }
    CHECK (accepted < static_cast<int> (Sampler::queueSize));
    CHECK (accepted + held - furrow::lanes <= static_cast<int> (Sampler::queueSize));
    sampler.update();
    Outputs out (1000);
    sampler.render (out.buses, 0, 1000);
    sampler.update();
    CHECK (sampler.collect() > 0);
}

TEST_CASE ("Updates, events and rendering never allocate")
{
    Sampler sampler;
    sampler.prepare (rate);
    for (int lane = 0; lane < furrow::lanes; ++lane)
    {
        LaneSettings s;
        s.target = Target::Sample;
        s.output = 1 + lane % outputs;
        s.choke = lane % 5;
        s.release = 50.0;
        s.tune = (lane % 7) * 100 - 300;
        REQUIRE (sampler.setLane (lane, s));
        auto sample = makeBuiltin (static_cast<Builtin> (lane % builtinCount));
        REQUIRE (sampler.setSample (lane, sample));
    }
    std::vector<std::unique_ptr<SampleBuffer>> replacements;
    for (int i = 0; i < 8; ++i)
        replacements.push_back (makeBuiltin (Builtin::Snare));

    Outputs out (512, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 });
    int replaced = 0;
    {
        test::CountAllocations count;
        for (int block = 0; block < 2000; ++block)
        {
            // A sample change posted from another thread between blocks.
            if (block % 250 == 0 && replaced < 8)
            {
                test::counting = false;
                sampler.setSample (block % furrow::lanes, replacements[static_cast<std::size_t> (replaced++)]);
                test::counting = true;
            }
            sampler.update();
            const int frames = 1 + (block * 37) % 512;
            int done = 0;
            for (int lane = 0; lane < furrow::lanes; ++lane)
            {
                const int offset = std::min (frames - 1, lane * 5);
                if (offset > done)
                {
                    sampler.render (out.buses, done, offset);
                    done = offset;
                }
                sampler.handle (cc (lane, 7, 100), root, bendRange);
                sampler.handle (bend (lane, 8192 + lane * 50), root, bendRange);
                sampler.handle (noteOff (lane, root + lane % 12), root, bendRange);
                sampler.handle (noteOn (lane, root + (lane + block) % 12, 1 + (lane * 13 + block) % 127, offset), root, bendRange);
            }
            sampler.render (out.buses, done, frames);
        }
    }
    CHECK (test::allocations == 0);
    CHECK (replaced == 8);
}

TEST_CASE ("The built-in kit is the same every time and plays at a safe level")
{
    for (int i = 0; i < builtinCount; ++i)
    {
        const auto kind = static_cast<Builtin> (i);
        INFO (builtinName (kind));
        const auto a = makeBuiltin (kind), b = makeBuiltin (kind);
        REQUIRE (a);
        REQUIRE (b);
        CHECK (a->channels() == 1);
        CHECK (a->sampleRate() == 48000.0);
        REQUIRE (a->frames() == b->frames());
        CHECK (std::equal (a->channel (0), a->channel (0) + a->frames(), b->channel (0)));
        float peak = 0.0f;
        for (std::int64_t f = 0; f < a->frames(); ++f)
            peak = std::max (peak, std::abs (a->channel (0)[f]));
        CHECK (peak > 0.5f);
        CHECK (peak <= 0.9f);
        // It ends in silence, so cutting it at the end makes no click.
        CHECK (std::abs (a->channel (0)[a->frames() - 1]) < 1.0e-3f);
    }
}

TEST_CASE ("V1.1 sampler supports OneShot, Reverse, SVF LP/HP filter, peak meters, and preview voice")
{
    Sampler sampler;
    sampler.prepare (rate);
    LaneSettings s;
    s.target = Target::Sample;
    s.attack = 0.0;
    s.release = 1.0;
    s.playMode = PlayMode::OneShot;
    REQUIRE (sampler.setLane (0, s));
    auto buf = mono (ramp (200));
    REQUIRE (sampler.setSample (0, buf));
    sampler.update();

    // OneShot ignores noteOff and plays to sample end while reporting lane peak.
    sampler.handle (noteOn (0, root, 127), root, bendRange);
    sampler.handle (noteOff (0, root), root, bendRange);
    std::vector<float> l (200, 0.0f), r (200, 0.0f);
    Bus buses[1] = { { l.data(), r.data() } };
    sampler.render (buses, 0, 200);
    CHECK (std::abs (l[150]) > 0.1f);
    CHECK (sampler.lanePeaks()[0] > 0.15f);
    sampler.clearPeaks();
    CHECK (sampler.lanePeaks()[0] == 0.0f);

    // Reverse plays frames from end down to start.
    s.reverse = true;
    REQUIRE (sampler.setLane (0, s));
    sampler.update();
    std::fill (l.begin(), l.end(), 0.0f);
    std::fill (r.begin(), r.end(), 0.0f);
    sampler.handle (noteOn (0, root, 127), root, bendRange);
    sampler.render (buses, 0, 50);
    CHECK (l[0] > l[40]);

    // LowPass filter attenuates Nyquist alternating impulse; HighPass attenuates DC.
    std::vector<float> nyq (256);
    for (std::size_t i = 0; i < nyq.size(); ++i) nyq[i] = (i % 2 == 0) ? 0.8f : -0.8f;
    auto nyqBuf = mono (nyq);
    s.reverse = false;
    s.filterMode = FilterMode::LowPass;
    s.filterCutoffHz = 500.0;
    s.filterResonance = 0.2;
    REQUIRE (sampler.setLane (0, s));
    REQUIRE (sampler.setSample (0, nyqBuf));
    sampler.update();
    std::vector<float> fl (256, 0.0f), fr (256, 0.0f);
    Bus fbuses[1] = { { fl.data(), fr.data() } };
    sampler.handle (noteOn (0, root, 127), root, bendRange);
    {
        const test::CountAllocations guard;
        sampler.render (fbuses, 0, 256);
        CHECK (test::allocations == 0);
    }
    float lpEnergy = 0.0f;
    for (std::size_t i = 160; i < 256; ++i) lpEnergy = std::max (lpEnergy, std::abs (fl[i]));
    CHECK (lpEnergy < 0.05f);

    // Preview voice renders directly on main bus, and stopAll smoothly silences both OneShot and preview voices.
    auto prev = mono (constant (4000, 0.5f));
    REQUIRE (sampler.setPreviewSample (prev));
    auto longBuf = mono (constant (4000, 0.5f));
    s.filterMode = FilterMode::Off;
    s.playMode = PlayMode::OneShot;
    REQUIRE (sampler.setLane (0, s));
    REQUIRE (sampler.setSample (0, longBuf));
    sampler.update();
    sampler.handle (noteOn (0, root, 127), root, bendRange);
    std::fill (fl.begin(), fl.end(), 0.0f);
    std::fill (fr.begin(), fr.end(), 0.0f);
    sampler.render (fbuses, 0, 64);
    CHECK (fl[10] > 0.5f);

    sampler.stopAll();
    std::vector<float> fadeL (600, 0.0f), fadeR (600, 0.0f);
    Bus fadeBuses[1] = { { fadeL.data(), fadeR.data() } };
    sampler.render (fadeBuses, 0, 600);
    CHECK (std::abs (fadeL[550]) < 1e-5f);
    CHECK (sampler.activeVoices() == 0);
}
