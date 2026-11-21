// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_COVERAGE_H
#define BITCOIN_OPRETURN_COVERAGE_H

#include <optional.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * Which heights the OP_RETURN record holds. Its coverage is the set of heights whose rows it has written. A height
 * from its floor up to its tip that it does not hold is in a gap, with a reason; heights below the floor were
 * removed to keep the record under its size limit.
 */
namespace opreturn {

//! The heights from lo to hi, both included.
struct HeightRange {
    int32_t lo{0};
    int32_t hi{0};

    friend bool operator==(const HeightRange& a, const HeightRange& b) { return a.lo == b.lo && a.hi == b.hi; }
    friend bool operator!=(const HeightRange& a, const HeightRange& b) { return !(a == b); }
};

//! Why the record does not hold a height that is not below its floor.
enum class GapReason : uint8_t {
    PENDING = 0,    //!< not read yet; the record reads it in this run
    PRUNED = 1,     //!< its block was deleted before the record read it
    UNREADABLE = 2, //!< its block could not be read, or failed its checks; it is tried again at the next start
    SKIPPED = 3,    //!< left unread in this run because the list of covered ranges is full
};

//! A range of heights the record does not hold, and why.
struct GapRange {
    int32_t lo{0};
    int32_t hi{0};
    GapReason reason{GapReason::PENDING};

    friend bool operator==(const GapRange& a, const GapRange& b) { return a.lo == b.lo && a.hi == b.hi && a.reason == b.reason; }
    friend bool operator!=(const GapRange& a, const GapRange& b) { return !(a == b); }
};

//! The most ranges the record's coverage holds.
static constexpr size_t MAX_COVERAGE_RANGES{10000};

/**
 * Whether ranges are in the form a HeightSet keeps: every range has 0 <= lo <= hi, the ranges are in height order,
 * and between two ranges lies at least one height that neither holds.
 */
bool IsCanonical(const std::vector<HeightRange>& ranges);

//! A set of block heights (0 to 2147483647), kept as ranges in the form IsCanonical describes.
class HeightSet
{
    std::vector<HeightRange> m_ranges;

public:
    HeightSet() = default;

    //! The set of the given ranges; false, and `out` unchanged, if they are not canonical.
    static bool FromRanges(std::vector<HeightRange> ranges, HeightSet& out);

    const std::vector<HeightRange>& Ranges() const { return m_ranges; }
    bool Empty() const { return m_ranges.empty(); }
    //! The number of ranges.
    size_t RangeCount() const { return m_ranges.size(); }
    //! The number of heights.
    uint64_t HeightCount() const;

    bool Contains(int32_t height) const;

    //! Adds the heights of a range. A range with lo above hi or below 0 changes nothing.
    void Add(HeightRange range);
    //! Removes the heights of a range. A range with lo above hi changes nothing.
    void Remove(HeightRange range);
    //! Removes every height above `height`.
    void RemoveAbove(int32_t height);
    //! Removes every height below `height`.
    void RemoveBelow(int32_t height);

    //! The heights of `window` (from height 0 on) that the set does not hold, as ranges in height order.
    std::vector<HeightRange> Missing(HeightRange window) const;
    //! The highest height of `window` (from height 0 on) that the set does not hold.
    Optional<int32_t> HighestMissing(HeightRange window) const;

    friend bool operator==(const HeightSet& a, const HeightSet& b) { return a.m_ranges == b.m_ranges; }
    friend bool operator!=(const HeightSet& a, const HeightSet& b) { return !(a == b); }
};

} // namespace opreturn

#endif // BITCOIN_OPRETURN_COVERAGE_H
