// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/record.h>

#include <chain.h>
#include <chainparams.h>
#include <clientversion.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <interfaces/handler.h>
#include <memusage.h>
#include <node/ui_interface.h>
#include <opreturn/log.h>
#include <opreturn/names.h>
#include <opreturn/script.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <streams.h>
#include <undo.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/signals2/signal.hpp>

#include <algorithm>
#include <ios>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace opreturn {
namespace {

using Clock = std::chrono::steady_clock;

//! The bytes of a key or a value as the database holds them (a value without its obfuscation).
struct RawBytes {
    std::vector<unsigned char> bytes;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (!bytes.empty()) s.write((const char*)bytes.data(), bytes.size());
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        if (!bytes.empty()) s.read((char*)bytes.data(), bytes.size());
    }
};

template <typename K>
std::vector<unsigned char> KeyBytes(const K& key)
{
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << key;
    return {stream.begin(), stream.end()};
}

//! Decodes stored bytes as the whole of a value of type T; throws std::ios_base::failure.
template <typename T>
void Decode(const std::vector<unsigned char>& bytes, T& value)
{
    CDataStream stream(bytes, SER_DISK, CLIENT_VERSION);
    stream >> Whole<T>(value);
}

//! Thrown when a read of the record's database fails, or finds the record's keys disagreeing with each other.
struct ReadError {
};

//! The most failed reads the worker is asked to confirm at a time.
constexpr size_t MAX_CONFIRMS{64};

const CBlockIndex* CommonAncestor(const CBlockIndex* a, const CBlockIndex* b)
{
    if (!a || !b) return nullptr;
    if (a->nHeight > b->nHeight) {
        a = a->GetAncestor(b->nHeight);
    } else if (b->nHeight > a->nHeight) {
        b = b->GetAncestor(a->nHeight);
    }
    while (a != b && a && b) {
        a = a->pprev;
        b = b->pprev;
    }
    return a == b ? a : nullptr;
}

//! The locator of a block: its hash and those of ancestors at growing distances, down to the genesis block.
CBlockLocator LocatorOf(const CBlockIndex* index)
{
    std::vector<uint256> have;
    int step = 1;
    while (index) {
        have.push_back(index->GetBlockHash());
        if (index->nHeight == 0) break;
        index = index->GetAncestor(std::max(index->nHeight - step, 0));
        if (have.size() > 10) step *= 2;
    }
    return CBlockLocator(have);
}

void Extend(HeightRange& range, int32_t lo, int32_t hi)
{
    if (lo > hi) return;
    if (range.lo > range.hi) {
        range = {lo, hi};
    } else {
        range.lo = std::min(range.lo, lo);
        range.hi = std::max(range.hi, hi);
    }
}

std::shared_ptr<const Visibility> EmptyVisibility(uint64_t generation, uint64_t removals)
{
    auto visibility = std::make_shared<Visibility>();
    visibility->coverage = std::make_shared<const std::vector<HeightRange>>();
    visibility->gaps = std::make_shared<const std::vector<GapRange>>();
    visibility->generation = generation;
    visibility->removals = removals;
    return visibility;
}

RowView ToView(const RowKey& key, Row& row, size_t max_script, const TxBrief& brief)
{
    RowView view;
    view.key = key;
    view.txpos = row.txpos;
    view.vout = row.vout;
    view.flags = row.flags;
    view.value = row.value;
    view.script_size = row.script_size;
    view.data_size = row.data_size;
    if (row.head.size() > max_script) row.head.resize(max_script);
    view.script_prefix = std::move(row.head);
    view.tx = brief;
    return view;
}

} // namespace

std::shared_ptr<BlockRows> ReadBlockRows(const CBlock& block, const uint256& hash)
{
    std::shared_ptr<BlockRows> out;
    std::vector<bool> matches(block.vtx.size(), false);
    uint32_t n = 0;
    for (size_t pos = 0; pos < block.vtx.size(); ++pos) {
        const CTransaction& tx = *block.vtx[pos];
        const int commitment = pos == 0 ? WitnessCommitmentIndex(tx) : -1;
        std::vector<uint32_t> outputs;
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            if (int(i) != commitment && IsOpReturn(tx.vout[i].scriptPubKey)) outputs.push_back(uint32_t(i));
        }
        if (outputs.empty()) continue;
        if (!out) out = std::make_shared<BlockRows>();
        matches[pos] = true;

        TxRecord record;
        record.txid = tx.GetHash();
        record.version = tx.nVersion;
        record.locktime = tx.nLockTime;
        record.size = uint32_t(tx.GetTotalSize());
        record.weight = uint32_t(GetTransactionWeight(tx));
        record.vsize = uint32_t(GetVirtualTransactionSize(tx));
        const bool coinbase = tx.IsCoinBase();
        if (coinbase) record.tx_flags |= TX_COINBASE;
        if (tx.IsHogEx()) record.tx_flags |= TX_HOGEX;
        if (tx.HasMWEBTx()) record.tx_flags |= TX_HAS_MWEB;
        if (tx.HasWitness()) record.tx_flags |= TX_HAS_WITNESS;
        if (outputs.size() > 1) record.tx_flags |= TX_MULTI_OPRETURN;
        for (const CTxOut& txout : tx.vout) {
            if (txout.scriptPubKey.IsMWEBPegin()) record.tx_flags |= TX_HAS_PEGIN;
        }
        for (const CTxIn& txin : tx.vin) {
            TxInRecord in;
            in.prevout = txin.prevout;
            in.script_sig = txin.scriptSig;
            in.sequence = txin.nSequence;
            in.spent_state = coinbase ? SpentState::NONE : SpentState::PENDING;
            if (!coinbase) record.tx_flags |= TX_SPENT_PENDING;
            record.vin.push_back(std::move(in));
        }
        size_t next_row = 0;
        for (size_t i = 0; i < tx.vout.size(); ++i) {
            const bool is_row = next_row < outputs.size() && outputs[next_row] == i;
            record.vout.push_back(TxOutRecord::FromOutput(tx.vout[i], is_row, is_row ? n + uint32_t(next_row) : 0));
            if (is_row) ++next_row;
        }
        if (tx.HasMWEBTx()) {
            record.n_pegin = uint32_t(tx.mweb_tx.GetPegIns().size());
            record.n_pegout = uint32_t(tx.mweb_tx.GetPegOuts().size());
            record.n_kernel = uint32_t(tx.mweb_tx.m_transaction->GetKernels().size());
        }

        TxBrief brief;
        brief.txid = record.txid;
        brief.block_time = block.nTime;
        brief.tx_flags = record.tx_flags;
        brief.paid_by_kind = coinbase ? PaidByKind::COINBASE : tx.IsHogEx() ? PaidByKind::HOGEX : PaidByKind::PENDING;
        brief.vsize = record.vsize;
        brief.n_in = uint32_t(tx.vin.size());
        brief.n_out = uint32_t(tx.vout.size());
        brief.n_rows = uint32_t(outputs.size());
        if (!coinbase && !tx.vin.empty()) out->pending = true;

        for (size_t j = 0; j < outputs.size(); ++j) {
            const CScript& script = tx.vout[outputs[j]].scriptPubKey;
            const ParsedScript parsed = ParseOpReturn(script, PARSE_FACTS);
            Row row;
            row.txpos = uint32_t(pos);
            row.vout = outputs[j];
            row.first_delta = uint32_t(j);
            row.flags = parsed.flags;
            row.value = tx.vout[outputs[j]].nValue;
            row.script_size = uint32_t(script.size());
            row.data_size = uint32_t(parsed.data_size);
            const Span<const unsigned char> head = ScriptHead(script);
            row.head.assign(head.begin(), head.end());
            if (j == 0) row.brief = brief;
            out->rows.push_back(std::move(row));
            if (script.size() > SCRIPT_HEAD_SIZE) {
                out->long_scripts.emplace_back(script.begin(), script.end());
            } else {
                out->long_scripts.emplace_back();
            }
        }
        n += uint32_t(outputs.size());
        out->txs.emplace_back(uint32_t(pos), std::move(record));
    }
    if (!out) return nullptr;
    out->hash = hash;
    out->time = block.nTime;
    out->ntx = uint32_t(block.vtx.size());
    std::vector<uint256> txids;
    txids.reserve(block.vtx.size());
    for (const CTransactionRef& tx : block.vtx) txids.push_back(tx->GetHash());
    out->proof = CPartialMerkleTree(txids, matches);
    // The memory the copy takes, which the queue's bound counts: its structures, the room its vectors hold, and what
    // its scripts take besides.
    size_t bytes = sizeof(BlockRows) + memusage::DynamicUsage(out->rows) + memusage::DynamicUsage(out->long_scripts) +
                   memusage::DynamicUsage(out->txs) + ::GetSerializeSize(out->proof, PROTOCOL_VERSION);
    for (const Row& row : out->rows) bytes += memusage::DynamicUsage(row.head) + memusage::DynamicUsage(row.brief.paid_by);
    for (const std::vector<unsigned char>& script : out->long_scripts) bytes += memusage::DynamicUsage(script);
    for (const auto& tx : out->txs) {
        bytes += memusage::DynamicUsage(tx.second.vin) + memusage::DynamicUsage(tx.second.vout);
        for (const TxInRecord& in : tx.second.vin) bytes += memusage::DynamicUsage(in.script_sig);
        for (const TxOutRecord& o : tx.second.vout) bytes += memusage::DynamicUsage(o.out.scriptPubKey);
    }
    out->bytes = bytes;
    return out;
}

void ResolveSpentCoins(TxRecord& tx, TxBrief& brief, const std::vector<Coin>* coins)
{
    if (coins && coins->size() != tx.vin.size()) coins = nullptr;
    bool all_known = true;
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        TxInRecord& in = tx.vin[i];
        if (in.spent_state == SpentState::PENDING) {
            // A coin whose script the record cannot store was never spent: such an input cannot be in a block.
            if (coins && (*coins)[i].out.scriptPubKey.size() <= MAX_SCRIPT_SIZE && MoneyRange((*coins)[i].out.nValue)) {
                const Coin& coin = (*coins)[i];
                in.spent_state = SpentState::KNOWN;
                if (coin.out.scriptPubKey.size() > SPENT_SCRIPT_INLINE_SIZE) {
                    // Kept under a key of its own: the record holds the coin without its script.
                    in.coin = Coin(CTxOut(coin.out.nValue, CScript()), int(coin.nHeight), coin.fCoinBase, coin.fPegout);
                    in.script_apart = uint32_t(coin.out.scriptPubKey.size());
                } else {
                    in.coin = coin;
                    in.script_apart = 0;
                }
            } else {
                in.spent_state = SpentState::UNAVAILABLE;
                in.coin.Clear();
                in.script_apart = 0;
            }
        }
        if (in.spent_state != SpentState::KNOWN) all_known = false;
    }
    // The script of the coin input i spends: from the coins given, which hold the scripts the record does not.
    const auto spent_script = [&](size_t i) -> const CScript& {
        return coins ? (*coins)[i].out.scriptPubKey : tx.vin[i].coin.out.scriptPubKey;
    };
    tx.tx_flags &= ~TX_SPENT_PENDING;
    brief.tx_flags = tx.tx_flags;
    const bool coinbase = tx.tx_flags & TX_COINBASE;
    const bool hogex = tx.tx_flags & TX_HOGEX;
    // A script kept apart is in `coins` only. Without them (for coins known before this call), the paying script the
    // brief has stays, and the other payees are not counted.
    const bool scripts_missing = !coins && std::any_of(tx.vin.begin(), tx.vin.end(), [](const TxInRecord& in) {
        return in.spent_state == SpentState::KNOWN && in.script_apart > 0;
    });
    const CScript kept = !coins && !tx.vin.empty() && tx.vin[0].script_apart > 0 ? brief.paid_by : CScript();
    brief.paid_by.clear();
    if (coinbase) {
        brief.paid_by_kind = PaidByKind::COINBASE;
    } else if (hogex) {
        brief.paid_by_kind = PaidByKind::HOGEX;
    } else if (!tx.vin.empty() && tx.vin[0].spent_state == SpentState::KNOWN) {
        brief.paid_by_kind = PaidByKind::SCRIPT;
        brief.paid_by = tx.vin[0].script_apart > 0 && !coins ? kept : spent_script(0);
    } else {
        brief.paid_by_kind = PaidByKind::UNAVAILABLE;
    }
    brief.fee = -1;
    brief.n_other = N_OTHER_UNKNOWN;
    brief.other_value = 0;
    if (!all_known || coinbase || hogex) return;

    CAmount in_value = 0;
    // The spent scripts, without copies: a transaction can spend coins whose scripts are many times its size.
    const auto less = [](const CScript* a, const CScript* b) { return *a < *b; };
    std::set<const CScript*, decltype(less)> spent_scripts(less);
    for (size_t i = 0; i < tx.vin.size(); ++i) {
        in_value += tx.vin[i].coin.out.nValue;
        if (!MoneyRange(in_value)) return;
        spent_scripts.insert(&spent_script(i));
    }
    CAmount out_value = 0;
    uint32_t n_other = 0;
    CAmount other_value = 0;
    for (const TxOutRecord& o : tx.vout) {
        out_value += o.out.nValue;
        if (!MoneyRange(out_value)) return;
        if (o.kind == TxOutRecord::Kind::ROW || IsOpReturn(o.out.scriptPubKey)) continue;
        if (spent_scripts.count(&o.out.scriptPubKey)) continue;
        ++n_other;
        other_value += o.out.nValue;
    }
    // The fee of a transaction that moves coins into MWEB is not computed here, so that it agrees with the fee of
    // the same transaction while it is unconfirmed.
    if (!(tx.tx_flags & (TX_HAS_MWEB | TX_HAS_PEGIN)) && in_value >= out_value) brief.fee = in_value - out_value;
    if (scripts_missing) return;
    brief.n_other = uint8_t(std::min<uint32_t>(n_other, N_OTHER_UNKNOWN - 1));
    brief.other_value = MoneyRange(other_value) ? other_value : 0;
}

void WorkerHold::Close()
{
    LOCK(m_mutex);
    m_closed = true;
}

void WorkerHold::Open()
{
    {
        LOCK(m_mutex);
        m_closed = false;
    }
    m_cv.notify_all();
}

bool WorkerHold::WaitUntilHeld(std::chrono::milliseconds timeout)
{
    WAIT_LOCK(m_mutex, lock);
    return m_cv.wait_for(lock, timeout, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_waiting; });
}

void WorkerHold::Pass()
{
    WAIT_LOCK(m_mutex, lock);
    if (!m_closed) return;
    m_waiting = true;
    m_cv.notify_all();
    m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return !m_closed; });
    m_waiting = false;
}

//! One block the node connected or disconnected, queued for the worker.
struct Record::Item {
    enum class Kind : uint8_t { CONNECT, DISCONNECT, DROPPED };
    Kind kind{Kind::CONNECT};
    const CBlockIndex* index{nullptr};
    std::shared_ptr<const BlockRows> rows; //!< a connected block with rows
    size_t bytes{0};
};

//! What only the worker uses.
struct Record::WorkerState {
    std::shared_ptr<CDBWrapper> db;
    HeightSet coverage;
    int32_t floor{-1};
    HeightSet unreadable;
    Stats stats;
    const CBlockIndex* tip{nullptr};
    //! Heights whose transaction records still lack coins they spend (the 'P' keys).
    std::set<int32_t> pending;
    bool is_new{false};
    bool cut_at_start{false};
    Clock::time_point last_sync{Clock::now()};
    Clock::time_point next_cs_main{};

    int32_t TipHeight() const { return tip ? tip->nHeight : -1; }
    bool IsVisible(int32_t height) const { return height >= 0 && height >= floor && height <= TipHeight() && coverage.Contains(height); }
};

//! The node's state notifications reach the record through this, which outlives the record: the destructor cuts the
//! link before the record goes.
struct NodeLink {
    Mutex mutex;
    Record* record GUARDED_BY(mutex){nullptr};
};

namespace {
//! What the record registers for the validation callbacks. It holds the record weakly: while a callback runs it holds
//! the record, and a record that its owners have let go of is gone, so the callback does nothing.
class Callbacks final : public CValidationInterface
{
    const std::weak_ptr<Record> m_record;

public:
    explicit Callbacks(std::weak_ptr<Record> record) : m_record(std::move(record)) {}

    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        if (const std::shared_ptr<Record> record = m_record.lock()) record->BlockConnected(block, index);
    }
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        if (const std::shared_ptr<Record> record = m_record.lock()) record->BlockDisconnected(block, index);
    }
};
} // namespace

struct Record::Signals {
    boost::signals2::signal<void(const RecordChange&)> changed;
    std::shared_ptr<NodeLink> link{std::make_shared<NodeLink>()};
    std::vector<boost::signals2::connection> node;
};

//! One batch of the worker, and what it changes.
class Record::Batch
{
public:
    Batch(CDBWrapper& db, BatchInfo::Kind kind) : batch(db) { info.kind = kind; }

    CDBBatch batch;
    BatchInfo info;
    Clock::time_point begun{Clock::now()};
    //! Heights whose keys it writes or erases: a later item of the same height goes into the next batch.
    std::set<int32_t> touched;
    size_t items{0};
    bool rows{false};
    bool live{false}; //!< it moves the record's tip, so it writes the tip locator
    bool coverage_changed{false};
    bool stats_changed{false};
    HeightRange added{0, -1};
    HeightRange removed{0, -1};
    HeightRange updated{0, -1};
    uint32_t new_rows{0};
    uint64_t uncovered_for_limit{0};
    //! The connected blocks it writes, with their numbers of rows, for the log.
    std::vector<std::pair<int32_t, uint32_t>> connected;
};

/**
 * Reads of the database.
 *
 * A single key is read with the database's own lookup, which reports a damaged file. Scans go through one iterator,
 * so that every key a reader scans belongs to one state of the database. The tree's iterator reports no error: a
 * table block it cannot read is passed over, and at the end it becomes invalid. So the key 'z', which sorts after
 * every other key and is never erased, ends every scan; and a scan of rows checks what it finds against the block
 * entries: a block's rows are numbered 0 to nrows - 1, and in a covered stretch of heights the heights with rows
 * are exactly those with a block entry. A failure throws ReadError.
 */
class Record::Reader
{
public:
    //! A reader of a reader thread, which holds the database open until it ends.
    explicit Reader(Record& record) : m_record(record), m_worker(false)
    {
        m_db = record.BeginRead();
    }
    //! The worker's reader.
    Reader(Record& record, std::shared_ptr<CDBWrapper> db) : m_record(record), m_worker(true), m_db(std::move(db)) {}
    ~Reader()
    {
        m_it.reset();
        if (!m_worker && m_db) {
            m_db.reset();
            m_record.EndRead();
        }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool Open() const { return m_db != nullptr; }

    //! The value of a single key; false when the key is not there.
    template <typename K, typename V>
    bool Point(const K& key, V& value)
    {
        const std::vector<unsigned char> bytes = KeyBytes(key);
        RawBytes raw;
        try {
            if (!m_db->Read(RawBytes{bytes}, raw)) return false;
        } catch (const std::exception&) {
            throw ReadError();
        }
        Hook(bytes);
        try {
            Decode(raw.bytes, value);
        } catch (const std::exception&) {
            throw ReadError();
        }
        return true;
    }

    void Seek(std::vector<unsigned char> key)
    {
        It().Seek(RawBytes{key});
        Land();
    }

    void Next()
    {
        It().Next();
        Land();
    }

    //! The key the reader's scan stands on.
    const std::vector<unsigned char>& Key() const { return m_key; }
    uint8_t Prefix() const { return m_key[0]; }

    //! That key, as a key of type K.
    template <typename K>
    void KeyAs(K& key)
    {
        try {
            Decode(m_key, key);
        } catch (const std::exception&) {
            throw ReadError();
        }
    }

    //! The value at that key.
    template <typename V>
    void Value(V& value)
    {
        Hook(m_key);
        RawBytes raw;
        if (!It().GetValue(raw)) throw ReadError();
        try {
            Decode(raw.bytes, value);
        } catch (const std::exception&) {
            throw ReadError();
        }
    }

    //! The scan's value of `key`; false when the scan finds another key there.
    template <typename K, typename V>
    bool Scanned(const K& key, V& value)
    {
        const std::vector<unsigned char> bytes = KeyBytes(key);
        Seek(bytes);
        if (m_key != bytes) return false;
        Value(value);
        return true;
    }

    //! The first row key at or after `key`; none when no row key follows.
    Optional<RowKey> FirstRowFrom(const RowKey& key)
    {
        Seek(KeyBytes(RowDbKey(key)));
        if (Prefix() != prefix::ROW) return nullopt;
        RowDbKey found;
        KeyAs(found);
        return found.key;
    }

    /**
     * The block entries at the heights lo to hi, lowest first, at most READ_BLOCK_ENTRIES of them; `cut` when there
     * are more. Only the entries are read.
     */
    void BlockEntries(int32_t lo, int32_t hi, std::vector<std::pair<int32_t, BlockMeta>>& out, bool& cut)
    {
        out.clear();
        cut = false;
        if (lo > hi) return;
        for (Seek(KeyBytes(HeightKey(prefix::BLOCK, lo))); Prefix() == prefix::BLOCK; Next()) {
            HeightKey key{prefix::BLOCK, 0};
            KeyAs(key);
            if (key.height > hi) break;
            if (out.size() == READ_BLOCK_ENTRIES) {
                cut = true;
                break;
            }
            out.emplace_back(key.height, BlockMeta());
            Value(out.back().second);
        }
    }

    //! Whether the database holds any key of the record (a new database holds only its obfuscation key).
    bool AnyRecordKey()
    {
        It().Seek(RawBytes{{prefix::COVERAGE}});
        return It().Valid();
    }

    //! Bytes of a row's script, from its head or its pieces.
    bool ScriptBytes(const RowKey& key, const Row& row, uint64_t offset, uint64_t max_bytes, std::vector<unsigned char>& out)
    {
        return ReadScriptBytes(row, offset, max_bytes, [&](uint32_t k, std::vector<unsigned char>& piece) {
            ScriptPiece value;
            if (!Point(PieceDbKey(key, k), value)) return false;
            piece = std::move(value.bytes);
            return true;
        }, out);
    }

private:
    CDBIterator& It()
    {
        if (!m_it) m_it.reset(m_db->NewIterator());
        return *m_it;
    }

    //! Every scan ends on the key 'z': a scan that runs out of keys has failed.
    void Land()
    {
        if (!It().Valid()) throw ReadError();
        RawBytes key;
        if (!It().GetKey(key) || key.bytes.empty()) throw ReadError();
        m_key = std::move(key.bytes);
    }

    void Hook(const std::vector<unsigned char>& key)
    {
        if (m_record.m_hooks.on_read) {
            try {
                m_record.m_hooks.on_read(key[0], m_worker);
            } catch (const std::exception&) {
                throw ReadError();
            }
        }
    }

    Record& m_record;
    const bool m_worker;
    std::shared_ptr<CDBWrapper> m_db;
    std::unique_ptr<CDBIterator> m_it;
    std::vector<unsigned char> m_key;
};

namespace {

//! Runs f under cs_main for the worker, keeping its holds short: after a hold longer than CS_MAIN_SLICE, the next one
//! may begin only twice that long later (next_hold), and the worker does its other work meanwhile. The wait for the
//! lock is not part of the hold.
template <typename F>
void WithCsMain(Clock::time_point& next_hold, F f)
{
    Clock::duration held;
    {
        LOCK(cs_main);
        const Clock::time_point begun = Clock::now();
        f();
        held = Clock::now() - begun;
    }
    if (held > CS_MAIN_SLICE) {
        Log(LogEvent::CS_MAIN_HELD, std::chrono::duration_cast<std::chrono::milliseconds>(held).count());
        next_hold = Clock::now() + 2 * held;
    }
}

} // namespace

Record::Record(fs::path path, const Options& options) : Record(std::move(path), options, RecordTestHooks{}) {}

Record::Record(fs::path path, const Options& options, RecordTestHooks hooks)
    : m_path(std::move(path)), m_options(options), m_hooks(std::move(hooks)), m_pruned(fPruneMode),
      m_signals(std::make_unique<Signals>())
{
    m_visibility = EmptyVisibility(0, 0);
    m_status.coverage = m_visibility->coverage;
    m_status.pruned = m_pruned;
    m_status.explicit_options = m_options.explicit_options;
    m_last_notify = Clock::now() - NOTIFY_INTERVAL;
}

Record::~Record()
{
    // No node lock and no node global here. When the record was not stopped, the node's notifications are cut off
    // through the link, and the registration for the validation callbacks that is left reaches nothing. No worker
    // runs: a running worker holds the record. A worker that has ended is joined; and when the worker held the last
    // reference, this runs as the last thing on its thread, which is then detached.
    {
        LOCK(m_signals->link->mutex);
        m_signals->link->record = nullptr;
    }
    std::thread worker;
    {
        LOCK(m_mutex);
        m_crashed = true;
        worker = std::move(m_worker);
    }
    if (m_hooks.hold) m_hooks.hold->Open();
    m_work_cv.notify_all();
    if (worker.joinable()) {
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }
}

void Record::ConnectNodeState()
{
    {
        LOCK(m_signals->link->mutex);
        if (m_signals->link->record) return;
        m_signals->link->record = this;
    }
    std::shared_ptr<NodeLink> link = m_signals->link;
    m_signals->node.push_back(uiInterface.NotifyBlockTip_connect([link](SynchronizationState state, const CBlockIndex* index) {
        LOCK(link->mutex);
        if (link->record) link->record->OnBlockTip(state, index);
    }));
    m_signals->node.push_back(uiInterface.NotifyHeaderTip_connect([link](SynchronizationState state, const CBlockIndex* index) {
        LOCK(link->mutex);
        if (link->record) link->record->OnHeaderTip(state, index);
    }));
}

void Record::DisconnectNodeState()
{
    for (boost::signals2::connection& connection : m_signals->node) connection.disconnect();
    m_signals->node.clear();
}

void Record::OnBlockTip(SynchronizationState state, const CBlockIndex* index)
{
    if (!index) return;
    {
        LOCK(m_mutex);
        m_node_known = true;
        m_node_tip = index;
        m_node_tip_height = index->nHeight;
        m_node_tip_time = index->GetBlockTime();
        m_node_tip_hash = index->GetBlockHash();
        m_initial_download = state != SynchronizationState::POST_INIT;
        m_change.kinds |= RecordChange::STATUS;
    }
    m_tip_cv.notify_all();
}

void Record::OnHeaderTip(SynchronizationState state, const CBlockIndex* index)
{
    if (!index) return;
    LOCK(m_mutex);
    m_header_tip = index->nHeight;
    m_header_tip_time = index->GetBlockTime();
    m_change.kinds |= RecordChange::STATUS;
}

void Record::SetState(State state)
{
    if (m_state == state) return;
    m_state = state;
    if (state == State::FAILED) {
        Log(LogEvent::FAILED, int64_t(m_reason));
    } else {
        Log(LogEvent::STATE, int64_t(state));
    }
    m_change.kinds |= RecordChange::STATUS;
    if (state == State::FAILED || state == State::STOPPED) m_change_now = true;
    m_state_cv.notify_all();
    // A wait for the tip ends when the record leaves RUNNING and BACKFILLING.
    m_tip_cv.notify_all();
}

void Record::Fail(FailReason reason)
{
    {
        LOCK(m_mutex);
        if (m_state == State::FAILED || m_state == State::STOPPED || m_crashed) return;
        m_reason = reason;
        SetState(State::FAILED);
        m_accepting = false;
        m_queue.clear();
        m_queue_bytes = 0;
    }
    Unregister();
    m_work_cv.notify_all();
    m_tip_cv.notify_all();
    SendChanges(true);
}

void Record::Register()
{
    std::shared_ptr<CValidationInterface> callbacks;
    {
        LOCK(m_mutex);
        if (m_callbacks || m_stopping) return;
        callbacks = std::make_shared<Callbacks>(shared_from_this());
        m_callbacks = callbacks;
        m_accepting = true;
    }
    RegisterSharedValidationInterface(callbacks);
}

void Record::Unregister()
{
    std::shared_ptr<CValidationInterface> callbacks;
    {
        LOCK(m_mutex);
        m_accepting = false;
        callbacks = std::move(m_callbacks);
        m_callbacks.reset();
    }
    if (callbacks) UnregisterSharedValidationInterface(callbacks);
}

void Record::load()
{
    ConnectNodeState();
    if (!m_options.record) {
        LOCK(m_mutex);
        SetState(State::OFF);
        return;
    }
    try {
        if (!Open(m_options.reindex)) return;
        LOCK(m_mutex);
        if (m_state == State::FAILED || m_stopping) return;
        StartWorker(Job::OPEN);
    } catch (...) {
        Fail(FailReason::INTERNAL);
    }
}

bool Record::Open(bool wipe)
{
    {
        LOCK(m_mutex);
        if (m_stopping) return false;
        m_reason = FailReason::NONE;
        SetState(State::OPENING);
    }
    SendChanges(false);
    bool open;
    {
        LOCK(m_mutex);
        open = m_db != nullptr;
    }
    // A database left open by a failure is closed before it is opened again.
    if (open) CloseDatabase();
    std::shared_ptr<CDBWrapper> db;
    try {
        db = std::make_shared<CDBWrapper>(m_path, DB_CACHE_BYTES, /*fMemory=*/false, wipe, /*obfuscate=*/true);
    } catch (const std::exception&) {
        Fail(FailReason::OPEN_FAILED);
        return false;
    }
    auto w = std::make_unique<WorkerState>();
    w->db = db;
    StoredCoverage stored;
    CBlockLocator locator;
    FailReason reason = FailReason::NONE;
    try {
        Reader reader(*this, db);
        RecordMeta meta;
        if (!reader.Point(uint8_t{prefix::META}, meta)) {
            if (reader.AnyRecordKey()) {
                reason = FailReason::OPEN_FAILED;
            } else {
                w->is_new = true;
            }
        } else if (meta.format > FORMAT_VERSION) {
            reason = FailReason::NEWER_FORMAT;
        } else if (meta.format != FORMAT_VERSION) {
            reason = FailReason::OPEN_FAILED;
        } else if (meta.genesis_hash != Params().GenesisBlock().GetHash()) {
            reason = FailReason::OTHER_CHAIN;
        } else {
            NoValue end;
            if (!reader.Point(uint8_t{prefix::END}, end) || !reader.Point(uint8_t{prefix::COVERAGE}, stored) ||
                !reader.Point(uint8_t{prefix::STATS}, w->stats)) {
                reason = FailReason::OPEN_FAILED;
            } else {
                reader.Point(uint8_t{prefix::TIP}, locator);
                // The heights whose coins are still to be looked up.
                for (reader.Seek(KeyBytes(HeightKey(prefix::PENDING, 0))); reader.Prefix() == prefix::PENDING; reader.Next()) {
                    HeightKey key{prefix::PENDING, 0};
                    reader.KeyAs(key);
                    w->pending.insert(key.height);
                }
            }
        }
    } catch (const ReadError&) {
        reason = FailReason::OPEN_FAILED;
    }
    if (!w->is_new) {
        // Readers find a new database only once its first batch has written the keys every scan needs.
        LOCK(m_mutex);
        m_db = db;
    }
    if (reason != FailReason::NONE) {
        Fail(reason);
        return false;
    }
    if (!HeightSet::FromRanges(stored.ranges, w->coverage)) {
        Fail(FailReason::OPEN_FAILED);
        return false;
    }
    w->floor = stored.floor;
    if (!HeightSet::FromRanges(stored.unreadable, w->unreadable)) {
        Fail(FailReason::OPEN_FAILED);
        return false;
    }

    // Register before the tip is read, so that no block is missed; callbacks queue until the worker runs.
    Register();
    const HeightSet loaded = w->coverage;
    {
        LOCK(cs_main);
        const CChain& active = ::ChainActive();
        const CBlockIndex* tip = active.Tip();
        const int32_t height = active.Height();
        if (locator.vHave.empty()) {
            // No locator: a new record, or one that never wrote a block. Nothing is covered.
            w->coverage = HeightSet();
        } else {
            const CBlockIndex* t = LookupBlockIndex(locator.vHave[0]);
            bool known = false;
            for (const uint256& hash : locator.vHave) {
                if (LookupBlockIndex(hash)) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                // The block index was built again (-reindex): every row is invisible until its block connects.
                w->coverage = HeightSet();
            } else {
                // The record wrote no height above its tip; a range above it is damage.
                if (t) w->coverage.RemoveAbove(t->nHeight);
                if (height < 0 && t) {
                    // The active chain is empty and the record's tip is known (-reindex-chainstate): nothing is cut,
                    // nothing is visible. When the tip is not known, the case below cuts everything.
                } else if (t && active.Contains(t)) {
                    // The record's tip is on the active chain: nothing is cut.
                } else if (t && t->nHeight > height && t->GetAncestor(height) == tip) {
                    // The active chain is behind the record's tip: nothing is cut; rows above it stay invisible.
                } else {
                    // The chain reorganised while the record was not running: heights above the fork leave coverage.
                    const CBlockIndex* fork = FindForkInGlobalIndex(active, locator);
                    w->coverage.RemoveAbove(fork ? fork->nHeight : -1);
                }
            }
        }
        w->tip = tip;
    }
    w->cut_at_start = w->coverage != loaded;
    m_w = std::move(w);
    // What a reader saw of the database before it was opened again (a switch, a rebuild) no longer holds.
    ++m_removals;
    Publish(nullptr);
    return true;
}

void Record::StartWorker(Job job)
{
    std::thread previous = std::move(m_worker);
    m_worker_running = true;
    m_ending = false;
    // The worker holds the record for as long as it runs, so that the record is never destroyed under it. The thread
    // that starts it holds the record too (an owner, a running callback, or the worker that ends), so this reference
    // is never the last when it is taken.
    std::shared_ptr<Record> self = shared_from_this();
    m_worker = std::thread([this, job, self = std::move(self), previous = std::move(previous)]() mutable {
        if (previous.joinable()) previous.join();
        WorkerMain(job, self);
        // Last: when the record's owners have let go of it, it goes here, and nothing of it is used after this.
        self.reset();
    });
}

void Record::WorkerMain(Job job, const std::shared_ptr<Record>& self)
{
    util::ThreadRename(std::string{WORKER_THREAD_NAME});
    try {
        bool opened = true;
        if (job == Job::REBUILD) {
            CloseDatabase();
            opened = Open(/*wipe=*/true);
        } else if (!m_w) {
            opened = Open(/*wipe=*/false);
        }
        if (opened) Loop(self);
    } catch (...) {
        Fail(FailReason::INTERNAL);
    }
    LOCK(m_mutex);
    m_worker_running = false;
    // What was asked while the worker ended: build again, or switch on again (a failed record is only built again).
    const bool may_start = !m_stopping && !m_crashed;
    const bool rebuild = may_start && m_rebuild_requested;
    const bool reopen = may_start && !rebuild && m_switch_pending == Pending::ON && m_state == State::OFF;
    m_rebuild_requested = false;
    m_off_requested = false;
    m_switch_pending = Pending::NONE;
    m_ending = false;
    m_state_cv.notify_all();
    if (rebuild) {
        StartWorker(Job::REBUILD);
    } else if (reopen) {
        StartWorker(Job::OPEN);
    }
}

void Record::Loop(const std::shared_ptr<Record>& self)
{
    // Whether the run ends with a rebuild.
    bool rebuild = false;
    // The step that ran: what an exception in it means.
    auto run = [this, &rebuild](const std::function<bool()>& step) -> bool {
        try {
            return step();
        } catch (const TestCrash&) {
            LOCK(m_mutex);
            m_crashed = true;
        } catch (const ReadError&) {
            Fail(FailReason::READ_FAILED);
        } catch (const dbwrapper_error&) {
            Fail(FailReason::WRITE_FAILED);
        } catch (const std::ios_base::failure&) {
            // A value the format refuses to store.
            Fail(FailReason::WRITE_FAILED);
        } catch (...) {
            Fail(FailReason::INTERNAL);
        }
        // A rebuild asked for survives a failure of the database it replaces: the end of WorkerMain makes it.
        if (rebuild) {
            LOCK(m_mutex);
            m_rebuild_requested = true;
        }
        return false;
    };
    if (!run([this] { return WriteFirstBatch(); })) return;
    while (true) {
        if (m_hooks.hold) m_hooks.hold->Pass();
        if (self && self.use_count() == 1) {
            // The record's owners have let go of it without stop(): it ends, writing nothing more.
            {
                LOCK(m_mutex);
                m_crashed = true;
                m_accepting = false;
            }
            Unregister();
            return;
        }
        std::deque<std::unique_ptr<Item>> items;
        bool draining;
        bool ending;
        {
            LOCK(m_mutex);
            if (m_state == State::FAILED || m_crashed) return;
            // The items taken stay in the queue's count until they are written (ApplyItems).
            items.swap(m_queue);
            if (!m_ending && (m_stopping || m_off_requested || m_rebuild_requested)) {
                // The run ends: what is asked from now on waits for its end.
                m_ending = true;
                rebuild = m_rebuild_requested;
                m_off_requested = false;
                m_rebuild_requested = false;
            }
            ending = m_ending;
            draining = m_draining || ending;
            if (ending && m_accepting) {
                // Switched off or rebuilt while running: nothing more is queued, and what is queued has its deadline.
                m_accepting = false;
            }
            if (ending && m_deadline == Clock::time_point{}) m_deadline = Clock::now() + STOP_DEADLINE;
            if (draining && !m_drain_seen) {
                m_drain_seen = true;
                m_state_cv.notify_all();
            }
        }
        if (ending) Unregister();
        if (!items.empty() && !run([&] { return ApplyItems(items); })) return;
        {
            std::deque<FailedRead> confirms;
            {
                LOCK(m_mutex);
                confirms.swap(m_confirms);
            }
            if (!confirms.empty() && !run([&] {
                    for (const FailedRead& confirm : confirms) Confirm(confirm);
                    return true;
                })) {
                return;
            }
        }
        if (ending) {
            {
                LOCK(m_mutex);
                // Blocks queued after the last look are written too, while the deadline lasts.
                if (!m_queue.empty()) continue;
            }
            if (!run([this] { return Finish(true); })) return;
            {
                LOCK(m_mutex);
                if (m_stopping) return; // stop() closes the database
            }
            CloseDatabase();
            {
                LOCK(m_mutex);
                m_draining = false;
                m_drain_seen = false;
                m_deadline = Clock::time_point{};
                m_queue_bytes = 0;
                if (!rebuild) SetState(State::OFF);
            }
            SendChanges(true);
            // Switched off: the end of WorkerMain acts on what was asked meanwhile.
            if (!rebuild) return;
            // The rebuild is made now; from here on a failure is the new record's.
            rebuild = false;
            if (!Open(/*wipe=*/true)) return;
            if (!run([this] { return WriteFirstBatch(); })) return;
            {
                LOCK(m_mutex);
                // Built again, so the record is on: a switch on or a rebuild asked meanwhile is done, and a switch
                // off is made now.
                m_ending = false;
                m_rebuild_requested = false;
                if (m_switch_pending == Pending::OFF) m_off_requested = true;
                m_switch_pending = Pending::NONE;
            }
            continue;
        }
        bool worked = !items.empty();
        // Coins are looked up when nothing else is to do and the last long hold of cs_main has had its pause.
        const bool pausing = !worked && !draining && Clock::now() < m_w->next_cs_main;
        if (!worked && !draining && !pausing) {
            bool resolved = false;
            if (!run([&] { resolved = ResolvePending(); return true; })) return;
            worked = resolved;
        }
        SendChanges(false);
        if (worked) continue;
        WAIT_LOCK(m_mutex, lock);
        const auto ready = [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) {
            return !m_queue.empty() || !m_confirms.empty() || m_stopping || m_off_requested || m_rebuild_requested ||
                   m_crashed || m_state == State::FAILED || (m_draining && !m_drain_seen);
        };
        if (ready()) continue;
        // A test's hold is looked at often, so that the worker stops there soon after the test closes it. Otherwise,
        // since nothing wakes the worker when the record's owners let go of it, it looks now and then.
        Clock::time_point wake = Clock::now() + (m_hooks.hold ? std::chrono::milliseconds{10} : OWNER_CHECK_INTERVAL);
        if (m_change.kinds != 0) wake = std::min(wake, m_last_notify + NOTIFY_INTERVAL);
        // A pause skipped the lookup: the worker wakes when it ends, also when it ended just now.
        if (pausing) wake = std::min(wake, m_w->next_cs_main);
        m_work_cv.wait_until(lock, wake);
    }
}

bool Record::WriteFirstBatch()
{
    WorkerState& w = *m_w;
    if (!w.is_new && !w.cut_at_start) return true;
    Batch batch(*w.db, BatchInfo::Kind::FIRST);
    if (w.is_new) {
        RecordMeta meta;
        meta.genesis_hash = Params().GenesisBlock().GetHash();
        meta.created_time = GetTime();
        meta.created_by = FormatFullVersion().substr(0, MAX_CREATED_BY_SIZE);
        batch.batch.Write(uint8_t{prefix::META}, meta);
        batch.batch.Write(uint8_t{prefix::END}, NoValue());
    }
    batch.coverage_changed = true;
    batch.stats_changed = true;
    if (!WriteBatch(batch)) return false;
    if (w.is_new) {
        LOCK(m_mutex);
        m_db = w.db;
    }
    w.is_new = false;
    w.cut_at_start = false;
    return true;
}

bool Record::ApplyItems(std::deque<std::unique_ptr<Item>>& items)
{
    WorkerState& w = *m_w;
    std::unique_ptr<Batch> batch;
    // The queued bytes of the items in `batch`, which leave the queue's count once it is written.
    size_t batch_bytes = 0;
    const auto flush = [&]() -> bool {
        if (!batch) return true;
        std::unique_ptr<Batch> done = std::move(batch);
        if (done->items > 0 && !WriteBatch(*done)) return false;
        LOCK(m_mutex);
        m_queue_bytes -= std::min(m_queue_bytes, batch_bytes);
        batch_bytes = 0;
        return true;
    };
    while (!items.empty()) {
        {
            LOCK(m_mutex);
            if (m_deadline != Clock::time_point{} && Clock::now() >= m_deadline) {
                // Past the deadline of a stop: the blocks left are not written, and become holes.
                items.clear();
                m_queue.clear();
                m_queue_bytes = 0;
                break;
            }
        }
        Item& item = *items.front();
        const int32_t height = item.index->nHeight;
        const bool with_rows = item.kind == Item::Kind::CONNECT && item.rows;
        if (batch && (with_rows || batch->rows || batch->touched.count(height) || batch->items >= MAX_JOINED_ITEMS)) {
            if (!flush()) return false;
        }
        if (!batch) batch = std::make_unique<Batch>(*w.db, BatchInfo::Kind::ITEMS);
        Batch& b = *batch;
        Reader reader(*this, w.db);
        if (item.kind == Item::Kind::CONNECT) {
            const int32_t old_tip = w.TipHeight();
            // The replace rule: what the height held before goes first.
            BlockMeta old;
            if (reader.Point(HeightKey(prefix::BLOCK, height), old)) EraseHeight(b, height, old);
            // The tip move rule; the height the block itself takes is covered again below.
            MoveTip(b, item.index);
            if (item.rows) {
                const BlockRows& rows = *item.rows;
                uint64_t others = 0;
                uint64_t script_bytes = 0;
                for (size_t i = 0; i < rows.rows.size(); ++i) {
                    const RowKey key{height, uint32_t(i)};
                    const Row& row = rows.rows[i];
                    b.batch.Write(RowDbKey(key), row);
                    others += StoredSize(RowDbKey(key), row);
                    script_bytes += row.script_size;
                    const std::vector<unsigned char>& script = rows.long_scripts[i];
                    for (uint32_t k = 0; k < PieceCount(script.size()); ++k) {
                        const Span<const unsigned char> piece = PieceOf(script, k);
                        ScriptPiece value;
                        value.bytes.assign(piece.begin(), piece.end());
                        b.batch.Write(PieceDbKey(key, k), value);
                        others += StoredSize(PieceDbKey(key, k), value);
                    }
                }
                for (const auto& tx : rows.txs) {
                    const TxLocation location{height, tx.first};
                    b.batch.Write(TxDbKey({height, tx.first}), tx.second);
                    b.batch.Write(TxidDbKey(tx.second.txid), location);
                    others += StoredSize(TxDbKey({height, tx.first}), tx.second) + StoredSize(TxidDbKey(tx.second.txid), location);
                }
                b.batch.Write(HeightKey(prefix::PROOF, height), rows.proof);
                others += StoredSize(HeightKey(prefix::PROOF, height), rows.proof);
                if (rows.pending) {
                    b.batch.Write(HeightKey(prefix::PENDING, height), NoValue());
                    others += StoredSize(HeightKey(prefix::PENDING, height), NoValue());
                    w.pending.insert(height);
                }
                BlockMeta meta;
                meta.hash = rows.hash;
                meta.time = rows.time;
                meta.ntx = rows.ntx;
                meta.nrows = uint32_t(rows.rows.size());
                SetLogicalBytes(meta, others);
                b.batch.Write(HeightKey(prefix::BLOCK, height), meta);
                w.stats.rows += rows.rows.size();
                w.stats.txs += rows.txs.size();
                w.stats.blocks += 1;
                w.stats.script_bytes += script_bytes;
                w.stats.logical_bytes += meta.logical_bytes;
                b.stats_changed = true;
                b.rows = true;
                b.new_rows += uint32_t(rows.rows.size());
                b.touched.insert(height);
            }
            Cover(b, height);
            // Heights covered above the old tip that the new tip makes visible.
            for (const HeightRange& range : w.coverage.Ranges()) {
                if (range.hi <= old_tip) continue;
                if (range.lo > height) break;
                Extend(b.added, std::max({range.lo, old_tip + 1, w.floor}), std::min(range.hi, height));
            }
            Extend(b.added, height, height);
            b.connected.emplace_back(height, item.rows ? uint32_t(item.rows->rows.size()) : 0U);
        } else if (item.kind == Item::Kind::DISCONNECT) {
            BlockMeta old;
            if (reader.Point(HeightKey(prefix::BLOCK, height), old) && old.hash == item.index->GetBlockHash()) {
                EraseHeight(b, height, old);
            }
            Uncover(b, {height, height});
            MoveTip(b, item.index->pprev);
        } else {
            // A block left out of the queue: its height is a hole.
            MoveTip(b, item.index);
        }
        b.live = true;
        ++b.items;
        batch_bytes += item.bytes;
        items.pop_front();
        if (with_rows && !flush()) return false;
    }
    return flush();
}

void Record::MoveTip(Batch& batch, const CBlockIndex* to)
{
    WorkerState& w = *m_w;
    const CBlockIndex* fork = CommonAncestor(w.tip, to);
    // Every covered height above the fork leaves coverage, up to the higher tip: what the old branch held there does
    // not hold for the new one, also at the height of a connected block, which writes it again and covers it, even
    // when it held no rows; and heights above the new tip stop being visible.
    if (!fork || fork->nHeight < std::numeric_limits<int32_t>::max()) {
        Uncover(batch, {fork ? fork->nHeight + 1 : 0, std::max(to ? to->nHeight : -1, w.TipHeight())});
    }
    w.tip = to;
}

void Record::Uncover(Batch& batch, HeightRange range)
{
    WorkerState& w = *m_w;
    range.lo = std::max(range.lo, 0);
    if (range.lo > range.hi) return;
    bool covered = false;
    for (const HeightRange& r : w.coverage.Ranges()) {
        if (r.hi < range.lo) continue;
        if (r.lo > range.hi) break;
        covered = true;
        Extend(batch.removed, std::max({r.lo, range.lo, w.floor}), std::min({r.hi, range.hi, w.TipHeight()}));
    }
    if (!covered) return;
    w.coverage.Remove(range);
    batch.info.uncovers = true;
    batch.coverage_changed = true;
}

void Record::Cover(Batch& batch, int32_t height)
{
    WorkerState& w = *m_w;
    w.coverage.Add({height, height});
    batch.coverage_changed = true;
    while (w.coverage.RangeCount() > MAX_COVERAGE_RANGES) {
        // The list of covered ranges is full: the smallest other range leaves coverage, and its heights are read
        // again later.
        const std::vector<HeightRange>& ranges = w.coverage.Ranges();
        const HeightRange* smallest = nullptr;
        for (const HeightRange& r : ranges) {
            if (r.lo <= height && height <= r.hi) continue;
            if (!smallest || int64_t{r.hi} - r.lo < int64_t{smallest->hi} - smallest->lo) smallest = &r;
        }
        if (!smallest) break;
        const HeightRange gone = *smallest;
        batch.uncovered_for_limit += uint64_t(int64_t{gone.hi} - gone.lo + 1);
        Uncover(batch, gone);
    }
}

void Record::EraseHeight(Batch& batch, int32_t height, const BlockMeta& meta)
{
    WorkerState& w = *m_w;
    Reader reader(*this, w.db);
    uint64_t txs = 0;
    uint64_t script_bytes = 0;
    // The rows: as many as the block entry counts, or the scan passed over a part of the database it could not read.
    uint32_t n = 0;
    for (reader.Seek(KeyBytes(RowDbKey({height, 0}))); reader.Prefix() == prefix::ROW; reader.Next()) {
        RowDbKey key;
        reader.KeyAs(key);
        if (key.key.height != height) break;
        ++n;
        Row row;
        reader.Value(row);
        script_bytes += row.script_size;
        batch.batch.Erase(RawBytes{reader.Key()});
        if (!row.IsFirst()) continue;
        ++txs;
        // The location of the transaction goes only when it points here: the transaction may have moved.
        TxLocation location;
        if (reader.Point(TxidDbKey(row.brief.txid), location) && location.height == height && location.txpos == row.txpos) {
            batch.batch.Erase(TxidDbKey(row.brief.txid));
        }
    }
    if (n != meta.nrows) throw ReadError();
    for (reader.Seek(KeyBytes(PieceDbKey({height, 0}, 0))); reader.Prefix() == prefix::PIECE; reader.Next()) {
        PieceDbKey key;
        reader.KeyAs(key);
        if (key.key.height != height) break;
        batch.batch.Erase(RawBytes{reader.Key()});
    }
    for (reader.Seek(KeyBytes(TxDbKey({height, 0}))); reader.Prefix() == prefix::TX; reader.Next()) {
        TxDbKey key;
        reader.KeyAs(key);
        if (key.key.height != height) break;
        batch.batch.Erase(RawBytes{reader.Key()});
    }
    for (reader.Seek(KeyBytes(SpentDbKey({height, 0}, 0))); reader.Prefix() == prefix::SPENT; reader.Next()) {
        SpentDbKey key;
        reader.KeyAs(key);
        if (key.tx.height != height) break;
        batch.batch.Erase(RawBytes{reader.Key()});
    }
    batch.batch.Erase(HeightKey(prefix::PROOF, height));
    batch.batch.Erase(HeightKey(prefix::PENDING, height));
    batch.batch.Erase(HeightKey(prefix::BLOCK, height));
    w.pending.erase(height);
    w.stats.rows -= std::min<uint64_t>(w.stats.rows, meta.nrows);
    w.stats.txs -= std::min(w.stats.txs, txs);
    w.stats.blocks -= std::min<uint64_t>(w.stats.blocks, 1);
    w.stats.script_bytes -= std::min(w.stats.script_bytes, script_bytes);
    w.stats.logical_bytes -= std::min(w.stats.logical_bytes, meta.logical_bytes);
    batch.stats_changed = true;
    batch.info.erases = true;
    batch.touched.insert(height);
    if (w.IsVisible(height)) Extend(batch.removed, height, height);
}

bool Record::ResolvePending()
{
    WorkerState& w = *m_w;
    int32_t height = -1;
    for (auto it = w.pending.rbegin(); it != w.pending.rend(); ++it) {
        if (w.IsVisible(*it)) {
            height = *it;
            break;
        }
    }
    if (height < 0) return false;
    const CBlockIndex* index = w.tip->GetAncestor(height);
    Reader reader(*this, w.db);
    Batch batch(*w.db, BatchInfo::Kind::PENDING);
    BlockMeta meta;
    if (!reader.Point(HeightKey(prefix::BLOCK, height), meta)) {
        // A mark without rows: nothing to look up.
        batch.batch.Erase(HeightKey(prefix::PENDING, height));
        w.pending.erase(height);
        return WriteBatch(batch);
    }
    if (!index || meta.hash != index->GetBlockHash()) {
        // A covered height holds the rows of the record's tip's ancestor; anything else is a fault of the record.
        Fail(FailReason::INTERNAL);
        return false;
    }
    CBlockUndo undo;
    bool have_undo = false;
    WithCsMain(w.next_cs_main, [&]() EXCLUSIVE_LOCKS_REQUIRED(cs_main) {
        if (index->nStatus & BLOCK_HAVE_UNDO) have_undo = UndoReadFromDisk(undo, index);
        if (m_hooks.in_cs_main) m_hooks.in_cs_main();
    });
    const uint64_t old_meta_size = StoredSize(HeightKey(prefix::BLOCK, height), meta);
    int64_t delta = -int64_t(StoredSize(HeightKey(prefix::PENDING, height), NoValue()));
    std::vector<std::pair<TxKey, TxRecord>> records;
    std::unique_ptr<Batch> scripts;
    for (reader.Seek(KeyBytes(TxDbKey({height, 0}))); reader.Prefix() == prefix::TX; reader.Next()) {
        TxDbKey key;
        reader.KeyAs(key);
        if (key.key.height != height) break;
        records.emplace_back(key.key, TxRecord());
        reader.Value(records.back().second);
    }
    for (auto& entry : records) {
        TxRecord& record = entry.second;
        if (!std::any_of(record.vin.begin(), record.vin.end(), [](const TxInRecord& in) { return in.spent_state == SpentState::PENDING; })) continue;
        uint32_t first_n = std::numeric_limits<uint32_t>::max();
        for (const TxOutRecord& o : record.vout) {
            if (o.kind == TxOutRecord::Kind::ROW) first_n = std::min(first_n, o.row_n);
        }
        Row first;
        const RowKey first_key{height, first_n};
        if (!reader.Point(RowDbKey(first_key), first) || !first.IsFirst() || first.txpos != entry.first.txpos) {
            throw ReadError();
        }
        const std::vector<Coin>* coins = nullptr;
        if (have_undo && entry.first.txpos >= 1 && entry.first.txpos - 1 < undo.vtxundo.size()) {
            coins = &undo.vtxundo[entry.first.txpos - 1].vprevout;
        }
        delta -= int64_t(StoredSize(TxDbKey(entry.first), record) + StoredSize(RowDbKey(first_key), first));
        std::vector<bool> resolving(record.vin.size());
        for (size_t i = 0; i < record.vin.size(); ++i) resolving[i] = record.vin[i].spent_state == SpentState::PENDING;
        ResolveSpentCoins(record, first.brief, coins);
        // The scripts the record does not hold go under their own keys, in batches written before the one that holds
        // the record: until that one is written the height stays pending, and they are written again.
        for (uint32_t i = 0; i < record.vin.size(); ++i) {
            if (!resolving[i] || record.vin[i].script_apart == 0) continue;
            SpentScript value;
            value.script = (*coins)[i].out.scriptPubKey;
            const SpentDbKey key(entry.first, i);
            delta += int64_t(StoredSize(key, value));
            if (!scripts) scripts = std::make_unique<Batch>(*w.db, BatchInfo::Kind::PENDING);
            scripts->batch.Write(key, value);
            if (scripts->batch.SizeEstimate() >= SPENT_BATCH_BYTES) {
                if (!WriteBatch(*scripts)) return false;
                scripts.reset();
            }
        }
        delta += int64_t(StoredSize(TxDbKey(entry.first), record) + StoredSize(RowDbKey(first_key), first));
        batch.batch.Write(TxDbKey(entry.first), record);
        batch.batch.Write(RowDbKey(first_key), first);
    }
    if (scripts && !WriteBatch(*scripts)) return false;
    // A step that ended before its last batch can have left scripts that no record refers to now (when this block's
    // undo data went meanwhile): they go, uncounted as they were.
    std::set<std::pair<uint32_t, uint32_t>> referenced;
    for (const auto& entry : records) {
        for (uint32_t i = 0; i < entry.second.vin.size(); ++i) {
            if (entry.second.vin[i].script_apart > 0) referenced.emplace(entry.first.txpos, i);
        }
    }
    Reader scan(*this, w.db);
    for (scan.Seek(KeyBytes(SpentDbKey({height, 0}, 0))); scan.Prefix() == prefix::SPENT; scan.Next()) {
        SpentDbKey key;
        scan.KeyAs(key);
        if (key.tx.height != height) break;
        if (!referenced.count({key.tx.txpos, key.n})) batch.batch.Erase(RawBytes{scan.Key()});
    }
    batch.batch.Erase(HeightKey(prefix::PENDING, height));
    const uint64_t old_logical = meta.logical_bytes;
    SetLogicalBytes(meta, uint64_t(int64_t(old_logical - old_meta_size) + delta));
    batch.batch.Write(HeightKey(prefix::BLOCK, height), meta);
    w.stats.logical_bytes = w.stats.logical_bytes - std::min(w.stats.logical_bytes, old_logical) + meta.logical_bytes;
    batch.stats_changed = true;
    Extend(batch.updated, height, height);
    w.pending.erase(height);
    return WriteBatch(batch);
}

bool Record::WriteBatch(Batch& batch)
{
    WorkerState& w = *m_w;
    if (batch.coverage_changed) {
        StoredCoverage stored;
        stored.ranges = w.coverage.Ranges();
        stored.floor = w.floor;
        // The heights that could not be read and are still not covered.
        if (!w.unreadable.Empty()) {
            for (const HeightRange& range : w.coverage.Ranges()) w.unreadable.Remove(range);
        }
        stored.unreadable = w.unreadable.Ranges();
        batch.batch.Write(uint8_t{prefix::COVERAGE}, stored);
    }
    if (batch.stats_changed) batch.batch.Write(uint8_t{prefix::STATS}, w.stats);
    if (batch.live && w.tip) batch.batch.Write(uint8_t{prefix::TIP}, LocatorOf(w.tip));
    // Raised before the batch reaches the database, so that a reader who reads its data sees the new value.
    batch.info.removals = (batch.info.erases || batch.info.uncovers) ? ++m_removals : m_removals.load();
    batch.info.coverage = w.coverage;
    batch.info.floor = w.floor;
    batch.info.tip = w.tip;
    batch.info.bytes = batch.batch.SizeEstimate();
    const Clock::time_point now = Clock::now();
    batch.info.sync = batch.info.kind == BatchInfo::Kind::LAST || now - w.last_sync >= SYNC_INTERVAL;
    if (m_hooks.before_write) m_hooks.before_write(batch.info);
    w.db->WriteBatch(batch.batch, batch.info.sync);
    if (batch.info.sync) w.last_sync = now;
    Publish(&batch);
    if (batch.uncovered_for_limit > 0) Log(LogEvent::COVERAGE_FULL, int64_t(MAX_COVERAGE_RANGES), int64_t(batch.uncovered_for_limit));
    const int64_t micros = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - batch.begun).count();
    for (const auto& block : batch.connected) Log(LogEvent::BLOCK, block.first, block.second, micros);
    if (m_hooks.after_write) m_hooks.after_write(batch.info);
    return true;
}

bool Record::Finish(bool sync)
{
    WorkerState& w = *m_w;
    Batch batch(*w.db, sync ? BatchInfo::Kind::LAST : BatchInfo::Kind::ITEMS);
    batch.coverage_changed = true;
    batch.stats_changed = true;
    // The tip locator, unless heights above the record's tip are covered: the stored locator still describes them
    // (start-up case B before a block connected), and one of the tip would make the next start cut them.
    const std::vector<HeightRange>& ranges = w.coverage.Ranges();
    batch.live = w.tip != nullptr && (ranges.empty() || ranges.back().hi <= w.tip->nHeight);
    return WriteBatch(batch);
}

void Record::Publish(Batch* batch)
{
    const WorkerState& w = *m_w;
    auto visibility = std::make_shared<Visibility>();
    visibility->coverage = std::make_shared<const std::vector<HeightRange>>(w.coverage.Ranges());
    const int32_t tip = w.TipHeight();
    auto gaps = std::make_shared<std::vector<GapRange>>();
    for (const HeightRange& range : w.coverage.Missing({std::max(w.floor, 0), tip})) {
        gaps->push_back({range.lo, range.hi, GapReason::PENDING});
    }
    visibility->gaps = gaps;
    visibility->floor = w.floor;
    visibility->live_tip = tip;
    if (w.tip) visibility->live_tip_hash = w.tip->GetBlockHash();
    visibility->removals = m_removals.load();
    int32_t oldest = -1;
    int64_t oldest_time = 0;
    for (const HeightRange& range : w.coverage.Ranges()) {
        if (range.hi < std::max(w.floor, 0)) continue;
        const int32_t lo = std::max(range.lo, std::max(w.floor, 0));
        if (lo <= tip) {
            oldest = lo;
            if (const CBlockIndex* index = w.tip->GetAncestor(lo)) oldest_time = index->GetBlockTime();
        }
        break;
    }
    {
        LOCK(m_mutex);
        visibility->generation = ++m_generation;
        m_visibility = visibility;
        m_tip_index = w.tip;
        m_status.live_tip = tip;
        m_status.live_tip_hash = visibility->live_tip_hash;
        m_status.floor = w.floor;
        m_status.coverage = visibility->coverage;
        m_status.backfill_height = gaps->empty() ? -1 : gaps->back().hi;
        m_status.oldest_visible = oldest;
        m_status.oldest_visible_time = oldest_time;
        m_status.rows = w.stats.rows;
        m_status.txs = w.stats.txs;
        m_status.generation = visibility->generation;
        if (m_state == State::OPENING || m_state == State::RUNNING || m_state == State::BACKFILLING) {
            SetState(gaps->empty() ? State::RUNNING : State::BACKFILLING);
        }
        m_change.kinds |= RecordChange::STATUS;
        if (batch) {
            if (batch->added.lo <= batch->added.hi) {
                m_change.kinds |= RecordChange::ROWS_ADDED;
                Extend(m_change.added, batch->added.lo, batch->added.hi);
            }
            if (batch->removed.lo <= batch->removed.hi) {
                m_change.kinds |= RecordChange::ROWS_REMOVED;
                Extend(m_change.removed, batch->removed.lo, batch->removed.hi);
            }
            if (batch->updated.lo <= batch->updated.hi) {
                m_change.kinds |= RecordChange::ROWS_UPDATED;
                Extend(m_change.updated, batch->updated.lo, batch->updated.hi);
            }
            m_change.new_rows += batch->new_rows;
        }
    }
    m_tip_cv.notify_all();
}

void Record::SendChanges(bool now)
{
    RecordChange change;
    {
        LOCK(m_mutex);
        if (m_change.kinds == 0) return;
        const Clock::time_point time = Clock::now();
        if (!now && !m_change_now && time < m_last_notify + NOTIFY_INTERVAL) return;
        change = m_change;
        change.generation = m_generation;
        m_change = RecordChange();
        m_change_now = false;
        m_last_notify = time;
    }
    m_signals->changed(change);
}

void Record::CloseDatabase()
{
    std::shared_ptr<CDBWrapper> db;
    {
        WAIT_LOCK(m_mutex, lock);
        db = std::move(m_db);
        m_db.reset();
        // Reads in progress end first; new ones find no database.
        m_state_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_readers == 0; });
        m_visibility = EmptyVisibility(++m_generation, ++m_removals);
        m_tip_index = nullptr;
        m_status.live_tip = -1;
        m_status.live_tip_hash.SetNull();
        m_status.coverage = m_visibility->coverage;
        m_status.backfill_height = -1;
        m_status.oldest_visible = -1;
        m_status.oldest_visible_time = 0;
        m_status.generation = m_generation;
    }
    m_w.reset();
    db.reset();
}

void Record::flush()
{
    WAIT_LOCK(m_mutex, lock);
    if (!m_worker_running) return;
    m_draining = true;
    m_drain_seen = false;
    m_work_cv.notify_all();
    m_state_cv.wait_for(lock, FLUSH_WAIT, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_drain_seen || !m_worker_running; });
}

void Record::stop()
{
    {
        LOCK(m_mutex);
        // Not from a listener that the worker calls: the worker cannot wait for its own end.
        if (m_worker.get_id() == std::this_thread::get_id()) return;
        if (m_state == State::STOPPED) return;
        m_stopping = true;
        m_accepting = false;
        if (m_deadline == Clock::time_point{}) m_deadline = Clock::now() + STOP_DEADLINE;
        if (m_state != State::FAILED && m_state != State::OFF) SetState(State::STOPPING);
    }
    Unregister();
    DisconnectNodeState();
    if (m_hooks.hold) m_hooks.hold->Open();
    m_work_cv.notify_all();
    SendChanges(true);
    std::thread worker;
    {
        LOCK(m_mutex);
        worker = std::move(m_worker);
    }
    if (worker.joinable()) worker.join();
    CloseDatabase();
    {
        LOCK(m_mutex);
        SetState(State::STOPPED);
        // No block index entry is used after this: the node may free them.
        m_node_tip = nullptr;
        m_queue.clear();
        m_queue_bytes = 0;
    }
    m_tip_cv.notify_all();
    SendChanges(true);
}

void Record::start()
{
    const CBlockIndex* tip;
    const CBlockIndex* header;
    bool initial_download;
    {
        LOCK(cs_main);
        tip = ::ChainActive().Tip();
        header = pindexBestHeader;
        initial_download = ::ChainstateActive().IsInitialBlockDownload();
    }
    {
        LOCK(m_mutex);
        m_node_known = true;
        m_node_tip = tip;
        if (tip) {
            m_node_tip_height = tip->nHeight;
            m_node_tip_time = tip->GetBlockTime();
            m_node_tip_hash = tip->GetBlockHash();
        }
        m_initial_download = initial_download;
        if (header) {
            m_header_tip = header->nHeight;
            m_header_tip_time = header->GetBlockTime();
        }
        m_change.kinds |= RecordChange::STATUS;
    }
    m_tip_cv.notify_all();
}

void Record::setRecordEnabled(bool enabled)
{
    LOCK(m_mutex);
    if (m_stopping) return;
    if (m_worker_running && m_ending) {
        // The worker is ending its run: the switch waits for the end, the last one asked counting.
        m_switch_pending = enabled ? Pending::ON : Pending::OFF;
        return;
    }
    if (enabled) {
        // A failed record is rebuilt, not switched on.
        m_off_requested = false;
        if (!m_worker_running && m_state == State::OFF) StartWorker(Job::OPEN);
    } else if (m_worker_running && m_state != State::FAILED) {
        m_off_requested = true;
        m_work_cv.notify_all();
    }
}

void Record::requestRebuild()
{
    LOCK(m_mutex);
    if (m_stopping) return;
    if (!m_worker_running) {
        StartWorker(Job::REBUILD);
    } else {
        // Made by the running worker, or at the end of its run when it is ending one (also after a failure).
        m_rebuild_requested = true;
        m_work_cv.notify_all();
    }
}

std::unique_ptr<interfaces::Handler> Record::handleChanged(std::function<void(const RecordChange&)> fn)
{
    // An exception of a listener's is dropped, so that it never leaves the worker or a validation callback.
    return interfaces::MakeHandler(m_signals->changed.connect([fn = std::move(fn)](const RecordChange& change) {
        try {
            fn(change);
        } catch (...) {
        }
    }));
}

RecordStatus Record::getStatus()
{
    RecordStatus status;
    {
        LOCK(m_mutex);
        status = m_status;
        status.state = m_state;
        status.reason = m_reason;
        status.node_known = m_node_known;
        status.initial_download = m_initial_download;
        status.node_tip = m_node_tip_height;
        status.node_tip_time = m_node_tip_time;
        status.node_tip_hash = m_node_tip_hash;
        status.header_tip = m_header_tip;
        status.header_tip_time = m_header_tip_time;
    }
    status.read_errors = m_read_errors.load();
    return status;
}

VisibilityRef Record::getVisibility()
{
    LOCK(m_mutex);
    return m_visibility;
}

uint64_t Record::getRemovals()
{
    return m_removals.load();
}

VisibilityRef Record::UseVisibility(const VisibilityRef& given)
{
    return given ? given : getVisibility();
}

std::shared_ptr<CDBWrapper> Record::BeginRead()
{
    LOCK(m_mutex);
    if (!m_db) return nullptr;
    ++m_readers;
    return m_db;
}

void Record::EndRead()
{
    {
        LOCK(m_mutex);
        --m_readers;
    }
    m_state_cv.notify_all();
}

void Record::ReadFailed(const FailedRead& failed)
{
    ++m_read_errors;
    {
        LOCK(m_mutex);
        if (m_confirms.size() < MAX_CONFIRMS) m_confirms.push_back(failed);
    }
    m_work_cv.notify_all();
}

namespace {

//! The first position after `key` in chain order.
RowKey After(const RowKey& key)
{
    if (key.height < 0) return {0, 0};
    if (key.n == std::numeric_limits<uint32_t>::max()) {
        return key.height == std::numeric_limits<int32_t>::max() ? key : RowKey{key.height + 1, 0};
    }
    return {key.height, key.n + 1};
}

//! The position after every row of a height, also of the largest one.
RowKey PositionAfter(int32_t height)
{
    if (height == std::numeric_limits<int32_t>::max()) return {height, std::numeric_limits<uint32_t>::max()};
    return {height + 1, 0};
}

} // namespace

RowBatch Record::readRows(const RowRequest& request)
{
    RowBatch out;
    Reader reader(*this);
    try {
        ReadRows(reader, request, out);
    } catch (const ReadError&) {
        out.failed = true;
        FailedRead failed;
        failed.call = FailedRead::Call::ROWS;
        failed.rows = request;
        failed.rows.visibility = nullptr;
        ReadFailed(failed);
    }
    return out;
}

void Record::ReadRows(Reader& reader, const RowRequest& request, RowBatch& out)
{
    out.next = request.from;
    const VisibilityRef visibility = UseVisibility(request.visibility);
    const Visibility& v = *visibility;
    out.generation = v.generation;
    if (!reader.Open()) {
        out.end = true;
        return;
    }
    if (request.max_rows == 0) return;
    const int32_t lowest = std::max({request.heights.lo, v.floor, 0});
    const int32_t highest = std::min(request.heights.hi, v.live_tip);
    const std::vector<HeightRange>& ranges = *v.coverage;
    size_t bytes = 0;
    // Adds a row to the batch, unless the batch is full: false then.
    const auto take = [&](const RowKey& key, Row& row, const TxBrief& brief) {
        if (out.rows.size() >= request.max_rows) return false;
        // Each row carries its transaction's paying script too.
        const size_t size = std::min<size_t>(row.head.size(), request.max_script) + brief.paid_by.size();
        if (!out.rows.empty() && bytes + size > request.max_bytes) return false;
        bytes += size;
        out.rows.push_back(ToView(key, row, request.max_script, brief));
        out.next = key;
        return true;
    };
    // The brief of a row that is not its transaction's first: from that first row, read through the same scan.
    const auto brief_of = [&](const RowKey& key, const Row& row) {
        if (row.first_delta > key.n) throw ReadError();
        Row first;
        if (!reader.Scanned(RowDbKey({key.height, key.n - row.first_delta}), first) || !first.IsFirst() || first.txpos != row.txpos) {
            throw ReadError();
        }
        return first.brief;
    };
    if (!request.older) {
        // Oldest first. `low` is the first position still to be read.
        RowKey low = After(request.from);
        if (low.height < lowest) low = {lowest, 0};
        for (const HeightRange& range : ranges) {
            const int32_t lo = std::max(range.lo, lowest);
            const int32_t hi = std::min(range.hi, highest);
            if (lo > hi || hi < low.height) continue;
            if (low.height < lo) low = {lo, 0};
            for (int32_t from = low.height; from <= hi;) {
                std::vector<std::pair<int32_t, BlockMeta>> blocks;
                bool cut;
                reader.BlockEntries(from, hi, blocks, cut);
                for (const auto& block : blocks) {
                    const int32_t height = block.first;
                    const uint32_t nrows = block.second.nrows;
                    const uint32_t start = height == low.height ? low.n : 0;
                    if (start >= nrows) {
                        low = {height, std::max(start, nrows)};
                        continue;
                    }
                    // No row between the position and this block's first row to read.
                    const Optional<RowKey> first = reader.FirstRowFrom(low);
                    if (!first || *first != RowKey{height, start}) throw ReadError();
                    TxBrief brief;
                    uint32_t brief_txpos = std::numeric_limits<uint32_t>::max();
                    for (uint32_t n = start; n < nrows; ++n) {
                        if (n > start) reader.Next();
                        const RowKey key{height, n};
                        RowDbKey found;
                        if (reader.Prefix() != prefix::ROW) throw ReadError();
                        reader.KeyAs(found);
                        if (found.key != key) throw ReadError();
                        if (out.rows.size() >= request.max_rows) return;
                        Row row;
                        reader.Value(row);
                        if (row.IsFirst()) {
                            brief = row.brief;
                            brief_txpos = row.txpos;
                        } else if (row.txpos != brief_txpos) {
                            brief = brief_of(key, row);
                            brief_txpos = row.txpos;
                            reader.Seek(KeyBytes(RowDbKey(key)));
                        }
                        if (!take(key, row, brief)) return;
                    }
                    low = {height, nrows};
                }
                if (!cut) break;
                from = blocks.back().first + 1;
            }
            // No row after the range's last block, up to its end.
            const Optional<RowKey> rest = reader.FirstRowFrom(low);
            if (rest && rest->height <= hi) throw ReadError();
            if (hi == std::numeric_limits<int32_t>::max()) break;
            low = {hi + 1, 0};
        }
        out.end = true;
        return;
    }

    // Newest first. `up` is the position before which rows are still to be read.
    RowKey up = request.from;
    if (highest < 0 || up.height < 0) {
        out.end = true;
        return;
    }
    if (up.height > highest) up = PositionAfter(highest);
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        const int32_t lo = std::max(it->lo, lowest);
        const int32_t hi = std::min(it->hi, highest);
        if (lo > hi || !(RowKey{lo, 0} < up)) continue;
        if (PositionAfter(hi) < up) up = PositionAfter(hi);
        int32_t top = up.n == 0 ? up.height - 1 : up.height;
        int32_t span = READ_FIRST_SPAN;
        // Block entries found below the block being read, highest last.
        std::vector<std::pair<int32_t, BlockMeta>> lower;
        while (top >= lo) {
            std::pair<int32_t, BlockMeta> found;
            bool have = false;
            if (!lower.empty()) {
                found = std::move(lower.back());
                lower.pop_back();
                have = true;
            }
            // Otherwise forward reads of block entries over a span below `top`: it doubles while a read finds
            // nothing, and is halved towards its upper end when a read is cut.
            for (int32_t span_hi = top; !have && span_hi >= lo;) {
                const int32_t span_lo = int32_t(std::max<int64_t>(lo, int64_t{span_hi} - span + 1));
                std::vector<std::pair<int32_t, BlockMeta>> entries;
                bool cut;
                reader.BlockEntries(span_lo, span_hi, entries, cut);
                if (cut) {
                    span = std::max<int32_t>(1, (span_hi - span_lo + 1) / 2);
                    continue;
                }
                if (entries.empty()) {
                    span_hi = span_lo - 1;
                    if (span < (1 << 24)) span *= 2;
                    continue;
                }
                found = std::move(entries.back());
                entries.pop_back();
                lower = std::move(entries);
                have = true;
            }
            if (!have) break;
            const int32_t height = found.first;
            const uint32_t nrows = found.second.nrows;
            const uint32_t wanted = height == up.height ? std::min(up.n, nrows) : nrows;
            // No row between this block's last row to read and the position.
            const Optional<RowKey> rest = reader.FirstRowFrom({height, wanted});
            if (rest && *rest < up) throw ReadError();
            uint32_t next = wanted; // rows below it are left
            while (next > 0) {
                const uint32_t k = uint32_t(std::min<size_t>({READ_RANGE_ROWS, request.max_rows - out.rows.size(), size_t{next}}));
                const uint32_t first_n = next - k;
                std::vector<std::pair<RowKey, Row>> range;
                range.reserve(k);
                reader.Seek(KeyBytes(RowDbKey({height, first_n})));
                for (uint32_t i = 0; i < k; ++i) {
                    if (i > 0) reader.Next();
                    RowDbKey key;
                    // The rows of a block are numbered without holes.
                    if (reader.Prefix() != prefix::ROW) throw ReadError();
                    reader.KeyAs(key);
                    if (key.key != RowKey{height, first_n + i}) throw ReadError();
                    range.emplace_back(key.key, Row());
                    reader.Value(range.back().second);
                }
                next = first_n;
                // Each transaction's first row holds its brief; only the lowest transaction of the range can
                // begin below it.
                TxBrief brief;
                uint32_t brief_txpos = std::numeric_limits<uint32_t>::max();
                if (!range.front().second.IsFirst()) {
                    brief = brief_of(range.front().first, range.front().second);
                    brief_txpos = range.front().second.txpos;
                }
                std::vector<TxBrief> briefs(range.size());
                for (size_t i = 0; i < range.size(); ++i) {
                    const Row& row = range[i].second;
                    if (row.IsFirst()) {
                        brief = row.brief;
                        brief_txpos = row.txpos;
                    } else if (row.txpos != brief_txpos) {
                        throw ReadError();
                    }
                    briefs[i] = brief;
                }
                for (size_t i = range.size(); i-- > 0;) {
                    if (!take(range[i].first, range[i].second, briefs[i])) return;
                }
                if (out.rows.size() >= request.max_rows) return;
            }
            up = {height, 0};
            top = height - 1;
        }
        // No row below the range's lowest block, down to its start.
        const Optional<RowKey> rest = reader.FirstRowFrom({lo, 0});
        if (rest && *rest < up) throw ReadError();
        up = {lo, 0};
    }
    out.end = true;
}

ReadResult Record::readRow(const RowKey& key, RowView& out, const VisibilityRef& visibility)
{
    const VisibilityRef v = UseVisibility(visibility);
    Reader reader(*this);
    try {
        return ReadRow(reader, *v, key, out);
    } catch (const ReadError&) {
        FailedRead failed;
        failed.call = FailedRead::Call::ROW;
        failed.key = key;
        ReadFailed(failed);
        return ReadResult::FAILED;
    }
}

ReadResult Record::ReadRow(Reader& reader, const Visibility& v, const RowKey& key, RowView& out)
{
    if (!v.IsVisible(key.height) || !reader.Open()) return ReadResult::NOT_FOUND;
    Row row;
    if (!reader.Point(RowDbKey(key), row)) return ReadResult::NOT_FOUND;
    TxBrief brief = row.brief;
    if (!row.IsFirst()) {
        Row first;
        if (row.first_delta > key.n || !reader.Point(RowDbKey({key.height, key.n - row.first_delta}), first) ||
            !first.IsFirst() || first.txpos != row.txpos) {
            // Two reads that do not agree: the height was written again in between, or the value is wrong.
            if (getRemovals() != v.removals) return ReadResult::NOT_FOUND;
            throw ReadError();
        }
        brief = std::move(first.brief);
    }
    out = ToView(key, row, SCRIPT_HEAD_SIZE, brief);
    return ReadResult::FOUND;
}

ReadResult Record::readScript(const RowKey& key, size_t offset, size_t max_bytes, std::vector<unsigned char>& out,
                              const VisibilityRef& visibility)
{
    const VisibilityRef v = UseVisibility(visibility);
    Reader reader(*this);
    try {
        return ReadScript(reader, *v, key, offset, max_bytes, out);
    } catch (const ReadError&) {
        out.clear();
        FailedRead failed;
        failed.call = FailedRead::Call::SCRIPT;
        failed.key = key;
        failed.offset = offset;
        failed.max_bytes = max_bytes;
        ReadFailed(failed);
        return ReadResult::FAILED;
    }
}

ReadResult Record::ReadScript(Reader& reader, const Visibility& v, const RowKey& key, size_t offset, size_t max_bytes,
                              std::vector<unsigned char>& out)
{
    out.clear();
    if (!v.IsVisible(key.height) || !reader.Open()) return ReadResult::NOT_FOUND;
    Row row;
    if (!reader.Point(RowDbKey(key), row)) return ReadResult::NOT_FOUND;
    if (!reader.ScriptBytes(key, row, offset, max_bytes, out)) {
        out.clear();
        if (getRemovals() != v.removals) return ReadResult::NOT_FOUND;
        throw ReadError();
    }
    return ReadResult::FOUND;
}

ReadResult Record::readTransaction(const TxKey& key, size_t max_row_script, size_t max_spent_script, TxView& out,
                                   const VisibilityRef& visibility)
{
    const VisibilityRef v = UseVisibility(visibility);
    Reader reader(*this);
    try {
        return ReadTransaction(reader, *v, key, max_row_script, max_spent_script, out);
    } catch (const ReadError&) {
        FailedRead failed;
        failed.call = FailedRead::Call::TRANSACTION;
        failed.tx = key;
        failed.max_bytes = max_row_script;
        failed.max_spent = max_spent_script;
        ReadFailed(failed);
        return ReadResult::FAILED;
    }
}

ReadResult Record::ReadTransaction(Reader& reader, const Visibility& v, const TxKey& key, size_t max_row_script,
                                   size_t max_spent_script, TxView& out)
{
    if (!v.IsVisible(key.height) || !reader.Open()) return ReadResult::NOT_FOUND;
    TxRecord record;
    if (!reader.Point(TxDbKey(key), record)) return ReadResult::NOT_FOUND;
    // What the record's other keys must agree with; when they do not, the height was written again meanwhile, or a
    // value is wrong.
    const auto disagree = [&]() {
        if (getRemovals() != v.removals) return ReadResult::NOT_FOUND;
        throw ReadError();
    };
    BlockMeta meta;
    if (!reader.Point(HeightKey(prefix::BLOCK, key.height), meta)) return disagree();
    TxView view;
    view.key = key;
    view.txid = record.txid;
    view.version = record.version;
    view.locktime = record.locktime;
    view.size = record.size;
    view.weight = record.weight;
    view.vsize = record.vsize;
    view.tx_flags = record.tx_flags;
    view.n_pegin = record.n_pegin;
    view.n_pegout = record.n_pegout;
    view.n_kernel = record.n_kernel;
    view.block_hash = meta.hash;
    view.block_time = meta.time;
    // The scripts of spent coins that have keys of their own, in input order while they fit the budget.
    size_t spent_bytes = 0;
    bool spent_cut = false;
    for (uint32_t i = 0; i < record.vin.size(); ++i) {
        TxInRecord& in = record.vin[i];
        TxInView in_view;
        in_view.prevout = in.prevout;
        in_view.script_sig = std::move(in.script_sig);
        in_view.sequence = in.sequence;
        in_view.spent_state = in.spent_state;
        in_view.spent = std::move(in.coin);
        if (in.spent_state == SpentState::KNOWN) {
            const uint32_t held = uint32_t(in_view.spent.out.scriptPubKey.size());
            in_view.spent_script_size = in.script_apart > 0 ? in.script_apart : held;
        }
        if (in.script_apart > 0 && !spent_cut) {
            if (in.script_apart > max_spent_script - spent_bytes) {
                spent_cut = true;
            } else {
                SpentScript spent;
                if (!reader.Point(SpentDbKey(key, i), spent) || spent.script.size() != in.script_apart) {
                    return disagree();
                }
                spent_bytes += in.script_apart;
                in_view.spent.out.scriptPubKey = std::move(spent.script);
            }
        }
        view.vin.push_back(std::move(in_view));
    }
    bool have_fee = false;
    for (TxOutRecord& o : record.vout) {
        TxOutView out_view;
        out_view.value = o.out.nValue;
        if (o.kind == TxOutRecord::Kind::ROW) {
            Row row;
            if (!reader.Point(RowDbKey({key.height, o.row_n}), row) || row.txpos != key.txpos) return disagree();
            out_view.is_row = true;
            out_view.row_n = o.row_n;
            out_view.script_size = row.script_size;
            if (row.IsFirst()) {
                view.fee = row.brief.fee;
                have_fee = true;
            }
            std::vector<unsigned char> bytes;
            if (max_row_script <= row.head.size()) {
                bytes.assign(row.head.begin(), row.head.begin() + max_row_script);
            } else if (!reader.ScriptBytes({key.height, o.row_n}, row, 0, max_row_script, bytes)) {
                return disagree();
            }
            out_view.script = CScript(bytes.begin(), bytes.end());
        } else {
            out_view.script_size = uint32_t(o.out.scriptPubKey.size());
            out_view.script = std::move(o.out.scriptPubKey);
        }
        view.vout.push_back(std::move(out_view));
    }
    if (!have_fee) return disagree();
    out = std::move(view);
    return ReadResult::FOUND;
}

ReadResult Record::readBlock(int32_t height, BlockView& out, const VisibilityRef& visibility)
{
    const VisibilityRef v = UseVisibility(visibility);
    Reader reader(*this);
    try {
        return ReadBlock(reader, *v, height, out);
    } catch (const ReadError&) {
        FailedRead failed;
        failed.call = FailedRead::Call::BLOCK;
        failed.height = height;
        ReadFailed(failed);
        return ReadResult::FAILED;
    }
}

ReadResult Record::ReadBlock(Reader& reader, const Visibility& v, int32_t height, BlockView& out)
{
    if (!v.IsVisible(height) || !reader.Open()) return ReadResult::NOT_FOUND;
    BlockMeta meta;
    if (!reader.Point(HeightKey(prefix::BLOCK, height), meta)) return ReadResult::NOT_FOUND;
    BlockView view;
    if (!reader.Point(HeightKey(prefix::PROOF, height), view.proof)) {
        if (getRemovals() != v.removals) return ReadResult::NOT_FOUND;
        throw ReadError();
    }
    view.height = height;
    view.hash = meta.hash;
    view.time = meta.time;
    view.ntx = meta.ntx;
    view.nrows = meta.nrows;
    out = std::move(view);
    return ReadResult::FOUND;
}

ReadResult Record::findTransaction(const uint256& txid, TxKey& out, const VisibilityRef& visibility)
{
    const VisibilityRef v = UseVisibility(visibility);
    Reader reader(*this);
    try {
        return FindTransaction(reader, *v, txid, out);
    } catch (const ReadError&) {
        FailedRead failed;
        failed.call = FailedRead::Call::FIND;
        failed.txid = txid;
        ReadFailed(failed);
        return ReadResult::FAILED;
    }
}

ReadResult Record::FindTransaction(Reader& reader, const Visibility& v, const uint256& txid, TxKey& out)
{
    if (!reader.Open()) return ReadResult::NOT_FOUND;
    TxLocation location;
    if (!reader.Point(TxidDbKey(txid), location) || !v.IsVisible(location.height)) return ReadResult::NOT_FOUND;
    out = {location.height, location.txpos};
    return ReadResult::FOUND;
}

void Record::Confirm(const FailedRead& failed)
{
    // A reader of its own, which sees the database as it is now, and the snapshot that is current now. The worker is
    // the only writer, so between its batches the two agree: a read that fails now has failed for good.
    Reader reader(*this, m_w->db);
    const VisibilityRef v = getVisibility();
    switch (failed.call) {
    case FailedRead::Call::ROWS: {
        RowRequest request = failed.rows;
        request.visibility = v;
        RowBatch out;
        ReadRows(reader, request, out);
        return;
    }
    case FailedRead::Call::ROW: {
        RowView out;
        ReadRow(reader, *v, failed.key, out);
        return;
    }
    case FailedRead::Call::SCRIPT: {
        std::vector<unsigned char> out;
        ReadScript(reader, *v, failed.key, failed.offset, failed.max_bytes, out);
        return;
    }
    case FailedRead::Call::TRANSACTION: {
        TxView out;
        ReadTransaction(reader, *v, failed.tx, failed.max_bytes, failed.max_spent, out);
        return;
    }
    case FailedRead::Call::BLOCK: {
        BlockView out;
        ReadBlock(reader, *v, failed.height, out);
        return;
    }
    case FailedRead::Call::FIND: {
        TxKey out;
        FindTransaction(reader, *v, failed.txid, out);
        return;
    }
    }
}

bool Record::CaughtUp(const CBlockIndex* target) const
{
    const CBlockIndex* tip = m_tip_index;
    if (!target || !tip || !m_node_tip) return false;
    return tip->GetAncestor(target->nHeight) == target && m_node_tip->GetAncestor(tip->nHeight) == tip;
}

bool Record::waitForTip(int timeout_ms)
{
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(std::max(timeout_ms, 0));
    WAIT_LOCK(m_mutex, lock);
    if (m_state != State::RUNNING && m_state != State::BACKFILLING) return false;
    const CBlockIndex* target = m_node_tip;
    if (!m_node_known || !target) return false;
    if (CaughtUp(target)) {
        m_stalled = false;
        return true;
    }
    // A budget of zero is a poll, which notes no stall.
    if (timeout_ms <= 0 || m_initial_download || m_stalled) return false;
    while (true) {
        const bool timed_out = m_tip_cv.wait_until(lock, deadline) == std::cv_status::timeout;
        if (m_state != State::RUNNING && m_state != State::BACKFILLING) return false;
        // The node has left the block the call began with: its best block now is what the record must reach.
        if (m_node_tip && m_node_tip->GetAncestor(target->nHeight) != target) target = m_node_tip;
        if (CaughtUp(target)) return true;
        if (timed_out || Clock::now() >= deadline) {
            m_stalled = true;
            return false;
        }
    }
}

void Record::BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    try {
        if (m_hooks.in_block_connected) m_hooks.in_block_connected();
        {
            LOCK(m_mutex);
            if (!m_accepting) return;
        }
        auto item = std::make_unique<Item>();
        item->kind = Item::Kind::CONNECT;
        item->index = pindex;
        item->rows = ReadBlockRows(*block, pindex->GetBlockHash());
        item->bytes = item->rows ? item->rows->bytes : 64;
        Push(std::move(item));
    } catch (...) {
        Fail(FailReason::INTERNAL);
    }
}

void Record::BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex)
{
    try {
        auto item = std::make_unique<Item>();
        item->kind = Item::Kind::DISCONNECT;
        item->index = pindex;
        item->bytes = 64;
        Push(std::move(item));
    } catch (...) {
        Fail(FailReason::INTERNAL);
    }
}

void Record::Push(std::unique_ptr<Item> item)
{
    {
        LOCK(m_mutex);
        if (!m_accepting) return;
        if (item->rows && m_queue_bytes + item->bytes > m_hooks.queue_bound) {
            // The queue is full: the block is left for later, and its height becomes a hole.
            item->kind = Item::Kind::DROPPED;
            item->rows.reset();
            item->bytes = 64;
        }
        m_queue_bytes += item->bytes;
        m_queue.push_back(std::move(item));
    }
    m_work_cv.notify_all();
}

} // namespace opreturn
