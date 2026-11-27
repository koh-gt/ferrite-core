// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_OPRETURN_H
#define BITCOIN_INTERFACES_OPRETURN_H

#include <amount.h>
#include <coins.h>
#include <merkleblock.h>
#include <opreturn/coverage.h>
#include <opreturn/format.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace interfaces {
class Handler;
} // namespace interfaces

/**
 * What the OP_RETURN record offers its readers: its state, the snapshot of what a reader may see, and reads of its
 * rows, scripts, transactions and blocks. The value types are plain structures.
 */
namespace opreturn {

enum class State : uint8_t {
    OFF = 0,             //!< not kept
    OPENING = 1,         //!< being opened
    RUNNING = 2,         //!< every height from its floor to its tip is covered
    BACKFILLING = 3,     //!< heights at or below its tip are still to be read
    PAUSED_LOW_DISK = 4, //!< writes nothing until disk space returns
    STOPPING = 5,        //!< writing what is queued before it stops
    STOPPED = 6,         //!< stopped with the node; reads find nothing
    FAILED = 7,          //!< stopped by an error; what it had written stays readable
};

//! Why the record failed.
enum class FailReason : uint8_t {
    NONE = 0,
    OPEN_FAILED = 1,  //!< its database could not be opened, or what it stores about itself could not be read
    NEWER_FORMAT = 2, //!< its database was made by a newer version
    OTHER_CHAIN = 3,  //!< its database belongs to another chain
    WRITE_FAILED = 4, //!< a write failed, or a value could not be stored
    READ_FAILED = 5,  //!< a read failed, and failed again when the record's worker read the same key
    INTERNAL = 6,     //!< an unexpected error
};

//! The number of states and of fail reasons; every value below it is one.
static constexpr uint8_t STATE_COUNT{8};
static constexpr uint8_t FAIL_REASON_COUNT{7};

/**
 * What a reader may see. Immutable: the record publishes a new one after the batch it describes is written, so a
 * reader never sees a height covered before its rows are written. A row at height h is visible when h is covered,
 * not below the floor and not above live_tip; every covered height up to live_tip holds exactly the rows of the
 * block that is the ancestor of the record's tip block at that height.
 */
struct Visibility {
    //! The covered heights, as canonical ranges (IsCanonical).
    std::shared_ptr<const std::vector<HeightRange>> coverage;
    //! Every height from the floor (or from 0) up to live_tip that is not covered, as ranges in height order, each
    //! with its reason.
    std::shared_ptr<const std::vector<GapRange>> gaps;
    int32_t floor{-1};     //!< heights below it were removed to keep the record under its size limit; -1: none
    int32_t live_tip{-1};  //!< the height of the record's tip block; -1 while it has none
    uint256 live_tip_hash; //!< the hash of the record's tip block; null while live_tip is -1
    //! The removal counter when this snapshot was published (OpReturnRecord::getRemovals).
    uint64_t removals{0};
    uint64_t generation{0}; //!< grows with every published snapshot

    //! Whether the rows of a height are visible.
    bool IsVisible(int32_t height) const
    {
        if (!coverage || height < 0 || height < floor || height > live_tip) return false;
        const auto it = std::lower_bound(coverage->begin(), coverage->end(), height,
                                         [](const HeightRange& range, int32_t h) { return range.hi < h; });
        return it != coverage->end() && it->lo <= height;
    }
};

using VisibilityRef = std::shared_ptr<const Visibility>;

//! The record's state for its readers. Copies of snapshots: making it takes no node lock and waits for nothing.
struct RecordStatus {
    State state{State::OFF};
    FailReason reason{FailReason::NONE};
    bool pruned{false};          //!< the node prunes blocks
    uint8_t explicit_options{0}; //!< ExplicitOption bits
    //! The node's state from its notifications; node_known stays false until start() has read it.
    bool node_known{false};
    bool initial_download{false};
    int32_t node_tip{-1};
    int64_t node_tip_time{0};
    uint256 node_tip_hash;
    int32_t header_tip{-1};
    int64_t header_tip_time{0};
    int32_t live_tip{-1};
    uint256 live_tip_hash;
    int32_t floor{-1};
    std::shared_ptr<const std::vector<HeightRange>> coverage;
    //! The highest height at or below live_tip that the record still has to read; -1 when there is none.
    int32_t backfill_height{-1};
    //! The lowest visible height and the time of its block; -1 and 0 when no height is visible.
    int32_t oldest_visible{-1};
    int64_t oldest_visible_time{0};
    uint64_t rows{0};
    uint64_t txs{0};
    uint64_t read_errors{0}; //!< reads by readers that failed since the record was made
    uint64_t generation{0};  //!< of the snapshot of visibility this status describes
};

//! A row as readers get it: the facts of the output, at most the first SCRIPT_HEAD_SIZE bytes of its script, and its
//! transaction's brief.
struct RowView {
    RowKey key;
    uint32_t txpos{0};
    uint32_t vout{0};
    uint16_t flags{0};
    CAmount value{0};
    uint32_t script_size{0};                  //!< of the whole script
    uint32_t data_size{0};                    //!< of the data of the whole script
    std::vector<unsigned char> script_prefix; //!< at most SCRIPT_HEAD_SIZE bytes; the rest comes from readScript
    TxBrief tx;
};

//! The position before every row, for a read in chain order, and the position after every row, for a read newest
//! first. A position (h, n) with n the largest uint32_t stands after every row of height h.
static constexpr RowKey ROW_KEY_BEFORE_ALL{-1, std::numeric_limits<uint32_t>::max()};
static constexpr RowKey ROW_KEY_AFTER_ALL{std::numeric_limits<int32_t>::max(), std::numeric_limits<uint32_t>::max()};

struct RowRequest {
    RowKey from{ROW_KEY_BEFORE_ALL}; //!< exclusive
    bool older{false};               //!< true: newest first, the rows before `from`; false: the rows after it
    HeightRange heights{0, std::numeric_limits<int32_t>::max()}; //!< no row outside these heights is read
    size_t max_rows{512};
    size_t max_script{SCRIPT_HEAD_SIZE}; //!< script bytes per row; more than SCRIPT_HEAD_SIZE has no effect
    size_t max_bytes{4 << 20};           //!< script bytes in all, each row's paying script included; a batch
                                         //!< holds at least one row
    VisibilityRef visibility;            //!< null: the current snapshot
};

struct RowBatch {
    std::vector<RowView> rows;
    RowKey next;            //!< where to continue: the last row returned, or `from` when none was
    bool end{false};        //!< no further visible row in this direction and within the heights asked for
    bool failed{false};     //!< a read of the record's files failed; `rows` holds what was read before
    uint64_t generation{0}; //!< of the snapshot the batch was read with
};

enum class ReadResult : uint8_t {
    FOUND = 0,
    NOT_FOUND = 1, //!< not visible in the snapshot, or not in the record
    FAILED = 2,    //!< a read of the record's files failed; the record's count of failed reads grew
};

struct TxInView {
    COutPoint prevout;
    CScript script_sig;
    uint32_t sequence{0};
    SpentState spent_state{SpentState::PENDING};
    Coin spent; //!< for SpentState::KNOWN; its script is left out when it is longer than the read was allowed
    uint32_t spent_script_size{0}; //!< for SpentState::KNOWN: the size of the coin's whole script
};

struct TxOutView {
    CAmount value{0};
    bool is_row{false};
    uint32_t row_n{0};       //!< the row's number in the block, for a row
    CScript script;          //!< the whole script, or for a row as much of it as was asked for
    uint32_t script_size{0}; //!< of the whole script
};

//! A transaction with rows, without its witnesses and its MWEB part.
struct TxView {
    TxKey key;
    uint256 txid;
    int32_t version{0};
    uint32_t locktime{0};
    uint32_t size{0};
    uint32_t weight{0};
    uint32_t vsize{0};
    uint32_t tx_flags{0}; //!< TxFlag bits
    std::vector<TxInView> vin;
    std::vector<TxOutView> vout;
    uint32_t n_pegin{0};
    uint32_t n_pegout{0};
    uint32_t n_kernel{0};
    CAmount fee{-1}; //!< -1: not known
    uint256 block_hash;
    uint32_t block_time{0};

    //! The scriptSig of a coinbase, which is the scriptSig of its one input.
    const CScript& CoinbaseScriptSig() const
    {
        static const CScript EMPTY;
        return vin.empty() ? EMPTY : vin[0].script_sig;
    }

    //! The transaction as it hashes to its txid: without witnesses and without an MWEB part. False when the script
    //! of an output was not read whole.
    bool ToMutableTransaction(CMutableTransaction& out) const
    {
        out = CMutableTransaction();
        out.nVersion = version;
        out.nLockTime = locktime;
        for (const TxInView& in : vin) out.vin.emplace_back(in.prevout, in.script_sig, in.sequence);
        for (const TxOutView& o : vout) {
            if (o.script.size() != o.script_size) return false;
            out.vout.emplace_back(o.value, o.script);
        }
        return true;
    }
};

struct BlockView {
    int32_t height{0};
    uint256 hash;
    uint32_t time{0};
    uint32_t ntx{0};
    uint32_t nrows{0};
    CPartialMerkleTree proof; //!< proves the transactions with rows against the block's merkle root
};

//! What changed, for the record's listeners. Changes are joined to at most one notification each NOTIFY_INTERVAL,
//! except that a change to FAILED or STOPPED is sent at once.
struct RecordChange {
    enum Kind : uint8_t { STATUS = 1, ROWS_ADDED = 2, ROWS_REMOVED = 4, ROWS_UPDATED = 8 };
    uint8_t kinds{0};
    HeightRange added{0, -1};   //!< the heights whose rows became visible (lo above hi: none)
    HeightRange removed{0, -1}; //!< the heights whose rows stopped being visible, or were written again
    HeightRange updated{0, -1}; //!< the heights whose rows got the coins their transactions spend
    uint32_t new_rows{0};
    uint64_t generation{0};
};

} // namespace opreturn

namespace interfaces {

/**
 * The OP_RETURN record, as its readers use it.
 *
 * The read methods may be called from any thread that may wait for the disk and holds no node lock, any number at
 * once; they take no node lock. Each reads with the snapshot it is given, or with the current one when it is given
 * none, and returns only what that snapshot shows. What a caller read is the state the snapshot describes when
 * getRemovals() still equals the snapshot's `removals` after its last read. A read that fails, also one that finds
 * the record's keys disagreeing with each other, returns FAILED (or a batch with `failed`) and counts the failure;
 * the record's worker then makes the same read itself, and the record fails only when that read fails too. Every
 * method is safe after the record has stopped: reads find nothing.
 */
class OpReturnRecord
{
public:
    virtual ~OpReturnRecord() {}

    //! Cheap; any thread. No node lock, no wait.
    virtual opreturn::RecordStatus getStatus() = 0;
    //! Cheap; any thread. The snapshot the next read would use.
    virtual opreturn::VisibilityRef getVisibility() = 0;
    //! Cheap; any thread. The removal counter: the record raises it before it writes any batch that erases rows or
    //! takes a height out of coverage. A reader that made its reads with a snapshot whose `removals` still equals
    //! it afterwards read the state that snapshot describes.
    virtual uint64_t getRemovals() = 0;

    virtual opreturn::RowBatch readRows(const opreturn::RowRequest& request) = 0;
    virtual opreturn::ReadResult readRow(const opreturn::RowKey& key, opreturn::RowView& out,
                                         const opreturn::VisibilityRef& visibility) = 0;
    //! Bytes [offset, offset + max_bytes) of a row's script, cut at its end.
    virtual opreturn::ReadResult readScript(const opreturn::RowKey& key, size_t offset, size_t max_bytes,
                                            std::vector<unsigned char>& out,
                                            const opreturn::VisibilityRef& visibility) = 0;
    /**
     * A transaction with rows; the script of each of its rows is read up to max_row_script bytes. The script of a
     * spent coin longer than SPENT_SCRIPT_INLINE_SIZE is read only while such scripts, in input order, come to at
     * most max_spent_script bytes in all; the rest are left out (TxInView::spent_script_size tells their sizes).
     */
    virtual opreturn::ReadResult readTransaction(const opreturn::TxKey& key, size_t max_row_script,
                                                 size_t max_spent_script, opreturn::TxView& out,
                                                 const opreturn::VisibilityRef& visibility) = 0;
    //! A block with rows: its facts and its proof.
    virtual opreturn::ReadResult readBlock(int32_t height, opreturn::BlockView& out,
                                           const opreturn::VisibilityRef& visibility) = 0;
    virtual opreturn::ReadResult findTransaction(const uint256& txid, opreturn::TxKey& out,
                                                 const opreturn::VisibilityRef& visibility) = 0;
    /**
     * A visible height with rows whose block time is at least `time`, such that the visible height with rows below
     * it, if there is one, has a block time below `time`. While the block times rise with the heights, that is the
     * lowest visible height with rows whose block time is at least `time`. Block times may also go back a little,
     * and then it is one of the heights where the times pass `time`, near the lowest. NOT_FOUND only when no visible
     * height has rows or the highest visible height with rows has a block time below `time`. A binary search over
     * the record's block entries: two seeks for each halving of the visible heights, and two more for each range of
     * the coverage without rows that a halving passes, so a coverage of many ranges makes it slower (with the most
     * ranges a coverage holds, tens of milliseconds).
     */
    virtual opreturn::ReadResult findHeightForTime(int64_t time, int32_t& out, const opreturn::VisibilityRef& visibility) = 0;

    /**
     * Waits at most timeout_ms until the node's best block as it was when the call began (or, once the node has left
     * that block, its best block then) is the record's tip block or an ancestor of it, and the record's tip block is
     * the node's current best block or an ancestor of it. Returns whether both hold, and false as soon as the record
     * leaves RUNNING and BACKFILLING. Returns false at once while the record is neither RUNNING nor BACKFILLING, or the
     * node has not told the record its best block; and, when the record has not caught up as the call begins, while
     * the node is in its initial block download, after a wait that ran out until a call finds the record caught up,
     * and for a timeout_ms of zero or less, which only looks and so notes no stall. Any thread that holds no node
     * lock; never a validation callback.
     */
    virtual bool waitForTip(int timeout_ms) = 0;

    //! For a program that shows the record in a GUI. Any thread; never waits: the record's worker does the work.
    //! Switching off stops the record and keeps its files; switching on opens them again. Asked while the worker
    //! ends its run, a switch is made once the run has ended (the last one asked counts). Before init has loaded the
    //! record (its chain client's load()), a switch does nothing.
    virtual void setRecordEnabled(bool enabled) = 0;
    //! Deletes the record and starts a new one at the node's tip; asked while the worker ends its run, or after the
    //! record failed, once the run has ended. Before init has loaded the record, it does nothing.
    virtual void requestRebuild() = 0;

    /**
     * `fn` is called on the record's worker, or on a thread that starts, stops or fails the record (a validation
     * callback's thread too), never under the record's lock. It must not block, and it may call setRecordEnabled
     * and requestRebuild. An exception it throws is dropped. It must not call the record's stop(), which does nothing
     * there, and must not let go of the last reference to the record: the record would then end as if the process
     * had ended, writing nothing more.
     */
    virtual std::unique_ptr<Handler> handleChanged(std::function<void(const opreturn::RecordChange&)> fn) = 0;
};

} // namespace interfaces

#endif // BITCOIN_INTERFACES_OPRETURN_H
