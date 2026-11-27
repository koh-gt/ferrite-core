// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_OPRETURN_H
#define BITCOIN_TEST_UTIL_OPRETURN_H

#include <interfaces/opreturn.h>
#include <primitives/transaction.h>
#include <span.h>
#include <sync.h>

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace opreturn {
namespace test {

//! OP_RETURN and one push of `data`, with the smallest push opcode that holds it.
std::vector<unsigned char> OneDataPush(Span<const unsigned char> data);

/**
 * An OP_RETURN script of exactly `size` bytes (at least 1): one push of the smallest kind that gives that size,
 * whose data is the byte `fill` repeated, or bytes made from `seed` when `fill` is negative. A script of one byte is
 * OP_RETURN alone, and one of two bytes OP_RETURN OP_0.
 */
std::vector<unsigned char> MakeScript(size_t size, int fill, uint32_t seed = 0);

//! A transaction whose outputs have the given scripts and the value 0: a coinbase, or a transaction with one input.
CMutableTransaction MakeOpReturnTx(const std::vector<std::vector<unsigned char>>& scripts, bool coinbase = false);

//! A sample of bytes that anyone can write into an OP_RETURN output.
struct HostileSample {
    std::string name;
    std::vector<unsigned char> bytes;
    bool utf8; //!< whether the bytes are well-formed UTF-8
};

/**
 * Bytes that must never reach a log line or an error message: markup, a link, terminal escape sequences, direction
 * controls, invisible characters, a NUL, line breaks with a forged log line, format directives, and malformed
 * UTF-8. Each sample holds HOSTILE_MARKER, so that a test can search for it.
 */
std::vector<HostileSample> HostileCorpus();

//! The text every hostile sample holds.
extern const std::string HOSTILE_MARKER;

//! Collects every line written to the debug log while it exists.
class LogLines
{
    mutable Mutex m_mutex;
    std::vector<std::string> m_lines GUARDED_BY(m_mutex);
    std::list<std::function<void(const std::string&)>>::iterator m_callback;

public:
    LogLines();
    ~LogLines();
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;

    std::vector<std::string> Lines() const;
    //! The lines that contain `text`.
    std::vector<std::string> Containing(const std::string& text) const;
};

/**
 * A record for the tests of its readers: rows kept in memory and shown through a snapshot the test sets. readRows,
 * readRow and readScript keep the contract of interfaces::OpReturnRecord; the other reads find nothing, and the
 * switches and listeners do nothing. A test can make reads fail, raise the removal counter as the record does
 * before a batch that removes rows, and run code before each read.
 */
class FakeRecord final : public interfaces::OpReturnRecord
{
public:
    FakeRecord();

    //! Adds a row with its whole script. Its flags, sizes and script head are made from the script, as the record
    //! makes them.
    void AddRow(const RowKey& key, const std::vector<unsigned char>& script, const TxBrief& tx = {});
    //! Adds a row as a record gave it, with its whole script, which replaces the row's script head.
    void AddView(RowView row, const std::vector<unsigned char>& script);
    //! Publishes a new snapshot that covers these heights (canonical ranges), with this floor and tip (-1: the
    //! highest height covered). Its removal count is the counter's value now.
    void Show(std::vector<HeightRange> coverage, int32_t floor = -1, int32_t live_tip = -1);
    //! Raises the removal counter, as the record does before it writes a batch that removes rows.
    void RaiseRemovals();
    //! readRows fails when it comes to this row, and readRow and readScript of this row fail.
    void FailRow(const RowKey& key);
    //! readScript of this row fails.
    void FailScript(const RowKey& key);
    //! Called before each read with the snapshot the read uses (the one given, or the current one).
    void BeforeRead(std::function<void(const VisibilityRef&)> fn);

    //! The snapshots of the reads made, in order, and how many of them failed.
    std::vector<VisibilityRef> ReadSnapshots() const;
    uint64_t ReadErrors() const;

    // interfaces::OpReturnRecord
    RecordStatus getStatus() override;
    VisibilityRef getVisibility() override;
    uint64_t getRemovals() override;
    RowBatch readRows(const RowRequest& request) override;
    ReadResult readRow(const RowKey& key, RowView& out, const VisibilityRef& visibility) override;
    ReadResult readScript(const RowKey& key, size_t offset, size_t max_bytes, std::vector<unsigned char>& out,
                          const VisibilityRef& visibility) override;
    ReadResult readTransaction(const TxKey& key, size_t max_row_script, size_t max_spent_script, TxView& out,
                               const VisibilityRef& visibility) override;
    ReadResult readBlock(int32_t height, BlockView& out, const VisibilityRef& visibility) override;
    ReadResult findTransaction(const uint256& txid, TxKey& out, const VisibilityRef& visibility) override;
    ReadResult findHeightForTime(int64_t time, int32_t& out, const VisibilityRef& visibility) override;
    bool waitForTip(int timeout_ms) override;
    void setRecordEnabled(bool enabled) override;
    void requestRebuild() override;
    std::unique_ptr<interfaces::Handler> handleChanged(std::function<void(const RecordChange&)> fn) override;

private:
    struct Entry {
        RowView row;                       //!< with the whole script as its head
        std::vector<unsigned char> script; //!< the whole script
    };

    //! The snapshot a read uses, after the test's code for it has run.
    VisibilityRef Use(const VisibilityRef& given);
    //! The row as a read gives it, with at most `max_script` bytes of its script.
    static RowView View(const Entry& entry, size_t max_script);

    mutable Mutex m_mutex;
    std::map<RowKey, Entry> m_rows GUARDED_BY(m_mutex);
    std::set<std::pair<int32_t, uint32_t>> m_fail_rows GUARDED_BY(m_mutex);
    std::set<std::pair<int32_t, uint32_t>> m_fail_scripts GUARDED_BY(m_mutex);
    VisibilityRef m_visibility GUARDED_BY(m_mutex);
    uint64_t m_removals GUARDED_BY(m_mutex){0};
    uint64_t m_generation GUARDED_BY(m_mutex){0};
    uint64_t m_read_errors GUARDED_BY(m_mutex){0};
    std::vector<VisibilityRef> m_read_snapshots GUARDED_BY(m_mutex);
    std::function<void(const VisibilityRef&)> m_before_read GUARDED_BY(m_mutex);
};

} // namespace test
} // namespace opreturn

#endif // BITCOIN_TEST_UTIL_OPRETURN_H
