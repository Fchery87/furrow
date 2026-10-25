#pragma once

#include "furrow/Rules.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace furrow
{

// Step values for every pattern and lane, stored in 16-step pages taken from a fixed pool. A
// page whose values all match the defaults is returned to the pool, so an empty rack uses none.
// Lengths are stored densely, one per pattern and lane.
//
// All memory is allocated by the constructor. No method allocates, so the audio thread may
// edit a store. Callers check capacity first (see Engine::preflight): set() assumes a free page.
//
// The free list hands out and takes back pages in SeqCore's order. That order decides the order
// of values in a saved state, so the saved words match SeqCore's exactly.
class PatternStore
{
public:
    PatternStore()
        : pageTable_ (tableSize, -1),
          pool_ (static_cast<std::size_t> (poolPages) * pageValues),
          free_ (poolPages),
          owner_ (poolPages),
          lengths_ (static_cast<std::size_t> (patterns) * lanes),
          marks_ (tableSize, 0)
    {
        reset();
    }

    void reset()
    {
        std::fill (pageTable_.begin(), pageTable_.end(), -1);
        for (int i = 0; i < poolPages; ++i)
        {
            free_[at (i)] = poolPages - 1 - i;
            owner_[at (i)] = -1;
        }
        freeTop_ = poolPages;
        nonDefault_ = 0;
        std::fill (lengths_.begin(), lengths_.end(), defaultLength);
        lengthCount_ = 0;
    }

    int get (int pattern, int lane, int step, int field) const
    {
        const int page = pageTable_[at (tableIndex (pattern, lane, step))];
        return page < 0 ? fieldDefault[at (field)] : pool_[at (cellIndex (page, step, field))];
    }

    void set (int pattern, int lane, int step, int field, int value)
    {
        const int table = tableIndex (pattern, lane, step);
        int page = pageTable_[at (table)];
        const int def = fieldDefault[at (field)];
        const int old = page < 0 ? def : pool_[at (cellIndex (page, step, field))];
        if (old == value)
            return;
        if (page < 0)
        {
            page = free_[at (--freeTop_)];
            pageTable_[at (table)] = page;
            owner_[at (page)] = table;
            for (int i = 0; i < pageValues; ++i)
                pool_[at (page * pageValues + i)] = fieldDefault[at (i % stepFields)];
        }
        pool_[at (cellIndex (page, step, field))] = value;
        if (old == def)
            ++nonDefault_;
        if (value == def)
        {
            --nonDefault_;
            if (pageIsDefault (page))
                freePage (table);
        }
    }

    int length (int pattern, int lane) const { return lengths_[at (pattern * lanes + lane)]; }

    void setLength (int pattern, int lane, int n)
    {
        int& length = lengths_[at (pattern * lanes + lane)];
        if (length != defaultLength && n == defaultLength)
            --lengthCount_;
        if (length == defaultLength && n != defaultLength)
            ++lengthCount_;
        length = n;
    }

    // Frees one lane's pages in one pattern and puts its length back to the default.
    void clearPatternLane (int pattern, int lane)
    {
        for (int k = 0; k < pagesPerLane; ++k)
        {
            const int table = (pattern * lanes + lane) * pagesPerLane + k;
            const int page = pageTable_[at (table)];
            if (page < 0)
                continue;
            for (int i = 0; i < pageValues; ++i)
                if (pool_[at (page * pageValues + i)] != fieldDefault[at (i % stepFields)])
                    --nonDefault_;
            freePage (table);
        }
        setLength (pattern, lane, defaultLength);
    }

    void clearLane (int lane)
    {
        for (int p = 0; p < patterns; ++p)
            clearPatternLane (p, lane);
    }

    void clearPattern (int pattern)
    {
        for (int s = 0; s < lanes; ++s)
            clearPatternLane (pattern, s);
    }

    int freePages() const { return freeTop_; }
    int nonDefaultCount() const { return nonDefault_; }
    int lengthCount() const { return lengthCount_; }

    // Counting the new pages a batch of edits would take. Call beginCount, then countPage for
    // each edit that sets a non-default value. countPage returns true the first time it sees a
    // page that is not yet allocated (or, with `ignoreAllocated`, any page).
    void beginCount()
    {
        // After four billion counts the stamp wraps, and old marks could match it again.
        if (++stamp_ == 0)
        {
            std::fill (marks_.begin(), marks_.end(), 0u);
            stamp_ = 1;
        }
    }

    bool countPage (int pattern, int lane, int step, bool ignoreAllocated = false)
    {
        const int table = tableIndex (pattern, lane, step);
        if ((! ignoreAllocated && pageTable_[at (table)] >= 0) || marks_[at (table)] == stamp_)
            return false;
        marks_[at (table)] = stamp_;
        return true;
    }

    // Calls f(pattern * lanes + lane, length) for each length that is not the default, in
    // pattern-then-lane order.
    template <typename F>
    void forEachLength (F&& f) const
    {
        for (int k = 0; k < patterns * lanes; ++k)
            if (lengths_[at (k)] != defaultLength)
                f (k, lengths_[at (k)]);
    }

    // Calls f(pattern * lanes + lane, step * stepFields + field, value) for each non-default
    // value, in pool page order, as SeqCore saves them.
    template <typename F>
    void forEachValue (F&& f) const
    {
        for (int page = 0; page < poolPages; ++page)
        {
            const int table = owner_[at (page)];
            if (table < 0)
                continue;
            for (int i = 0; i < pageValues; ++i)
            {
                const int value = pool_[at (page * pageValues + i)];
                if (value != fieldDefault[at (i % stepFields)])
                    f (table / pagesPerLane, (table % pagesPerLane) * pageValues + i, value);
            }
        }
    }

private:
    static constexpr int tableSize = patterns * lanes * pagesPerLane;

    static std::size_t at (int i) { return static_cast<std::size_t> (i); }

    static int tableIndex (int pattern, int lane, int step)
    {
        return (pattern * lanes + lane) * pagesPerLane + step / pageSteps;
    }

    static int cellIndex (int page, int step, int field)
    {
        return page * pageValues + (step % pageSteps) * stepFields + field;
    }

    bool pageIsDefault (int page) const
    {
        for (int i = 0; i < pageValues; ++i)
            if (pool_[at (page * pageValues + i)] != fieldDefault[at (i % stepFields)])
                return false;
        return true;
    }

    void freePage (int table)
    {
        const int page = pageTable_[at (table)];
        if (page < 0)
            return;
        free_[at (freeTop_++)] = page;
        owner_[at (page)] = -1;
        pageTable_[at (table)] = -1;
    }

    std::vector<int> pageTable_;
    std::vector<int> pool_;
    std::vector<int> free_;
    std::vector<int> owner_;
    std::vector<int> lengths_;
    std::vector<std::uint32_t> marks_;
    int freeTop_ = 0;
    int nonDefault_ = 0;
    int lengthCount_ = 0;
    std::uint32_t stamp_ = 0;
};

} // namespace furrow
