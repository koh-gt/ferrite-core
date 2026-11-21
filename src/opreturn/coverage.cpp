// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/coverage.h>

#include <algorithm>
#include <limits>

namespace opreturn {
namespace {

constexpr int32_t MAX_HEIGHT{std::numeric_limits<int32_t>::max()};

//! The first range whose last height is at or above `height`.
std::vector<HeightRange>::const_iterator FirstEndingAtOrAbove(const std::vector<HeightRange>& ranges, int32_t height)
{
    return std::lower_bound(ranges.begin(), ranges.end(), height,
                            [](const HeightRange& range, int32_t h) { return range.hi < h; });
}

} // namespace

bool IsCanonical(const std::vector<HeightRange>& ranges)
{
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].lo < 0 || ranges[i].lo > ranges[i].hi) return false;
        // At least one height between this range and the one before.
        if (i > 0 && int64_t{ranges[i - 1].hi} + 1 >= int64_t{ranges[i].lo}) return false;
    }
    return true;
}

bool HeightSet::FromRanges(std::vector<HeightRange> ranges, HeightSet& out)
{
    if (!IsCanonical(ranges)) return false;
    out.m_ranges = std::move(ranges);
    return true;
}

uint64_t HeightSet::HeightCount() const
{
    uint64_t count = 0;
    for (const HeightRange& range : m_ranges) count += uint64_t(int64_t{range.hi} - range.lo + 1);
    return count;
}

bool HeightSet::Contains(int32_t height) const
{
    const auto it = FirstEndingAtOrAbove(m_ranges, height);
    return it != m_ranges.end() && it->lo <= height;
}

void HeightSet::Add(HeightRange range)
{
    if (range.lo < 0 || range.lo > range.hi) return;
    // The first range that ends at most one height before the new one begins can join it.
    auto first = std::lower_bound(m_ranges.begin(), m_ranges.end(), range.lo,
                                  [](const HeightRange& r, int32_t lo) { return int64_t{r.hi} + 1 < lo; });
    auto last = first;
    while (last != m_ranges.end() && int64_t{last->lo} <= int64_t{range.hi} + 1) {
        range.lo = std::min(range.lo, last->lo);
        range.hi = std::max(range.hi, last->hi);
        ++last;
    }
    first = m_ranges.erase(first, last);
    m_ranges.insert(first, range);
}

void HeightSet::Remove(HeightRange range)
{
    if (range.lo > range.hi) return;
    std::vector<HeightRange> kept;
    kept.reserve(m_ranges.size() + 1);
    for (const HeightRange& r : m_ranges) {
        if (r.hi < range.lo || r.lo > range.hi) {
            kept.push_back(r);
            continue;
        }
        if (r.lo < range.lo) kept.push_back({r.lo, range.lo - 1});
        if (r.hi > range.hi) kept.push_back({range.hi + 1, r.hi});
    }
    m_ranges.swap(kept);
}

void HeightSet::RemoveAbove(int32_t height)
{
    if (height == MAX_HEIGHT) return;
    Remove({std::max(height + 1, 0), MAX_HEIGHT});
}

void HeightSet::RemoveBelow(int32_t height)
{
    if (height <= 0) return;
    Remove({0, height - 1});
}

std::vector<HeightRange> HeightSet::Missing(HeightRange window) const
{
    std::vector<HeightRange> missing;
    window.lo = std::max(window.lo, 0);
    if (window.lo > window.hi) return missing;
    int64_t next = window.lo; // the lowest height not yet looked at
    for (auto it = FirstEndingAtOrAbove(m_ranges, window.lo); it != m_ranges.end() && it->lo <= window.hi; ++it) {
        if (it->lo > next) missing.push_back({int32_t(next), it->lo - 1});
        next = int64_t{it->hi} + 1;
    }
    if (next <= window.hi) missing.push_back({int32_t(next), window.hi});
    return missing;
}

Optional<int32_t> HeightSet::HighestMissing(HeightRange window) const
{
    window.lo = std::max(window.lo, 0);
    if (window.lo > window.hi) return nullopt;
    const auto it = FirstEndingAtOrAbove(m_ranges, window.hi);
    if (it == m_ranges.end() || it->lo > window.hi) return window.hi;
    // window.hi lies in *it. The height below it belongs to no range, because ranges never touch.
    if (it->lo <= window.lo) return nullopt;
    return it->lo - 1;
}

} // namespace opreturn
