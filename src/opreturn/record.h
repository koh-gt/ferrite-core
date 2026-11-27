// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_RECORD_H
#define BITCOIN_OPRETURN_RECORD_H

#include <fs.h>
#include <interfaces/opreturn.h>
#include <merkleblock.h>
#include <opreturn/coverage.h>
#include <opreturn/format.h>
#include <opreturn/options.h>
#include <sync.h>
#include <threadsafety.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

class CBlock;
class CBlockIndex;
class CDBWrapper;
class CValidationInterface;
enum class SynchronizationState;

namespace opreturn {

//! The most bytes of blocks with rows the record queues for its worker; a block that would pass it is left out
//! and becomes a hole.
static constexpr size_t MAX_QUEUE_BYTES{64 << 20};
//! The cache of the record's database.
static constexpr size_t DB_CACHE_BYTES{8 << 20};
//! How long stop() lets the worker write queued blocks; what is left after that becomes holes.
static constexpr std::chrono::seconds STOP_DEADLINE{5};
//! How long flush() waits for the worker to finish the step it is in.
static constexpr std::chrono::seconds FLUSH_WAIT{1};
//! Listeners hear of changes at most this often; a change to FAILED or STOPPED is sent at once.
static constexpr std::chrono::milliseconds NOTIFY_INTERVAL{250};
//! A hold of cs_main by the worker longer than this is logged, and the worker then waits twice as long before
//! its next one.
static constexpr std::chrono::milliseconds CS_MAIN_SLICE{20};
//! The most queued blocks without rows the worker writes in one batch.
static constexpr size_t MAX_JOINED_ITEMS{1000};
//! At most one batch in this time is written with sync, besides the last one.
static constexpr std::chrono::seconds SYNC_INTERVAL{60};
//! The scripts of spent coins that transaction records do not hold are written in batches of about this many bytes.
static constexpr size_t SPENT_BATCH_BYTES{8 << 20};
//! How often an idle worker looks whether it alone still holds its record.
static constexpr std::chrono::seconds OWNER_CHECK_INTERVAL{1};
//! A read newest first takes rows in ranges of at most READ_RANGE_ROWS. A read finds the blocks with rows by reading
//! at most READ_BLOCK_ENTRIES block entries at a time; newest first, over spans of heights that start at
//! READ_FIRST_SPAN.
static constexpr size_t READ_RANGE_ROWS{64};
static constexpr size_t READ_BLOCK_ENTRIES{256};
static constexpr int32_t READ_FIRST_SPAN{64};

/**
 * What the record stores of one block with rows: the block's facts and proof, its rows (each with the script head,
 * and the brief in each transaction's first row) and the records of the transactions with rows.
 */
struct BlockRows {
    uint256 hash;
    uint32_t time{0};
    uint32_t ntx{0};
    CPartialMerkleTree proof;
    std::vector<Row> rows;
    //! The whole script of each row whose script is longer than its head; empty for the others.
    std::vector<std::vector<unsigned char>> long_scripts;
    //! The transactions with rows: their positions in the block and their records.
    std::vector<std::pair<uint32_t, TxRecord>> txs;
    //! Whether a coin that a transaction spends is still to be looked up.
    bool pending{false};
    //! About the memory it takes, for the queue bound.
    size_t bytes{0};
};

/**
 * The rows of a block: every output whose script is an OP_RETURN script, except the coinbase output that consensus
 * reads as the witness commitment, numbered in the order of transactions and outputs. Null when the block has none.
 * The coins that the transactions spend are not known yet, except that a coinbase spends none.
 */
std::shared_ptr<BlockRows> ReadBlockRows(const CBlock& block, const uint256& hash);

/**
 * Fills in what is known of the coins a transaction record spends, from `coins` (one per input, as the block's
 * undo data lists them) or, when it is null, as not available, and with them its brief: what paid for it, its fee
 * and the outputs to scripts it does not spend.
 */
void ResolveSpentCoins(TxRecord& tx, TxBrief& brief, const std::vector<Coin>* coins);

//! What a batch of the worker does, for tests.
struct BatchInfo {
    enum class Kind : uint8_t {
        FIRST,   //!< the first batch of a new or opened record: what the record stores about itself
        ITEMS,   //!< blocks the node connected or disconnected
        PENDING, //!< the coins of a block's transactions
        LAST,    //!< the last batch before the record stops
    };
    Kind kind{Kind::ITEMS};
    bool erases{false};   //!< it erases rows
    bool uncovers{false}; //!< it takes a height out of coverage
    uint64_t removals{0}; //!< the removal counter as it is written
    HeightSet coverage;   //!< the coverage it writes
    int32_t floor{-1};
    const CBlockIndex* tip{nullptr}; //!< the record's tip block after it
    size_t bytes{0};                 //!< about the bytes it writes
    bool sync{false};                //!< it is written with sync
};

//! Thrown by a test's hook before or after a write: the worker stops as if the process had ended there, and the
//! record writes nothing more.
struct TestCrash {
};

//! Lets a test hold the record's worker before its next step.
class WorkerHold
{
    Mutex m_mutex;
    std::condition_variable m_cv;
    bool m_closed GUARDED_BY(m_mutex){false};
    bool m_waiting GUARDED_BY(m_mutex){false};

public:
    //! The worker stops before its next step.
    void Close();
    //! The worker goes on.
    void Open();
    //! Waits until the worker waits at the hold; false after `timeout`.
    bool WaitUntilHeld(std::chrono::milliseconds timeout);
    //! Called by the worker.
    void Pass();
};

//! What a test can change in a record. Only the test constructor takes it.
struct RecordTestHooks {
    size_t queue_bound{MAX_QUEUE_BYTES};
    std::shared_ptr<WorkerHold> hold;
    //! Called before each batch is written; may throw dbwrapper_error (a failed write) or TestCrash.
    std::function<void(const BatchInfo&)> before_write;
    //! Called after each batch is written and its visibility published; may throw TestCrash.
    std::function<void(const BatchInfo&)> after_write;
    //! Called before each stored value is read, with its key's first byte and whether the worker reads it; may
    //! throw dbwrapper_error (a failed read).
    std::function<void(uint8_t prefix, bool worker)> on_read;
    //! Called in BlockConnected before it looks at the block.
    std::function<void()> in_block_connected;
    //! Called by the worker while it holds cs_main to read a block's undo data, after the read.
    std::function<void()> in_cs_main;
};

/**
 * The OP_RETURN record: every OP_RETURN output of the active chain, kept in a database of its own.
 *
 * The node's validation callbacks queue every block that is connected or disconnected. One worker thread writes
 * them in order, one batch per block with rows (blocks without rows are joined), and is the only writer; it then
 * looks up the coins that the transactions of the newest block still lacking them spend. After each batch it
 * publishes what readers may see (Visibility). The record's tip block is the block of the last item it applied, or
 * the node's tip when it started; every covered height at or below it holds exactly the rows of the tip block's
 * ancestor at that height. A failure stops the record alone, never the node.
 *
 * Made with std::make_shared. The record registers for the validation callbacks an object that holds it weakly: a
 * callback that runs keeps the record alive, and the registration never keeps alive a record that its owners have let
 * go of. The worker holds its record while it runs, so the record is never destroyed under it: when its owners let go
 * of the record without stop(), the worker ends within OWNER_CHECK_INTERVAL, writing nothing more, and the record goes
 * with it.
 */
class Record final : public interfaces::OpReturnRecord, public std::enable_shared_from_this<Record>
{
public:
    Record(fs::path path, const Options& options);
    //! For tests.
    Record(fs::path path, const Options& options, RecordTestHooks hooks);
    ~Record() override;

    Record(const Record&) = delete;
    Record& operator=(const Record&) = delete;

    /**
     * Opens the database, registers for the validation callbacks, reads the node's active chain and starts the
     * worker; with the record switched off it only listens to the node's state. Writes nothing: the worker's first
     * batch writes what a new record, or a coverage cut at start, needs. Never throws; an error turns the record to
     * FAILED.
     */
    void load();
    //! Reads the node's state once; later the node's notifications keep it.
    void start();
    //! The first step of the node's shutdown: from now on the worker reads no undo data.
    void flush();
    //! Writes what is queued, for at most STOP_DEADLINE, then a last batch, and closes the database once the reads in
    //! progress have ended. Later reads find nothing. Does nothing when a listener on the worker thread calls it.
    void stop();

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

    // The validation callbacks, called on the scheduler thread through the registered object (or by a test). They
    // copy what the record needs from the block and queue it; they never touch the disk, take no node lock and never
    // throw.
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex);
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex);

    struct Item;
    struct WorkerState;
    struct Signals;
    class Batch;
    class Reader;

    //! A read of a reader's that failed: the call and its arguments, which the worker makes again, with the snapshot
    //! that is current when it does.
    struct FailedRead {
        enum class Call : uint8_t { ROWS, ROW, SCRIPT, TRANSACTION, BLOCK, FIND, HEIGHT_FOR_TIME };
        Call call{Call::ROW};
        RowRequest rows;      //!< ROWS, without its snapshot
        RowKey key;           //!< ROW, SCRIPT
        size_t offset{0};     //!< SCRIPT
        size_t max_bytes{0};  //!< SCRIPT; TRANSACTION: the script bytes of each row
        size_t max_spent{0};  //!< TRANSACTION: the bytes of spent scripts with keys of their own
        TxKey tx;             //!< TRANSACTION
        int32_t height{-1};   //!< BLOCK
        uint256 txid;         //!< FIND
        int64_t time{0};      //!< HEIGHT_FOR_TIME
    };

private:
    enum class Job : uint8_t { OPEN, REBUILD };
    //! A switch asked while the worker ends its run.
    enum class Pending : uint8_t { NONE, ON, OFF };

    void OnBlockTip(SynchronizationState state, const CBlockIndex* index);
    void OnHeaderTip(SynchronizationState state, const CBlockIndex* index);
    void ConnectNodeState();
    void DisconnectNodeState();

    bool Open(bool wipe);
    void StartWorker(Job job) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void WorkerMain(Job job, const std::shared_ptr<Record>& self);
    void Loop(const std::shared_ptr<Record>& self);
    bool Finish(bool sync);
    void CloseDatabase();
    void Fail(FailReason reason);
    void SetState(State state) EXCLUSIVE_LOCKS_REQUIRED(m_mutex);
    void Register();
    void Unregister();
    void Push(std::unique_ptr<Item> item);
    void Publish(Batch* batch);
    void SendChanges(bool now);

    // The worker's steps.
    bool WriteFirstBatch();
    bool ApplyItems(std::deque<std::unique_ptr<Item>>& items);
    bool ResolvePending();
    bool WriteBatch(Batch& batch);
    void MoveTip(Batch& batch, const CBlockIndex* to);
    void EraseHeight(Batch& batch, int32_t height, const BlockMeta& meta);
    void Uncover(Batch& batch, HeightRange range);
    void Cover(Batch& batch, int32_t height);

    // Reads. Each read is made by one function for the readers' calls and for the worker, which makes again a read
    // that failed; the function throws on a failure.
    std::shared_ptr<CDBWrapper> BeginRead();
    void EndRead();
    void ReadFailed(const FailedRead& failed);
    void Confirm(const FailedRead& failed);
    void ReadRows(Reader& reader, const RowRequest& request, RowBatch& out);
    ReadResult ReadRow(Reader& reader, const Visibility& v, const RowKey& key, RowView& out);
    ReadResult ReadScript(Reader& reader, const Visibility& v, const RowKey& key, size_t offset, size_t max_bytes,
                          std::vector<unsigned char>& out);
    ReadResult ReadTransaction(Reader& reader, const Visibility& v, const TxKey& key, size_t max_row_script,
                               size_t max_spent_script, TxView& out);
    ReadResult ReadBlock(Reader& reader, const Visibility& v, int32_t height, BlockView& out);
    ReadResult FindTransaction(Reader& reader, const Visibility& v, const uint256& txid, TxKey& out);
    ReadResult FindHeightForTime(Reader& reader, const Visibility& v, int64_t time, int32_t& out);
    VisibilityRef UseVisibility(const VisibilityRef& given);
    bool CaughtUp(const CBlockIndex* target) const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const fs::path m_path;
    const Options m_options;
    const RecordTestHooks m_hooks;
    const bool m_pruned;

    mutable Mutex m_mutex;
    std::condition_variable m_work_cv;  //!< wakes the worker
    std::condition_variable m_tip_cv;   //!< wakes waitForTip
    std::condition_variable m_state_cv; //!< wakes flush(), stop() and whoever waits for the readers to end

    State m_state GUARDED_BY(m_mutex){State::OFF};
    FailReason m_reason GUARDED_BY(m_mutex){FailReason::NONE};
    //! Whether the callbacks queue blocks.
    bool m_accepting GUARDED_BY(m_mutex){false};
    //! What is registered for the validation callbacks; null while nothing is.
    std::shared_ptr<CValidationInterface> m_callbacks GUARDED_BY(m_mutex);
    std::deque<std::unique_ptr<Item>> m_queue GUARDED_BY(m_mutex);
    size_t m_queue_bytes GUARDED_BY(m_mutex){0};

    // What the worker is asked to do.
    bool m_draining GUARDED_BY(m_mutex){false};
    bool m_drain_seen GUARDED_BY(m_mutex){false};
    bool m_stopping GUARDED_BY(m_mutex){false};
    //! After it, a stopping worker writes no further queued block; unset (zero) while nothing stops.
    std::chrono::steady_clock::time_point m_deadline GUARDED_BY(m_mutex);
    bool m_off_requested GUARDED_BY(m_mutex){false};
    bool m_rebuild_requested GUARDED_BY(m_mutex){false};
    //! The worker has begun to end its run (a switch off, a rebuild or a stop): what is asked meanwhile waits for the
    //! end.
    bool m_ending GUARDED_BY(m_mutex){false};
    //! A switch asked while the worker ends its run, made once the run has ended. A rebuild asked meanwhile stays in
    //! m_rebuild_requested, which the end of the run acts on.
    Pending m_switch_pending GUARDED_BY(m_mutex){Pending::NONE};
    bool m_worker_running GUARDED_BY(m_mutex){false};
    //! load() has run: a switch or a rebuild asked before it does nothing.
    bool m_loaded GUARDED_BY(m_mutex){false};
    bool m_crashed GUARDED_BY(m_mutex){false};
    std::thread m_worker GUARDED_BY(m_mutex);
    //! Reads that readers saw fail, for the worker to make again.
    std::deque<FailedRead> m_confirms GUARDED_BY(m_mutex);

    // The database and its readers.
    std::shared_ptr<CDBWrapper> m_db GUARDED_BY(m_mutex);
    int m_readers GUARDED_BY(m_mutex){0};

    // What readers see.
    VisibilityRef m_visibility GUARDED_BY(m_mutex);
    const CBlockIndex* m_tip_index GUARDED_BY(m_mutex){nullptr};
    RecordStatus m_status GUARDED_BY(m_mutex);
    std::atomic<uint64_t> m_removals{0};
    std::atomic<uint64_t> m_read_errors{0};
    uint64_t m_generation GUARDED_BY(m_mutex){0};

    // The node's state, from its notifications.
    bool m_node_known GUARDED_BY(m_mutex){false};
    //! The node's best block. stop() clears it, and nothing reads it once the record has stopped.
    const CBlockIndex* m_node_tip GUARDED_BY(m_mutex){nullptr};
    //! What getStatus() reports of it, copied when the node tells it, so that no entry is read after stop().
    int32_t m_node_tip_height GUARDED_BY(m_mutex){-1};
    int64_t m_node_tip_time GUARDED_BY(m_mutex){0};
    uint256 m_node_tip_hash GUARDED_BY(m_mutex);
    bool m_initial_download GUARDED_BY(m_mutex){true};
    int32_t m_header_tip GUARDED_BY(m_mutex){-1};
    int64_t m_header_tip_time GUARDED_BY(m_mutex){0};
    //! A wait for the tip ran out: later waits return at once until a call finds the record caught up.
    bool m_stalled GUARDED_BY(m_mutex){false};

    // Changes not yet sent to the listeners.
    RecordChange m_change GUARDED_BY(m_mutex);
    bool m_change_now GUARDED_BY(m_mutex){false};
    std::chrono::steady_clock::time_point m_last_notify GUARDED_BY(m_mutex);

    //! Only the worker (and load() before it starts the worker) uses it.
    std::unique_ptr<WorkerState> m_w;
    std::unique_ptr<Signals> m_signals;
};

} // namespace opreturn

#endif // BITCOIN_OPRETURN_RECORD_H
