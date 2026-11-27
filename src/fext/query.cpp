// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fext/query.h>

#include <amount.h>
#include <fext/text.h>
#include <opreturn/format.h>
#include <opreturn/script.h>

#include <algorithm>
#include <limits>

namespace fext {
namespace {

using Clock = std::chrono::steady_clock;

//! The rows a slice asks the record for at a time.
constexpr size_t READ_ROWS{256};

bool Cancelled(const std::atomic<bool>* cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

/**
 * Whether a row's text holds the needle. The row holds the head of its script; the rest that the search looks at
 * is read with the slice's snapshot. A script that cannot be read is counted and does not match; a row whose text
 * was searched only in part is counted once the search has decided.
 */
MatchResult MatchText(interfaces::OpReturnRecord& record, const opreturn::RowView& row, const RowFilter& filter,
                      std::vector<unsigned char>& script, ScanResult& out, const std::atomic<bool>* cancel)
{
    Span<const unsigned char> bytes{row.script_prefix};
    bool in_part{false};
    if (row.script_size > row.script_prefix.size()) {
        const size_t limit = filter.whole_messages ? size_t{row.script_size} :
                                                     std::min<size_t>(row.script_size, TEXT_SEARCH_SCRIPT_BYTES);
        in_part = limit < row.script_size;
        if (limit > row.script_prefix.size()) {
            if (record.readScript(row.key, 0, limit, script, out.visibility) != opreturn::ReadResult::FOUND) {
                ++out.unreadable;
                return MatchResult::NO_MATCH;
            }
            bytes = script;
        }
    }
    const MatchResult result = FindFolded(SearchText(bytes, row.flags), *filter.text, cancel);
    if (in_part && result != MatchResult::CANCELLED) ++out.searched_in_part;
    return result;
}

} // namespace

TextNeedle::TextNeedle(Span<const unsigned char> typed)
{
    std::vector<uint32_t> decoded;
    DecodeUtf8(typed, decoded);
    m_folded = FoldForSearch(decoded, /*case_insensitive=*/true);
    m_border.assign(m_folded.size(), 0);
    size_t k = 0;
    for (size_t i = 1; i < m_folded.size(); ++i) {
        while (k > 0 && m_folded[i] != m_folded[k]) k = m_border[k - 1];
        if (m_folded[i] == m_folded[k]) ++k;
        m_border[i] = k;
    }
}

TextNeedle::TextNeedle(const std::string& typed) : TextNeedle(MakeUCharSpan(typed)) {}

MatchResult FindFolded(const std::vector<uint32_t>& text, const TextNeedle& needle, const std::atomic<bool>* cancel,
                       uint64_t* steps)
{
    const std::vector<uint32_t>& pattern = needle.Folded();
    const std::vector<size_t>& border = needle.Borders();
    if (pattern.empty()) return MatchResult::MATCH;
    uint64_t taken = 0;
    MatchResult result = MatchResult::NO_MATCH;
    // The number of the needle's first code points that the text ending at i matches.
    size_t matched = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i % CANCEL_INTERVAL == 0 && Cancelled(cancel)) {
            result = MatchResult::CANCELLED;
            break;
        }
        ++taken;
        while (matched > 0 && text[i] != pattern[matched]) {
            matched = border[matched - 1];
            ++taken;
        }
        if (text[i] == pattern[matched]) ++matched;
        if (matched == pattern.size()) {
            result = MatchResult::MATCH;
            break;
        }
    }
    if (steps) *steps += taken;
    return result;
}

std::vector<uint32_t> SearchText(Span<const unsigned char> script, uint16_t flags)
{
    const opreturn::ParsedScript parsed = opreturn::ParseOpReturnPrefix(script, flags, opreturn::PARSE_DATA);
    std::vector<uint32_t> decoded;
    DecodeUtf8(parsed.data, decoded);
    return FoldForSearch(decoded, /*case_insensitive=*/true);
}

bool IsBinary(const opreturn::RowView& row)
{
    return !(row.flags & opreturn::ROW_UTF8) || (row.flags & opreturn::ROW_EMPTY);
}

bool PaidBelow(const opreturn::TxBrief& tx, int64_t min_fee_rate)
{
    constexpr uint64_t LARGEST{std::numeric_limits<uint64_t>::max()};
    static_assert(uint64_t(MAX_MONEY) <= LARGEST / 1000, "fee * 1000 must fit in 64 bits");
    static_assert(uint64_t(MAX_MIN_FEE_RATE) <= LARGEST / std::numeric_limits<uint32_t>::max(), "rate * vsize too");
    if (min_fee_rate <= 0 || tx.fee < 0 || tx.vsize == 0) return false;
    // fee / vsize < rate / 1000 without rounding. A record holds no fee above MAX_MONEY, so both products fit.
    const uint64_t rate = uint64_t(std::min(min_fee_rate, MAX_MIN_FEE_RATE));
    const uint64_t fee = uint64_t(std::min(tx.fee, MAX_MONEY));
    return fee * 1000 < rate * tx.vsize;
}

bool PaidBy(const opreturn::RowView& row, const CScript& script)
{
    return row.tx.paid_by_kind == opreturn::PaidByKind::SCRIPT && row.tx.paid_by == script;
}

bool PassesRules(const opreturn::RowView& row, const RowFilter& filter)
{
    if (filter.hide_binary && IsBinary(row)) return false;
    if (PaidBelow(row.tx, filter.min_fee_rate)) return false;
    if (filter.paid_by && !PaidBy(row, *filter.paid_by)) return false;
    return true;
}

ScanResult Scan(interfaces::OpReturnRecord& record, const ScanRequest& request, const RowFilter& filter,
                const std::atomic<bool>* cancel)
{
    ScanResult out;
    out.next = request.from;
    out.visibility = request.visibility ? request.visibility : record.getVisibility();
    // A snapshot without coverage shows nothing.
    if (!out.visibility || !out.visibility->coverage) {
        out.end = true;
        return out;
    }
    const Clock::time_point deadline = Clock::now() + request.slice;
    const size_t max_found = std::max<size_t>(request.max_found, 1);
    const bool text = filter.text && !filter.text->Empty();
    std::vector<unsigned char> script;
    bool stop = false;
    while (!stop) {
        if (Cancelled(cancel)) {
            out.cancelled = true;
            break;
        }
        opreturn::RowRequest rows;
        rows.from = out.next;
        rows.older = request.older;
        rows.heights = request.heights;
        rows.max_rows = READ_ROWS;
        // Only a text search looks at the scripts.
        rows.max_script = text ? opreturn::SCRIPT_HEAD_SIZE : 0;
        rows.visibility = out.visibility;
        const opreturn::RowBatch batch = record.readRows(rows);
        for (const opreturn::RowView& row : batch.rows) {
            bool match = PassesRules(row, filter);
            if (match && text) {
                const MatchResult result = MatchText(record, row, filter, script, out, cancel);
                if (result == MatchResult::CANCELLED) {
                    out.cancelled = true;
                    stop = true;
                    break;
                }
                match = result == MatchResult::MATCH;
            }
            if (match) out.found.push_back(row.key);
            out.next = row.key;
            ++out.rows;
            if (out.found.size() >= max_found || Clock::now() >= deadline) {
                stop = true;
                break;
            }
            if (Cancelled(cancel)) {
                out.cancelled = true;
                stop = true;
                break;
            }
        }
        if (stop) break;
        if (batch.failed) {
            out.failed = true;
            break;
        }
        // The record returns at least one row unless it has ended or failed (a batch holds at least one row, and
        // max_rows is not 0). Were it ever to return none, reading on would read the same rows again: stop.
        if (batch.end || batch.rows.empty()) {
            out.end = true;
            break;
        }
    }
    // What the slice read holds only if no row was removed since its snapshot was taken (the counter is compared
    // with the snapshot's own, not with the record's current snapshot, which a removal replaces); a failed read may
    // also come from such a removal, and is then simply read again. A cancelled slice is checked too.
    if (record.getRemovals() != out.visibility->removals) {
        out.found.clear();
        out.next = request.from;
        out.end = false;
        out.failed = false;
        out.stale = true;
        out.rows = 0;
        out.searched_in_part = 0;
        out.unreadable = 0;
    }
    return out;
}

} // namespace fext
