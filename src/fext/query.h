// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FEXT_QUERY_H
#define BITCOIN_FEXT_QUERY_H

#include <interfaces/opreturn.h>
#include <optional.h>
#include <script/script.h>
#include <span.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

/**
 * The FEXT tab's queries of the OP_RETURN record: which rows a view of the tab shows, and the scan that finds them.
 * The tab's loader thread runs them. They read through interfaces::OpReturnRecord and nothing else, so they take no
 * node lock and wait for nothing but the disk.
 *
 * A scan runs in slices. Each slice reads with one snapshot of what the record shows (opreturn::Visibility), and
 * what it found counts only if no row was removed while it read: the record's removal counter still equals the
 * snapshot's after the slice's last read. A scan of several slices reads every slice with the snapshot of its first
 * (ScanResult::visibility, passed on in ScanRequest::visibility), so that its slices show one state of the record.
 * When a slice is stale, rows were removed since that snapshot was taken, and what every slice of the scan found may
 * name removed rows: the caller drops it and scans again from the start, with the record's current snapshot. (A
 * caller that applies the record's ROWS_REMOVED changes to what it found may instead go on from the stale slice's
 * position with the current snapshot.) A read that fails is not an error of the program: the record has counted it
 * and reads the same rows again itself, and the slice only says so, so that its caller can try again.
 */
namespace fext {

//! What a text search looks at of a longer message: the data in the first SCRIPT_PIECE_SIZE bytes of its script,
//! which the record gives in one read, unless the search asks for whole messages.
static constexpr size_t TEXT_SEARCH_SCRIPT_BYTES{opreturn::SCRIPT_PIECE_SIZE};
//! How long a slice of a scan runs. The time is checked after each row, so a slice runs over by the row it decides
//! and the read of the rows it began with: a row searched up to its first 64 KiB costs about a millisecond, one of a
//! megabyte searched whole about 20 ms (measured on a 2.1 GHz Xeon).
static constexpr std::chrono::milliseconds SCAN_SLICE{20};
//! The most rows a slice finds, unless asked for fewer.
static constexpr size_t SCAN_MAX_FOUND{2048};
//! The substring search reads the cancel flag at least once in this many code points of a message's text.
static constexpr size_t CANCEL_INTERVAL{65536};
//! The highest minimum fee rate a view takes, in atoms per 1,000 virtual bytes.
static constexpr int64_t MAX_MIN_FEE_RATE{100000000};

enum class MatchResult : uint8_t {
    NO_MATCH = 0,
    MATCH = 1,
    CANCELLED = 2, //!< the cancel flag was set before the search could decide
};

/**
 * What a text search looks for: the text the user typed, decoded and folded as the text of every message is
 * (FoldForSearch, with simple case folding). So it is found whatever invisible characters, white space characters
 * and letter case a message uses.
 */
class TextNeedle
{
public:
    //! The needle for typed text in UTF-8; an ill-formed sequence becomes U+FFFD, as it does in a message.
    explicit TextNeedle(Span<const unsigned char> typed);
    explicit TextNeedle(const std::string& typed);

    //! The folded text. It is empty when the typed text is, or holds only characters the folding drops; an empty
    //! needle occurs in every text.
    const std::vector<uint32_t>& Folded() const { return m_folded; }
    bool Empty() const { return m_folded.empty(); }
    //! For each prefix of the folded text, the length of its longest proper prefix that is also its suffix.
    const std::vector<size_t>& Borders() const { return m_border; }

private:
    std::vector<uint32_t> m_folded;
    std::vector<size_t> m_border;
};

/**
 * Whether the needle occurs in folded text, as a plain substring. It makes one pass over the text (the algorithm of
 * Knuth, Morris and Pratt), so its time is linear in the length of the text, whatever the needle; it reads `cancel`
 * before the text's first code point and then before every CANCEL_INTERVAL-th. This is the one function a pattern
 * matcher would replace. With `steps`, it adds the steps it took: one for each code point of the text and one for
 * each fall-back to a shorter partial match, together at most twice the length of the text.
 */
MatchResult FindFolded(const std::vector<uint32_t>& text, const TextNeedle& needle,
                       const std::atomic<bool>* cancel = nullptr, uint64_t* steps = nullptr);

/**
 * The text a search looks at: the data of an OP_RETURN script, or of the beginning of a longer one, decoded and
 * folded as for a case-insensitive search. `script` holds the script or its first bytes, and `flags` are the row
 * flags of the whole script (opreturn::RowFlag). It reads no cancel flag, and costs most of a text search's time
 * (SCAN_SLICE gives what a row costs).
 */
std::vector<uint32_t> SearchText(Span<const unsigned char> script, uint16_t flags);

//! What a view shows: the rows that pass its rules and, while it searches, match its search.
struct RowFilter {
    //! Hide the messages that are not text: data that is not well-formed UTF-8, and empty data.
    bool hide_binary{true};
    //! Hide the messages whose transaction paid less than this many atoms per 1,000 virtual bytes; a message whose
    //! fee is not known is shown. 0 hides none; a value above MAX_MIN_FEE_RATE counts as that.
    int64_t min_fee_rate{0};
    //! Show only the messages paid for by a coin with this script.
    Optional<CScript> paid_by;
    //! Show only the messages whose text holds this needle; null: no text search.
    std::shared_ptr<const TextNeedle> text;
    //! Search the whole script of a long message, not only the data in its first TEXT_SEARCH_SCRIPT_BYTES bytes.
    bool whole_messages{false};
};

//! Whether a row's data is not text: not well-formed UTF-8, or empty.
bool IsBinary(const opreturn::RowView& row);

//! Whether a transaction paid less than `min_fee_rate` atoms per 1,000 virtual bytes. False when its fee or its
//! size is not known, and for a rate of 0 or less.
bool PaidBelow(const opreturn::TxBrief& tx, int64_t min_fee_rate);

//! Whether a coin with this script paid for the row's transaction (opreturn::PaidByKind::SCRIPT).
bool PaidBy(const opreturn::RowView& row, const CScript& script);

//! Whether a row passes the filter's rules that need nothing but the row: all of them but the text search.
bool PassesRules(const opreturn::RowView& row, const RowFilter& filter);

//! Where a slice of a scan begins, which way it reads, and when it ends.
struct ScanRequest {
    //! The slice reads the rows before this position (newest first) or after it (oldest first).
    opreturn::RowKey from{opreturn::ROW_KEY_AFTER_ALL};
    bool older{true};
    //! No row outside these heights is read.
    opreturn::HeightRange heights{0, std::numeric_limits<int32_t>::max()};
    //! The slice ends when it has found this many rows (at least one).
    size_t max_found{SCAN_MAX_FOUND};
    //! ... or when this time has passed, which it checks after each row; it decides one row at least.
    std::chrono::milliseconds slice{SCAN_SLICE};
    //! The snapshot to read with; null: the record's current one. Every slice of a scan after its first passes the
    //! first slice's (ScanResult::visibility).
    opreturn::VisibilityRef visibility;
};

//! What a slice found, and where the next one begins.
struct ScanResult {
    //! The rows that pass the filter, in the order read.
    std::vector<opreturn::RowKey> found;
    //! Where the next slice begins: the last row this one decided, or the request's position.
    opreturn::RowKey next;
    //! No visible row is left in this direction, within the heights asked for.
    bool end{false};
    //! The cancel flag was set: the slice stopped after `next`. A cancelled slice is checked for removals too, so it
    //! may also be stale; then what it found does not count either.
    bool cancelled{false};
    //! A read of rows failed: the slice stopped after `next`, and what it found before holds. The record has
    //! counted the failure and reads the same rows again itself; the caller may try again.
    bool failed{false};
    //! Rows were removed since the snapshot the slice read with was taken, so nothing it found counts: `found` is
    //! empty, `next` is the request's position, the counts are 0 and `end` and `failed` are false. Nor does what the
    //! scan's earlier slices found under that snapshot: the caller scans again from the start (see the top of this
    //! file).
    bool stale{false};
    //! The rows the slice decided.
    uint64_t rows{0};
    //! Of them, the long messages whose text was searched, but only in part.
    uint64_t searched_in_part{0};
    //! Of them, the messages whose script could not be read for the text search; they do not match it.
    uint64_t unreadable{0};
    //! The snapshot the slice read with.
    opreturn::VisibilityRef visibility;
};

/**
 * One slice of a scan of the record's rows with a filter. Every read of the slice uses one snapshot, the one asked
 * for or the record's current one; a text search reads the rest of a long script with readScript. Cancelled through
 * `cancel`, which it reads before each read of rows, after each row and in the substring search (FindFolded), but not
 * while a message's text is decoded and folded. Never throws for the record's sake, and takes no lock of its own.
 */
ScanResult Scan(interfaces::OpReturnRecord& record, const ScanRequest& request, const RowFilter& filter,
                const std::atomic<bool>* cancel = nullptr);

} // namespace fext

#endif // BITCOIN_FEXT_QUERY_H
