#include "furrow/BlockClock.h"

#include <catch2/catch_test_macros.hpp>

using furrow::BlockClock;
using furrow::BlockInfo;

namespace
{
// 120 BPM at 48 kHz: one beat is exactly 24,000 samples.
BlockInfo block (double beat, int numSamples = 480)
{
    return BlockInfo { true, beat, 120.0, 48000.0, numSamples };
}
} // namespace

TEST_CASE ("The first playing block is a jump")
{
    BlockClock clock;
    REQUIRE (clock.begin (block (0.0)));
    CHECK (clock.jumped());
}

TEST_CASE ("Contiguous blocks are not jumps")
{
    BlockClock clock;
    clock.begin (block (0.0));
    REQUIRE (clock.begin (block (0.02)));
    CHECK_FALSE (clock.jumped());
}

TEST_CASE ("A seek forward or a loop wrap back is a jump")
{
    BlockClock clock;
    clock.begin (block (4.0));
    clock.begin (block (4.02));
    CHECK_FALSE (clock.jumped());

    clock.begin (block (8.0));
    CHECK (clock.jumped());

    clock.begin (block (8.02));
    clock.begin (block (0.0));
    CHECK (clock.jumped());
}

TEST_CASE ("Stopping makes the next playing block a jump")
{
    BlockClock clock;
    clock.begin (block (0.0));
    clock.stop();
    clock.begin (block (0.02));
    CHECK (clock.jumped());
}

TEST_CASE ("A block with no time leaves the clock unchanged")
{
    BlockClock clock;
    clock.begin (block (0.0));
    CHECK_FALSE (clock.begin (block (0.02, 0)));
    CHECK_FALSE (clock.begin (BlockInfo { true, 0.02, 0.0, 48000.0, 480 }));
    CHECK_FALSE (clock.begin (BlockInfo { true, 0.02, 120.0, 0.0, 480 }));

    // The empty blocks did not move the expected next position.
    REQUIRE (clock.begin (block (0.02)));
    CHECK_FALSE (clock.jumped());
}

TEST_CASE ("Offsets round half up and stay inside the block")
{
    BlockClock clock;
    clock.begin (block (1.0));

    const double sample = 1.0 / 24000.0;
    CHECK (clock.offsetOf (1.0) == 0);
    CHECK (clock.offsetOf (1.0 + 10.4 * sample) == 10);
    CHECK (clock.offsetOf (1.0 + 10.5 * sample) == 11);
    CHECK (clock.offsetOf (1.0 + 479.4 * sample) == 479);
    CHECK (clock.offsetOf (1.0 + 600.0 * sample) == 479);
    CHECK (clock.offsetOf (0.5) == 0);
}
