// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/record.h>

#include <arith_uint256.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <crypto/common.h>
#include <dbwrapper.h>
#include <interfaces/handler.h>
#include <key.h>
#include <miner.h>
#include <node/ui_interface.h>
#include <opreturn/format.h>
#include <opreturn/log.h>
#include <opreturn/names.h>
#include <opreturn/script.h>
#include <pow.h>
#include <script/interpreter.h>
#include <streams.h>
#include <test/data/opreturn_mweb_blocks.json.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <undo.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace opreturn;
using opreturn::test::LogLines;
using opreturn::test::MakeScript;
using opreturn::test::OneDataPush;

namespace {

using Ms = std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;
constexpr uint32_t NO_N{std::numeric_limits<uint32_t>::max()};
//! A budget for readTransaction that reads every spent coin's script.
constexpr size_t ALL_SPENT{std::numeric_limits<size_t>::max()};

template <typename F>
bool WaitUntil(F condition, Ms timeout = Ms{20000})
{
    const auto end = Clock::now() + timeout;
    while (!condition()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(Ms{1});
    }
    return true;
}

CScript OpReturn(const std::string& text)
{
    const std::vector<unsigned char> script = OneDataPush(MakeUCharSpan(text));
    return CScript(script.begin(), script.end());
}

CScript Script(const std::vector<unsigned char>& bytes) { return CScript(bytes.begin(), bytes.end()); }

//! An output the test's key can spend.
struct TestCoin {
    COutPoint outpoint;
    CAmount value{0};
};

//! Problems found on another thread, reported by the test's own thread.
class Problems
{
    Mutex m_mutex;
    std::vector<std::string> m_list GUARDED_BY(m_mutex);

public:
    void Add(const std::string& problem)
    {
        LOCK(m_mutex);
        if (m_list.size() < 20) m_list.push_back(problem);
    }
    std::vector<std::string> List()
    {
        LOCK(m_mutex);
        return m_list;
    }
};

//! Holds the queue of validation callbacks: no callback queued after it is delivered until Release().
class QueueHold
{
    std::promise<void> m_release;
    bool m_released{false};

public:
    QueueHold()
    {
        std::promise<void> held;
        std::future<void> is_held = held.get_future();
        std::shared_future<void> release = m_release.get_future().share();
        CallFunctionInValidationInterfaceQueue([release, held = std::make_shared<std::promise<void>>(std::move(held))]() {
            held->set_value();
            release.wait();
        });
        is_held.wait();
    }
    void Release()
    {
        if (!m_released) m_release.set_value();
        m_released = true;
    }
    ~QueueHold() { Release(); }
};

struct RecordSetup : public TestChain100Setup {
    CScript m_script;
    std::vector<TestCoin> m_coins;
    Mutex m_blocks_mutex;
    std::map<uint256, std::shared_ptr<const CBlock>> m_blocks GUARDED_BY(m_blocks_mutex);
    fs::path m_path;
    unsigned int m_salt{0};
    //! Block index entries made by the test, which are in no block index; freed after the records.
    std::vector<std::unique_ptr<CBlockIndex>> m_fake;
    std::vector<std::unique_ptr<uint256>> m_fake_hashes;

    RecordSetup()
    {
        m_script = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
        // Forty more blocks, so that the coinbases of the first forty are spendable from the next block on.
        for (int i = 0; i < 40; ++i) m_coinbase_txns.push_back(CreateAndProcessBlock({}, m_script).vtx[0]);
        for (size_t i = 0; i < 40; ++i) {
            m_coins.push_back({COutPoint(m_coinbase_txns[i]->GetHash(), 0), m_coinbase_txns[i]->vout[0].nValue});
        }
        m_path = GetDataDir() / "record";
        LOCK2(cs_main, m_blocks_mutex);
        for (const CBlockIndex* index = ::ChainActive().Genesis(); index; index = ::ChainActive().Next(index)) {
            auto block = std::make_shared<CBlock>();
            BOOST_REQUIRE(ReadBlockFromDisk(*block, index, Params().GetConsensus()));
            m_blocks[index->GetBlockHash()] = block;
        }
    }

    TestCoin Take()
    {
        BOOST_REQUIRE(!m_coins.empty());
        const TestCoin coin = m_coins.front();
        m_coins.erase(m_coins.begin());
        return coin;
    }

    //! A transaction spending coins of the test's key; `padding` bytes of pushes go before each signature.
    CMutableTransaction Spend(const std::vector<TestCoin>& coins, const std::vector<CTxOut>& outputs, size_t padding = 0)
    {
        CMutableTransaction tx;
        for (const TestCoin& coin : coins) tx.vin.emplace_back(coin.outpoint);
        tx.vout = outputs;
        for (size_t i = 0; i < coins.size(); ++i) {
            const uint256 hash = SignatureHash(m_script, tx, uint32_t(i), SIGHASH_ALL, coins[i].value, SigVersion::BASE);
            std::vector<unsigned char> signature;
            BOOST_REQUIRE(coinbaseKey.Sign(hash, signature));
            signature.push_back(uint8_t(SIGHASH_ALL));
            CScript script_sig;
            // Pushes of 500 bytes, then the signature: the script is evaluated as pushes, and the key's check only
            // looks at the top of the stack.
            while (padding > 0) {
                const size_t size = std::min<size_t>(500, padding - 3);
                script_sig << std::vector<unsigned char>(size, 0x55);
                padding -= std::min(padding, size + 3);
            }
            script_sig << signature;
            tx.vin[i].scriptSig = script_sig;
        }
        return tx;
    }

    //! A transaction spending one coin, with OP_RETURN outputs and its change to the test's key.
    CMutableTransaction Message(const std::vector<CScript>& scripts, CAmount value = 0)
    {
        const TestCoin coin = Take();
        std::vector<CTxOut> outputs;
        for (const CScript& script : scripts) outputs.emplace_back(value, script);
        outputs.emplace_back(coin.value - 1000 - value * CAmount(scripts.size()), m_script);
        return Spend({coin}, outputs);
    }

    std::shared_ptr<CBlock> MakeBlock(const CBlockIndex* prev, const std::vector<CMutableTransaction>& txs,
                                      const std::vector<CTxOut>& coinbase_outputs = {})
    {
        // Built from an empty pool, so that the coinbase claims no fee of transactions it does not hold.
        CTxMemPool empty_pool;
        std::unique_ptr<CBlockTemplate> block_template = BlockAssembler(empty_pool, Params()).CreateNewBlock(m_script);
        auto block = std::make_shared<CBlock>(block_template->block);
        block->hashPrevBlock = prev->GetBlockHash();
        block->nTime = prev->nTime + 1 + (m_salt++ % 3);
        block->vtx.resize(1);
        for (const CMutableTransaction& tx : txs) block->vtx.push_back(MakeTransactionRef(tx));
        unsigned int extra_nonce = m_salt;
        IncrementExtraNonce(block.get(), prev, extra_nonce);
        // The witness commitment for these transactions, last; the coinbase's other outputs go before it, which
        // leaves the commitment as it is (it does not cover the coinbase).
        CMutableTransaction coinbase(*block->vtx[0]);
        const int commitment = GetWitnessCommitmentIndex(*block);
        BOOST_REQUIRE(commitment != NO_WITNESS_COMMITMENT);
        coinbase.vout.erase(coinbase.vout.begin() + commitment);
        block->vtx[0] = MakeTransactionRef(coinbase);
        GenerateCoinbaseCommitment(*block, prev, Params().GetConsensus());
        coinbase = CMutableTransaction(*block->vtx[0]);
        coinbase.vout.insert(coinbase.vout.end() - 1, coinbase_outputs.begin(), coinbase_outputs.end());
        block->vtx[0] = MakeTransactionRef(coinbase);
        block->hashMerkleRoot = BlockMerkleRoot(*block);
        while (!CheckProofOfWork(block->GetPoWHash(), block->nBits, Params().GetConsensus())) ++block->nNonce;
        LOCK(m_blocks_mutex);
        m_blocks[block->GetHash()] = block;
        return block;
    }

    const CBlockIndex* Process(const std::shared_ptr<CBlock>& block)
    {
        bool fresh = false;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(Params(), block, /*fForceProcessing=*/true, &fresh));
        return WITH_LOCK(cs_main, return LookupBlockIndex(block->GetHash()));
    }

    const CBlockIndex* Tip() { return WITH_LOCK(cs_main, return ::ChainActive().Tip()); }

    //! A block on the active tip, which becomes the tip.
    const CBlockIndex* Mine(const std::vector<CMutableTransaction>& txs = {}, const std::vector<CTxOut>& coinbase_outputs = {})
    {
        const CBlockIndex* index = Process(MakeBlock(Tip(), txs, coinbase_outputs));
        BOOST_REQUIRE(index == Tip());
        return index;
    }

    void Invalidate(const CBlockIndex* index)
    {
        BlockValidationState state;
        BOOST_REQUIRE(InvalidateBlock(state, Params(), const_cast<CBlockIndex*>(index)));
    }

    void Reconsider(const CBlockIndex* index)
    {
        {
            LOCK(cs_main);
            ResetBlockFailureFlags(const_cast<CBlockIndex*>(index));
        }
        BlockValidationState state;
        BOOST_REQUIRE(ActivateBestChain(state, Params()));
    }

    Options RecordOptions(bool reindex = false) const
    {
        Options options;
        options.record = true;
        options.reindex = reindex;
        options.explicit_options = EXPLICIT_RECORD;
        return options;
    }

    std::shared_ptr<Record> Make(RecordTestHooks hooks = {}, bool reindex = false)
    {
        return std::make_shared<Record>(m_path, RecordOptions(reindex), std::move(hooks));
    }

    std::shared_ptr<Record> Start(RecordTestHooks hooks = {}, bool reindex = false)
    {
        std::shared_ptr<Record> record = Make(std::move(hooks), reindex);
        record->load();
        record->start();
        return record;
    }

    //! Waits until the record's tip block is `tip` (the node's tip when none is given).
    void Catch(Record& record, const CBlockIndex* tip = nullptr)
    {
        if (!tip) tip = Tip();
        SyncWithValidationInterfaceQueue();
        const uint256 hash = tip->GetBlockHash();
        BOOST_REQUIRE_MESSAGE(WaitUntil([&] { return record.getVisibility()->live_tip_hash == hash; }),
                              "the record did not reach height " << tip->nHeight << ": state " << int(record.getStatus().state)
                              << " reason " << int(record.getStatus().reason) << " tip " << record.getVisibility()->live_tip);
    }

    //! A block index entry that is in no block index, on `prev`.
    const CBlockIndex* Fake(const CBlockIndex* prev, const uint256& hash)
    {
        m_fake_hashes.push_back(std::make_unique<uint256>(hash));
        auto index = std::make_unique<CBlockIndex>();
        index->pprev = const_cast<CBlockIndex*>(prev);
        index->nHeight = prev->nHeight + 1;
        index->phashBlock = m_fake_hashes.back().get();
        index->BuildSkip();
        m_fake.push_back(std::move(index));
        return m_fake.back().get();
    }

    //! The rows a block has: every output whose script begins with OP_RETURN, except the witness commitment.
    std::vector<std::pair<uint32_t, uint32_t>> Expected(const CBlock& block)
    {
        std::vector<std::pair<uint32_t, uint32_t>> rows;
        const int commitment = GetWitnessCommitmentIndex(block);
        for (size_t pos = 0; pos < block.vtx.size(); ++pos) {
            for (size_t i = 0; i < block.vtx[pos]->vout.size(); ++i) {
                const CScript& script = block.vtx[pos]->vout[i].scriptPubKey;
                if (pos == 0 && int(i) == commitment) continue;
                if (!script.empty() && script[0] == OP_RETURN) rows.emplace_back(uint32_t(pos), uint32_t(i));
            }
        }
        return rows;
    }

    //! Checks that every visible height holds exactly the rows of the block that is the ancestor of `tip` there.
    void CheckHolds(Record& record, const VisibilityRef& v, const CBlockIndex* tip, Problems& problems)
    {
        for (const HeightRange& range : *v->coverage) {
            for (int32_t h = std::max({range.lo, v->floor, 0}); h <= std::min(range.hi, v->live_tip); ++h) {
                const CBlockIndex* index = tip ? tip->GetAncestor(h) : nullptr;
                if (!index) {
                    problems.Add(strprintf("height %d is covered above the tip", h));
                    continue;
                }
                std::shared_ptr<const CBlock> block;
                {
                    LOCK(m_blocks_mutex);
                    const auto it = m_blocks.find(index->GetBlockHash());
                    if (it != m_blocks.end()) block = it->second;
                }
                const std::vector<std::pair<uint32_t, uint32_t>> expected = block ? Expected(*block) : std::vector<std::pair<uint32_t, uint32_t>>{};
                RowRequest request;
                request.from = {h - 1, NO_N};
                request.heights = {h, h};
                request.max_rows = 1 << 20;
                request.max_bytes = std::numeric_limits<size_t>::max();
                request.visibility = v;
                const RowBatch batch = record.readRows(request);
                if (batch.failed) problems.Add(strprintf("height %d could not be read", h));
                if (batch.rows.size() != expected.size()) {
                    problems.Add(strprintf("height %d holds %d rows, its block %d", h, batch.rows.size(), expected.size()));
                    continue;
                }
                for (size_t i = 0; i < expected.size(); ++i) {
                    const RowView& row = batch.rows[i];
                    const CTransaction& tx = *block->vtx[expected[i].first];
                    const CScript& script = tx.vout[expected[i].second].scriptPubKey;
                    if (row.txpos != expected[i].first || row.vout != expected[i].second || row.tx.txid != tx.GetHash() ||
                        row.script_size != script.size() ||
                        !std::equal(row.script_prefix.begin(), row.script_prefix.end(), script.begin())) {
                        problems.Add(strprintf("row %d of height %d is not its block's", i, h));
                    }
                }
            }
        }
    }

    ~RecordSetup()
    {
        // The records were stopped by the cases; the entries they pointed to go last.
        SyncWithValidationInterfaceQueue();
    }
};

std::vector<RowView> AllRows(Record& record, bool older, size_t max_rows = 1 << 20, VisibilityRef v = nullptr)
{
    RowRequest request;
    request.from = older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
    request.older = older;
    request.max_rows = max_rows;
    request.max_bytes = std::numeric_limits<size_t>::max();
    request.visibility = v;
    const RowBatch batch = record.readRows(request);
    BOOST_CHECK(!batch.failed);
    return batch.rows;
}

std::vector<RowView> RowsAt(Record& record, int32_t height)
{
    RowRequest request;
    request.from = {height - 1, NO_N};
    request.heights = {height, height};
    request.max_rows = 1 << 20;
    request.max_bytes = std::numeric_limits<size_t>::max();
    const RowBatch batch = record.readRows(request);
    BOOST_CHECK(!batch.failed);
    return batch.rows;
}

//! The keys of a database the record has closed, as their first bytes and heights where they have one.
std::vector<std::vector<unsigned char>> Keys(const fs::path& path)
{
    std::vector<std::vector<unsigned char>> keys;
    CDBWrapper db(path, 1 << 20, false, false, true);
    std::unique_ptr<CDBIterator> it(db.NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        struct Raw {
            std::vector<unsigned char> bytes;
            void Unserialize(CDataStream& s)
            {
                bytes.resize(s.size());
                if (!bytes.empty()) s.read((char*)bytes.data(), bytes.size());
            }
        } key;
        BOOST_REQUIRE(it->GetKey(key));
        keys.push_back(key.bytes);
    }
    return keys;
}

size_t CountPrefix(const std::vector<std::vector<unsigned char>>& keys, uint8_t prefix_byte)
{
    return size_t(std::count_if(keys.begin(), keys.end(), [&](const std::vector<unsigned char>& k) { return !k.empty() && k[0] == prefix_byte; }));
}

//! Waits until no record holds the database at `path` open: a record that ends on its worker closes it there.
bool WaitUntilClosed(const fs::path& path)
{
    return WaitUntil([&] {
        try {
            CDBWrapper db(path, 1 << 20, false, false, true);
            return true;
        } catch (const dbwrapper_error&) {
            return false;
        }
    });
}

//! A scriptSig of exactly `size` bytes that pushes data and then `signature`.
CScript ScriptSigOfSize(size_t size, const std::vector<unsigned char>& signature)
{
    CScript tail;
    tail << signature;
    BOOST_REQUIRE(size >= tail.size());
    const size_t padding = size - tail.size();
    CScript script;
    while (script.size() + 503 <= padding) script << std::vector<unsigned char>(500, 0x55);
    const size_t left = padding - script.size();
    if (left >= 259) {
        script << std::vector<unsigned char>(left - 3, 0x55);
    } else if (left >= 79) {
        script << std::vector<unsigned char>(std::min<size_t>(left - 2, 255), 0x55);
    } else if (left >= 2) {
        script << std::vector<unsigned char>(std::min<size_t>(left - 1, 75), 0x55);
    }
    while (script.size() < padding) script << OP_0;
    script.insert(script.end(), tail.begin(), tail.end());
    BOOST_REQUIRE_EQUAL(script.size(), size);
    return script;
}

//! A block of one transaction that spends nothing real, for the record's callbacks only.
std::shared_ptr<const CBlock> LooseBlock(const std::vector<CTxOut>& outputs, const CScript& script_sig = CScript() << OP_1)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint(InsecureRand256(), 0), script_sig);
    tx.vout = outputs;
    auto block = std::make_shared<CBlock>();
    block->vtx.push_back(MakeTransactionRef(coinbase));
    block->vtx.push_back(MakeTransactionRef(tx));
    block->hashMerkleRoot = BlockMerkleRoot(*block);
    return block;
}

//! The contents of every file under a directory.
std::string FilesUnder(const fs::path& dir)
{
    std::string all;
    for (fs::recursive_directory_iterator it(dir), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path())) continue;
        std::ifstream file(it->path().string(), std::ios::binary);
        all.append(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return all;
}

//! The MWEB test vectors: blocks around MWEB's activation on regtest, their undo data and what they hold.
UniValue Vectors()
{
    UniValue vectors;
    BOOST_REQUIRE(vectors.read(std::string(json_tests::opreturn_mweb_blocks, json_tests::opreturn_mweb_blocks + sizeof(json_tests::opreturn_mweb_blocks))));
    BOOST_REQUIRE(vectors["blocks"].size() >= 5);
    return vectors;
}

std::shared_ptr<CBlock> VectorBlock(const UniValue& entry)
{
    auto block = std::make_shared<CBlock>();
    CDataStream stream(ParseHex(entry["hex"].get_str()), SER_NETWORK, PROTOCOL_VERSION);
    stream >> *block;
    BOOST_REQUIRE(stream.empty());
    BOOST_REQUIRE_EQUAL(block->GetHash().GetHex(), entry["hash"].get_str());
    return block;
}

CBlockUndo VectorUndo(const UniValue& entry)
{
    const std::vector<unsigned char> bytes = ParseHex(entry["undo"].get_str());
    CDataStream stream(bytes, SER_DISK, CLIENT_VERSION);
    CBlockUndo undo;
    UnserializeBlockUndo(undo, stream, bytes.size());
    return undo;
}

//! The whole script of row n of a block's rows.
CScript WholeScript(const BlockRows& rows, size_t n)
{
    const std::vector<unsigned char>& bytes = rows.long_scripts[n].empty() ? rows.rows[n].head : rows.long_scripts[n];
    return CScript(bytes.begin(), bytes.end());
}

//! A transaction record as the record stores it, rebuilt into the transaction it describes.
CMutableTransaction Rebuild(const TxRecord& stored, const BlockRows& rows)
{
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << stored;
    TxRecord record;
    stream >> Whole<TxRecord>(record);
    CMutableTransaction tx;
    tx.nVersion = record.version;
    tx.nLockTime = record.locktime;
    for (const TxInRecord& in : record.vin) tx.vin.emplace_back(in.prevout, in.script_sig, in.sequence);
    for (const TxOutRecord& o : record.vout) {
        tx.vout.emplace_back(o.out.nValue, o.kind == TxOutRecord::Kind::ROW ? WholeScript(rows, o.row_n) : o.out.scriptPubKey);
    }
    return tx;
}

//! A script of exactly `size` bytes (at least 3) that an empty scriptSig spends: a push of `salt`, pushes of 520 bytes,
//! then OP_1s, so that it leaves fewer than 1,000 items on the stack and a true one on top.
CScript SpendableScript(size_t size, uint8_t salt)
{
    std::vector<unsigned char> bytes{0x01, salt};
    while (size - bytes.size() > 523 + 1) {
        bytes.insert(bytes.end(), {OP_PUSHDATA2, 520 & 0xff, 520 >> 8});
        bytes.insert(bytes.end(), 520, salt);
    }
    while (bytes.size() < size) bytes.push_back(OP_1);
    return CScript(bytes.begin(), bytes.end());
}

struct RawKey {
    std::vector<unsigned char> bytes;
    void Unserialize(CDataStream& s)
    {
        bytes.resize(s.size());
        if (!bytes.empty()) s.read((char*)bytes.data(), bytes.size());
    }
};

//! The bytes of every key and value of a database the record has closed that belong to `height`, as the record's
//! logical bytes count them: its block entry, proof, rows, pieces, transaction records, spent scripts, pending mark,
//! and the locations of its transactions. Also the size of the largest value and the number of keys, by prefix.
struct HeightBytes {
    uint64_t total{0};
    std::map<uint8_t, size_t> keys;
    std::map<uint8_t, size_t> largest;
};

HeightBytes BytesAt(const fs::path& path, int32_t height)
{
    HeightBytes out;
    CDBWrapper db(path, 1 << 20, false, false, true);
    std::unique_ptr<CDBIterator> it(db.NewIterator());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        RawKey key;
        BOOST_REQUIRE(it->GetKey(key));
        const std::vector<unsigned char>& k = key.bytes;
        if (k.empty()) continue;
        bool at = false;
        if (k[0] == prefix::TXID) {
            TxLocation location;
            at = it->GetValue(location) && location.height == height;
        } else if (k.size() >= 5 && std::string("bpPrsti").find(char(k[0])) != std::string::npos) {
            at = ReadBE32(&k[1]) == uint32_t(height);
        }
        if (!at) continue;
        out.total += k.size() + it->GetValueSize();
        ++out.keys[k[0]];
        out.largest[k[0]] = std::max(out.largest[k[0]], size_t(it->GetValueSize()));
    }
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_record_tests, RecordSetup)

BOOST_AUTO_TEST_CASE(rows_from_block)
{
    // Every OP_RETURN output is a row: empty, not push-only, value-burning, several in one transaction, and the
    // coinbase's own; the witness commitment is not, but an earlier output that looks like one is.
    const std::vector<unsigned char> look_alike = ParseHex("6a24aa21a9ed" + std::string(64, '1'));
    const CMutableTransaction many = Message({Script({OP_RETURN}), Script({OP_RETURN, OP_CHECKSIG, 0x05, 0x62}), OpReturn("hello"),
                                              CScript() << OP_RETURN << OP_1 << std::vector<unsigned char>{1, 2}});
    const TestCoin coin = Take();
    const CMutableTransaction burn = Spend({coin}, {CTxOut(5000, OpReturn("burn")), CTxOut(coin.value - 6000, m_script)});
    const CMutableTransaction none = Spend({Take()}, {CTxOut(1000, m_script)});
    const std::shared_ptr<CBlock> block = MakeBlock(Tip(), {none, many, burn}, {CTxOut(0, Script(look_alike)), CTxOut(0, OpReturn("coinbase"))});

    const std::shared_ptr<BlockRows> rows = ReadBlockRows(*block, block->GetHash());
    BOOST_REQUIRE(rows);
    const std::vector<std::pair<uint32_t, uint32_t>> expected = Expected(*block);
    BOOST_REQUIRE_EQUAL(rows->rows.size(), expected.size());
    BOOST_CHECK_EQUAL(expected.size(), 2U + 4U + 1U);
    for (size_t i = 0; i < expected.size(); ++i) {
        const Row& row = rows->rows[i];
        const CTxOut& out = block->vtx[expected[i].first]->vout[expected[i].second];
        BOOST_CHECK_EQUAL(row.txpos, expected[i].first);
        BOOST_CHECK_EQUAL(row.vout, expected[i].second);
        BOOST_CHECK_EQUAL(row.value, out.nValue);
        const ParsedScript parsed = ParseOpReturn(out.scriptPubKey, PARSE_FACTS);
        BOOST_CHECK_EQUAL(row.flags, parsed.flags);
        BOOST_CHECK_EQUAL(row.data_size, parsed.data_size);
        BOOST_CHECK(row.head == std::vector<unsigned char>(out.scriptPubKey.begin(), out.scriptPubKey.end()));
    }
    // The coinbase's look-alike and its message are rows; its last output, the commitment, is not.
    BOOST_CHECK_EQUAL(rows->rows[0].txpos, 0U);
    BOOST_CHECK(rows->rows[0].head == look_alike);
    BOOST_CHECK_EQUAL(GetWitnessCommitmentIndex(*block), int(block->vtx[0]->vout.size()) - 1);
    // A transaction's first row holds its brief, the others refer to it.
    BOOST_CHECK_EQUAL(rows->rows[2].first_delta, 0U);
    BOOST_CHECK_EQUAL(rows->rows[2].brief.n_rows, 4U);
    BOOST_CHECK_EQUAL(rows->rows[5].first_delta, 3U);
    BOOST_CHECK(rows->rows[2].brief.tx_flags & TX_MULTI_OPRETURN);
    BOOST_CHECK(rows->rows[0].brief.paid_by_kind == PaidByKind::COINBASE);
    BOOST_CHECK(rows->rows[2].brief.paid_by_kind == PaidByKind::PENDING);
    BOOST_CHECK_EQUAL(rows->rows[6].value, 5000);
    BOOST_CHECK(rows->pending);
    // The transaction without a row has no record; the proof proves exactly the transactions with rows.
    BOOST_REQUIRE_EQUAL(rows->txs.size(), 3U);
    BOOST_CHECK_EQUAL(rows->txs[0].first, 0U);
    BOOST_CHECK_EQUAL(rows->txs[1].first, 2U);
    BOOST_CHECK_EQUAL(rows->txs[2].first, 3U);
    std::vector<uint256> matched;
    std::vector<unsigned int> positions;
    CPartialMerkleTree proof = rows->proof;
    BOOST_CHECK(proof.ExtractMatches(matched, positions) == block->hashMerkleRoot);
    BOOST_CHECK(positions == std::vector<unsigned int>({0, 2, 3}));

    // Through the running record, in chain order.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* index = Process(block);
    Catch(*record, index);
    const std::vector<RowView> read = RowsAt(*record, index->nHeight);
    BOOST_REQUIRE_EQUAL(read.size(), expected.size());
    for (size_t i = 0; i < read.size(); ++i) {
        BOOST_CHECK_EQUAL(read[i].txpos, expected[i].first);
        BOOST_CHECK_EQUAL(read[i].vout, expected[i].second);
        BOOST_CHECK(read[i].tx.txid == block->vtx[expected[i].first]->GetHash());
        BOOST_CHECK(read[i].key == RowKey({index->nHeight, uint32_t(i)}));
    }
    record->stop();
}

//! Collects the record's notifications.
class Changes
{
    Mutex m_mutex;
    std::vector<RecordChange> m_list GUARDED_BY(m_mutex);
    std::unique_ptr<interfaces::Handler> m_handler;

public:
    explicit Changes(Record& record)
    {
        m_handler = record.handleChanged([this](const RecordChange& change) {
            LOCK(m_mutex);
            m_list.push_back(change);
        });
    }
    ~Changes() { m_handler->disconnect(); }
    std::vector<RecordChange> List()
    {
        LOCK(m_mutex);
        return m_list;
    }
    //! Whether every height from lo to hi was reported as `kind`, in one notification or in several.
    bool ReportedAll(RecordChange::Kind kind, int32_t lo, int32_t hi)
    {
        const std::vector<RecordChange> list = List();
        for (int32_t h = lo; h <= hi; ++h) {
            const bool reported = std::any_of(list.begin(), list.end(), [&](const RecordChange& change) {
                const HeightRange& range = kind == RecordChange::ROWS_ADDED ? change.added : kind == RecordChange::ROWS_REMOVED ? change.removed : change.updated;
                return (change.kinds & kind) && range.lo <= h && h <= range.hi;
            });
            if (!reported) return false;
        }
        return true;
    }
    //! Whether the heights lo to hi were reported, together, as `kind`.
    bool Reported(RecordChange::Kind kind, int32_t lo, int32_t hi)
    {
        for (const RecordChange& change : List()) {
            const HeightRange& range = kind == RecordChange::ROWS_ADDED ? change.added : kind == RecordChange::ROWS_REMOVED ? change.removed : change.updated;
            if ((change.kinds & kind) && range.lo <= lo && hi <= range.hi) return true;
        }
        return false;
    }
};

//! A thread that reads all the time and reports what a reader must never see.
class Watcher
{
    std::atomic<bool> m_done{false};
    std::thread m_thread;

public:
    Problems problems;
    std::atomic<uint64_t> reads{0};

    explicit Watcher(Record& record)
    {
        m_thread = std::thread([this, &record] {
            while (!m_done) {
                const VisibilityRef v = record.getVisibility();
                for (const bool older : {false, true}) {
                    RowRequest request;
                    request.from = older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
                    request.older = older;
                    request.max_rows = 1000;
                    request.visibility = v;
                    const RowBatch batch = record.readRows(request);
                    if (batch.failed) problems.Add("a read failed");
                    for (const RowView& row : batch.rows) {
                        if (row.key.height > v->live_tip) problems.Add(strprintf("a row at height %d above the tip %d", row.key.height, v->live_tip));
                        if (!v->IsVisible(row.key.height)) problems.Add(strprintf("a row at height %d, which is not visible", row.key.height));
                        if (row.tx.n_rows == 0) problems.Add("a row without its brief");
                    }
                    ++reads;
                }
            }
        });
    }
    void Stop()
    {
        m_done = true;
        if (m_thread.joinable()) m_thread.join();
    }
    ~Watcher() { Stop(); }
};

BOOST_AUTO_TEST_CASE(live_connect_visible)
{
    std::shared_ptr<Record> record = Start();
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    {
        // A new record at a tip of 50: every height below is still to be read, nothing is visible.
        const VisibilityRef v = record->getVisibility();
        BOOST_CHECK_EQUAL(v->live_tip, 50);
        BOOST_CHECK(v->live_tip_hash == Tip()->GetBlockHash());
        BOOST_REQUIRE_EQUAL(v->gaps->size(), 1U);
        BOOST_CHECK(v->gaps->front() == GapRange({0, 50, GapReason::PENDING}));
        BOOST_CHECK(v->coverage->empty());
    }
    Watcher watcher(*record);
    for (int i = 0; i < 5; ++i) {
        const CBlockIndex* index = Mine({Message({OpReturn("one"), OpReturn("two")})});
        Catch(*record, index);
        // Readable as soon as the batch that wrote them is published.
        BOOST_CHECK_EQUAL(RowsAt(*record, index->nHeight).size(), 2U);
        BOOST_CHECK(record->getVisibility()->IsVisible(index->nHeight));
    }
    BOOST_CHECK(WaitUntil([&] { return watcher.reads > 10; }));
    watcher.Stop();
    for (const std::string& problem : watcher.problems.List()) BOOST_ERROR(problem);
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(v->coverage->size() == 1 && v->coverage->front() == HeightRange({51, 55}));
    BOOST_CHECK_EQUAL(AllRows(*record, false).size(), 10U);
    const RecordStatus status = record->getStatus();
    BOOST_CHECK_EQUAL(status.live_tip, 55);
    BOOST_CHECK_EQUAL(status.rows, 10U);
    BOOST_CHECK_EQUAL(status.txs, 5U);
    BOOST_CHECK_EQUAL(status.oldest_visible, 51);
    BOOST_CHECK_EQUAL(status.oldest_visible_time, WITH_LOCK(cs_main, return ::ChainActive()[51]->GetBlockTime()));
    BOOST_CHECK_EQUAL(status.backfill_height, 50);
    record->stop();
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    // After stop() reads find nothing.
    BOOST_CHECK(AllRows(*record, false).empty());
    RowView row;
    BOOST_CHECK(record->readRow({51, 0}, row, nullptr) == ReadResult::NOT_FOUND);
}

BOOST_AUTO_TEST_CASE(reorg_disconnect_connect)
{
    std::shared_ptr<Record> record = Start();
    Changes changes(*record);
    const CMutableTransaction moved = Message({OpReturn("moved")});
    const CBlockIndex* first = Mine({moved});
    const CBlockIndex* second = Mine({Message({OpReturn("gone")})});
    Catch(*record);
    TxKey key;
    BOOST_CHECK(record->findTransaction(moved.GetHash(), key, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(key == TxKey({first->nHeight, 1}));

    // Both blocks leave the chain: their rows leave the record, and listeners are told.
    Invalidate(first);
    Catch(*record, first->pprev);
    BOOST_CHECK(RowsAt(*record, first->nHeight).empty());
    BOOST_CHECK(RowsAt(*record, second->nHeight).empty());
    BOOST_CHECK(record->findTransaction(moved.GetHash(), key, nullptr) == ReadResult::NOT_FOUND);
    BOOST_CHECK(WaitUntil([&] { return changes.ReportedAll(RecordChange::ROWS_REMOVED, first->nHeight, second->nHeight); }));
    BOOST_CHECK_EQUAL(record->getStatus().rows, 0U);

    // The moved transaction is mined one block later on the new branch: its location follows it.
    Mine();
    const CBlockIndex* again = Mine({moved});
    Catch(*record);
    BOOST_CHECK(record->findTransaction(moved.GetHash(), key, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(key == TxKey({again->nHeight, 1}));
    BOOST_CHECK_EQUAL(RowsAt(*record, again->nHeight).size(), 1U);
    BOOST_CHECK(RowsAt(*record, first->nHeight).empty());
    BOOST_CHECK(WaitUntil([&] { return changes.Reported(RecordChange::ROWS_ADDED, again->nHeight, again->nHeight); }));

    // Blocks disconnected and a block without rows connected at the same height, all in one queue: the totals
    // stay those of the rows stored.
    record->stop();
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    record = Start(hooks);
    const CBlockIndex* b1 = Mine({Message({OpReturn("b1")})});
    Mine({Message({OpReturn("b2")})});
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    Invalidate(b1);
    Mine();
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    Catch(*record);
    const RecordStatus status = record->getStatus();
    record->stop();
    const std::vector<std::vector<unsigned char>> keys = Keys(m_path);
    BOOST_CHECK_EQUAL(status.rows, CountPrefix(keys, 'r'));
    BOOST_CHECK_EQUAL(status.txs, CountPrefix(keys, 't'));
    // A block's proof goes with its entry.
    BOOST_CHECK_EQUAL(CountPrefix(keys, 'p'), CountPrefix(keys, 'b'));
}

BOOST_AUTO_TEST_CASE(replace_rule)
{
    // Keys and values of one height, read from the closed database.
    const auto height_keys = [&](int32_t height) {
        std::map<std::vector<unsigned char>, std::vector<unsigned char>> found;
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        std::unique_ptr<CDBIterator> it(db.NewIterator());
        struct Raw {
            std::vector<unsigned char> bytes;
            void Unserialize(CDataStream& s)
            {
                bytes.resize(s.size());
                if (!bytes.empty()) s.read((char*)bytes.data(), bytes.size());
            }
        };
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            Raw key, value;
            BOOST_REQUIRE(it->GetKey(key) && it->GetValue(value));
            const uint8_t p = key.bytes[0];
            if ((p == 'b' || p == 'p' || p == 'r' || p == 's' || p == 't' || p == 'P') && key.bytes.size() >= 5) {
                const int32_t h = int32_t(ReadBE32(key.bytes.data() + 1));
                if (h == height) found[key.bytes] = value.bytes;
            }
        }
        return found;
    };
    const auto resolved = [&](Record& record, int32_t height) {
        return WaitUntil([&] {
            RowView row;
            return record.readRow({height, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
        });
    };

    const CMutableTransaction long_tx = Message({Script(MakeScript(5000, 0x41)), OpReturn("a"), OpReturn("b")});
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({long_tx});
    Catch(*record);
    BOOST_REQUIRE(resolved(*record, a->nHeight));
    record->stop();
    const auto keys_a = height_keys(a->nHeight);
    BOOST_CHECK_EQUAL(CountPrefix(Keys(m_path), 's'), 1U); // a script of 5,000 bytes lies in one piece

    // Another block at the same height, with one short row: nothing of the first block stays.
    record = Start();
    Invalidate(a);
    const CBlockIndex* b = Mine({Message({OpReturn("short")})});
    Catch(*record);
    BOOST_REQUIRE(resolved(*record, b->nHeight));
    BOOST_CHECK_EQUAL(RowsAt(*record, b->nHeight).size(), 1U);
    record->stop();
    const auto keys_b = height_keys(b->nHeight);
    BOOST_CHECK_EQUAL(keys_b.size(), 4U); // its block entry, proof, row and transaction record
    BOOST_CHECK_EQUAL(CountPrefix(Keys(m_path), 's'), 0U);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        TxLocation location;
        BOOST_CHECK(!db.Read(TxidDbKey(long_tx.GetHash()), location));
    }

    // The first block again: the height holds exactly what it held the first time.
    record = Start();
    Reconsider(a);
    Mine();
    Catch(*record);
    BOOST_REQUIRE(resolved(*record, a->nHeight));
    record->stop();
    BOOST_CHECK(height_keys(a->nHeight) == keys_a);

    // A height that holds the rows of a block no longer covered (left from before a restart) is written with
    // another block, without a disconnection first: the old rows go too.
    record = Start();
    const CBlockIndex* c = Mine({Message({Script(MakeScript(5000, 0x46)), OpReturn("c1"), OpReturn("c2")})});
    Catch(*record);
    BOOST_REQUIRE(resolved(*record, c->nHeight));
    record->stop();
    BOOST_CHECK_EQUAL(height_keys(c->nHeight).size(), 7U); // block entry, proof, three rows, a piece, a record
    Invalidate(c);
    const CBlockIndex* d = Mine({Message({OpReturn("d")})});
    record = Start();
    BOOST_CHECK(!record->getVisibility()->IsVisible(c->nHeight));
    Invalidate(d);
    const CBlockIndex* e = Mine({Message({OpReturn("e")})});
    Catch(*record, e);
    BOOST_REQUIRE(resolved(*record, e->nHeight));
    BOOST_CHECK_EQUAL(RowsAt(*record, e->nHeight).size(), 1U);
    record->stop();
    BOOST_CHECK_EQUAL(height_keys(e->nHeight).size(), 4U);
}

BOOST_AUTO_TEST_CASE(tip_hash_follows_live_path)
{
    Problems problems;
    std::atomic<int> batches{0};
    Record* raw = nullptr;
    RecordTestHooks hooks;
    hooks.queue_bound = 5000;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.after_write = [&](const BatchInfo& info) {
        const VisibilityRef v = raw->getVisibility();
        if (!info.tip) return;
        ++batches;
        if (v->live_tip != info.tip->nHeight || v->live_tip_hash != info.tip->GetBlockHash()) problems.Add("the tip hash is not the tip block's");
        if (raw->getStatus().live_tip_hash != info.tip->GetBlockHash()) problems.Add("the status has another tip hash");
    };
    std::shared_ptr<Record> record = Start(hooks);
    raw = record.get();
    // Connected blocks with and without rows, a block left out of a full queue, and disconnected blocks.
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    BOOST_CHECK(record->getVisibility()->live_tip_hash == a->GetBlockHash());
    hooks.hold->Close();
    BOOST_REQUIRE(WaitUntil([&] { return hooks.hold->WaitUntilHeld(Ms{10}); }));
    const CBlockIndex* big = Mine({Message({Script(MakeScript(3000, 0x42))})});
    const CBlockIndex* left = Mine({Message({Script(MakeScript(3000, 0x43))})});
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    Catch(*record, left);
    // The second block did not fit in the queue: its height is not covered, the tip is its block.
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(v->IsVisible(big->nHeight));
    BOOST_CHECK(!v->IsVisible(left->nHeight));
    BOOST_CHECK(v->live_tip_hash == left->GetBlockHash());
    Mine();
    Catch(*record);
    Invalidate(left);
    Catch(*record, big);
    BOOST_CHECK(record->getVisibility()->live_tip_hash == big->GetBlockHash());
    Invalidate(a);
    Catch(*record, a->pprev);
    BOOST_CHECK(record->getVisibility()->live_tip_hash == a->pprev->GetBlockHash());
    record->stop();
    BOOST_CHECK(batches > 4);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

BOOST_AUTO_TEST_CASE(removals_counter)
{
    Problems problems;
    std::atomic<int> raised{0}, kept{0}, pending{0}, uncover_only{0};
    Record* raw = nullptr;
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo& info) {
        // Compared with the snapshot readers still see: the one published after the batch before.
        const uint64_t published = raw->getVisibility()->removals;
        const bool must_raise = info.erases || info.uncovers;
        // Raised before the batch reaches the database, by one.
        if (must_raise && (info.removals != published + 1 || raw->getRemovals() != info.removals)) problems.Add("not raised before the batch");
        if (!must_raise && info.removals != published) problems.Add("raised for a batch that removes nothing");
        if (info.kind == BatchInfo::Kind::PENDING && must_raise) problems.Add("filling in coins removed something");
        (must_raise ? raised : kept)++;
        if (info.uncovers && !info.erases) ++uncover_only;
        if (info.kind == BatchInfo::Kind::PENDING) ++pending;
    };
    hooks.after_write = [&](const BatchInfo& info) {
        if (raw->getVisibility()->removals != info.removals) problems.Add("the snapshot does not carry the counter");
    };
    std::shared_ptr<Record> record = Make(hooks);
    raw = record.get();
    record->load();
    record->start();
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Mine({Message({OpReturn("b")})});
    Catch(*record);
    BOOST_CHECK(WaitUntil([&] { return pending >= 2; }));
    // Opening the database counts as a removal: nothing a reader saw before it holds.
    const uint64_t before = record->getRemovals();
    BOOST_CHECK_EQUAL(before, 1U);
    // A disconnection erases rows and uncovers; replacing a covered height erases.
    Invalidate(a);
    Catch(*record, a->pprev);
    BOOST_CHECK(record->getRemovals() > before);
    Mine({Message({OpReturn("c")})});
    Catch(*record);
    // A block without rows disconnected: its batch erases nothing and only uncovers.
    const CBlockIndex* empty = Mine();
    Catch(*record);
    const uint64_t before_empty = record->getRemovals();
    Invalidate(empty);
    Catch(*record, empty->pprev);
    BOOST_CHECK(record->getRemovals() > before_empty);
    // Switched off and on: a snapshot from before does not match the counter afterwards.
    const VisibilityRef old = record->getVisibility();
    record->setRecordEnabled(false);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::OFF; }));
    BOOST_CHECK(record->getRemovals() > old->removals);
    record->setRecordEnabled(true);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING; }));
    BOOST_CHECK(record->getRemovals() > old->removals + 1);
    record->stop();
    // Only counts that do not depend on how the worker joins queued blocks into batches: the two disconnections
    // above may be one batch or two, and the disconnection without rows is one; the first batch and those of the
    // blocks with rows, which are never joined, remove nothing.
    BOOST_CHECK(raised >= 2);
    BOOST_CHECK(uncover_only >= 1);
    BOOST_CHECK(kept >= 3);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

BOOST_AUTO_TEST_CASE(invariant_after_every_batch)
{
    Problems problems;
    std::atomic<int> checked{0};
    Record* raw = nullptr;
    RecordTestHooks hooks;
    hooks.queue_bound = 3000;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.after_write = [&](const BatchInfo& info) {
        CheckHolds(*raw, raw->getVisibility(), info.tip, problems);
        ++checked;
    };
    const auto start = [&]() {
        std::shared_ptr<Record> record = Make(hooks);
        raw = record.get();
        record->load();
        record->start();
        CheckHolds(*record, record->getVisibility(), Tip(), problems);
        return record;
    };
    std::shared_ptr<Record> record = start();
    // Blocks with rows, then a reorganisation of one block and one of three.
    const CBlockIndex* a1 = Mine({Message({OpReturn("a1")})});
    const CBlockIndex* a2 = Mine({Message({OpReturn("a2"), OpReturn("a2b")})});
    Mine({Message({OpReturn("a3")})});
    Catch(*record);
    Invalidate(Tip());
    Mine({Message({OpReturn("b3")})});
    Mine({Message({OpReturn("b4")})});
    Catch(*record);
    Invalidate(a1);
    for (int i = 0; i < 5; ++i) Mine({Message({OpReturn(strprintf("c%d", i))})});
    Catch(*record);
    // Blocks left out of a full queue.
    hooks.hold->Close();
    BOOST_REQUIRE(WaitUntil([&] { return hooks.hold->WaitUntilHeld(Ms{10}); }));
    Mine({Message({Script(MakeScript(2500, 0x44))})});
    Mine({Message({Script(MakeScript(2500, 0x45))})});
    Mine({Message({OpReturn("d")})});
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    Catch(*record);
    record->stop();
    // Restarts: normal, after a reorganisation while stopped, and with the record's tip ahead of the node.
    record = start();
    Catch(*record);
    record->stop();
    Reconsider(a1);
    for (int i = 0; i < 6; ++i) Mine({Message({OpReturn(strprintf("e%d", i))})});
    record = start();
    Mine({Message({OpReturn("f")})});
    Catch(*record);
    record->stop();
    Invalidate(Tip()->pprev);
    record = start();
    Mine({Message({OpReturn("g")})});
    Catch(*record);
    record->stop();
    BOOST_CHECK(a2->nHeight > 0);
    BOOST_CHECK(checked > 15);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

//! Bytes written to a value as they are, for values the record would refuse to write.
struct RawValue {
    std::vector<unsigned char> bytes;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (!bytes.empty()) s.write((const char*)bytes.data(), bytes.size());
    }
};

BOOST_AUTO_TEST_CASE(startup_cases)
{
    const std::thread::id test_thread = std::this_thread::get_id();
    std::atomic<int> writes_here{0};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo&) {
        if (std::this_thread::get_id() == test_thread) ++writes_here;
    };
    const auto covered = [](Record& record) { return *record.getVisibility()->coverage; };

    // A new record: nothing is covered, every height up to the node's tip is still to be read.
    std::shared_ptr<Record> record = Start(hooks);
    BOOST_CHECK(covered(*record).empty());
    BOOST_CHECK(*record->getVisibility()->gaps == std::vector<GapRange>({{0, 50, GapReason::PENDING}}));
    for (int i = 0; i < 3; ++i) Mine({Message({OpReturn(strprintf("a%d", i))})});
    Catch(*record);
    record->stop();

    // Case A: the record's tip is on the active chain. Nothing is cut; what connected meanwhile is a hole.
    Mine();
    Mine({Message({OpReturn("b")})});
    record = Start(hooks);
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, 53}}));
    BOOST_CHECK(*record->getVisibility()->gaps == std::vector<GapRange>({{0, 50, GapReason::PENDING}, {54, 55, GapReason::PENDING}}));
    BOOST_CHECK_EQUAL(record->getVisibility()->live_tip, 55);
    const CBlockIndex* c = Mine({Message({OpReturn("c")})});
    Catch(*record);
    record->stop();

    // Case B: the node is behind the record's tip, on its branch. Nothing is cut; what lies above the node's tip
    // is invisible until its block connects again.
    Invalidate(c->pprev);
    BOOST_REQUIRE(Tip() == c->pprev->pprev);
    record = Start(hooks);
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, 53}, {56, 56}}));
    BOOST_CHECK(!record->getVisibility()->IsVisible(56));
    BOOST_CHECK(RowsAt(*record, 56).empty());
    Reconsider(c->pprev);
    Catch(*record, c);
    BOOST_CHECK_EQUAL(RowsAt(*record, 56).size(), 1U);
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, 53}, {55, 56}}));
    record->stop();

    // Case C: the chain reorganised while the record was stopped: heights above the fork leave coverage.
    const CBlockIndex* fork = WITH_LOCK(cs_main, return ::ChainActive()[52]);
    Invalidate(WITH_LOCK(cs_main, return ::ChainActive()[53]));
    for (int i = 0; i < 5; ++i) Mine({Message({OpReturn(strprintf("d%d", i))})});
    record = Start(hooks);
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, fork->nHeight}}));
    BOOST_CHECK_EQUAL(record->getVisibility()->live_tip, 57);
    record->stop();
    {
        // The cut was written by the worker's first batch.
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        StoredCoverage stored;
        BOOST_REQUIRE(db.Read(uint8_t{'C'}, stored));
        BOOST_CHECK(stored.ranges == std::vector<HeightRange>({{51, 52}}));
    }

    // Case D: no block of the record's locator is known (the block index was built again): nothing is covered.
    CBlockLocator saved;
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Read(uint8_t{'T'}, saved));
        BOOST_REQUIRE(db.Write(uint8_t{'T'}, CBlockLocator({InsecureRand256(), InsecureRand256()})));
    }
    record = Start(hooks);
    BOOST_CHECK(covered(*record).empty());
    record->stop();

    // A covered range above the record's tip (damage) is cut there.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(uint8_t{'T'}, saved));
        StoredCoverage stored;
        stored.ranges = {{51, 52}, {100, 120}};
        BOOST_REQUIRE(db.Write(uint8_t{'C'}, stored));
    }
    record = Start(hooks);
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, 52}}));
    record->stop();

    // The active chain is empty (as under -reindex-chainstate): nothing is cut and nothing is visible.
    const CBlockIndex* tip = Tip();
    {
        LOCK(cs_main);
        ::ChainActive().SetTip(nullptr);
    }
    record = Make(hooks);
    record->load();
    BOOST_CHECK(covered(*record) == std::vector<HeightRange>({{51, 52}}));
    BOOST_CHECK_EQUAL(record->getVisibility()->live_tip, -1);
    BOOST_CHECK(!record->getVisibility()->IsVisible(51));
    BOOST_CHECK(record->getVisibility()->gaps->empty());
    record->stop();
    // The active chain is empty and no block of the locator is known (as under -reindex): nothing is covered.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(uint8_t{'T'}, CBlockLocator({InsecureRand256()})));
    }
    record = Make(hooks);
    record->load();
    BOOST_CHECK(covered(*record).empty());
    BOOST_CHECK_EQUAL(record->getVisibility()->live_tip, -1);
    BOOST_CHECK(record->getVisibility()->gaps->empty());
    record->stop();
    // The active chain is empty, and the record's tip block is not known while an older block of its locator is (a
    // start in the middle of -reindex): as in case C, nothing above the fork stays covered, and the empty chain has
    // no fork.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(uint8_t{'T'}, CBlockLocator({InsecureRand256(), Params().GenesisBlock().GetHash()})));
        StoredCoverage stored;
        stored.ranges = {{51, 52}};
        BOOST_REQUIRE(db.Write(uint8_t{'C'}, stored));
    }
    record = Make(hooks);
    record->load();
    BOOST_CHECK(covered(*record).empty());
    BOOST_CHECK_EQUAL(record->getVisibility()->live_tip, -1);
    record->stop();
    {
        LOCK(cs_main);
        ::ChainActive().SetTip(const_cast<CBlockIndex*>(tip));
    }

    // load() writes nothing itself: every batch is the worker's.
    BOOST_CHECK_EQUAL(writes_here, 0);
}

BOOST_AUTO_TEST_CASE(tip_move_rule_on_start)
{
    Problems problems;
    Record* raw = nullptr;
    RecordTestHooks hooks;
    hooks.after_write = [&](const BatchInfo& info) { CheckHolds(*raw, raw->getVisibility(), info.tip, problems); };
    std::shared_ptr<Record> record = Make(hooks);
    raw = record.get();
    record->load();
    const CBlockIndex* b1 = Mine({Message({OpReturn("b1")})});
    const CBlockIndex* b2 = Mine({Message({OpReturn("b2")})});
    Catch(*record);
    record->stop();

    // While no record runs, the node moves to a branch C; its first block's callback is delivered to no one.
    Invalidate(b1);
    const CBlockIndex* c1 = Process(MakeBlock(Tip(), {Message({OpReturn("c1")})}));
    BOOST_REQUIRE(Tip() == c1);
    SyncWithValidationInterfaceQueue();
    {
        // The callbacks of the next changes wait: C's second block, then back to B, one block longer.
        QueueHold hold;
        const CBlockIndex* c2 = Process(MakeBlock(c1, {Message({OpReturn("c2")})}));
        BOOST_REQUIRE(Tip() == c2);
        {
            LOCK(cs_main);
            ResetBlockFailureFlags(const_cast<CBlockIndex*>(b1));
        }
        const CBlockIndex* b3 = Process(MakeBlock(b2, {Message({OpReturn("b3")})}));
        BOOST_REQUIRE(Tip() == b3);
        // The record starts on B's tip: its coverage of B's blocks stands. The first callback it then gets names
        // C's second block, whose parent is not the record's tip.
        record = Make(hooks);
        raw = record.get();
        record->load();
        record->start();
        BOOST_CHECK(*record->getVisibility()->coverage == std::vector<HeightRange>({{b1->nHeight, b2->nHeight}}));
        CheckHolds(*record, record->getVisibility(), b3, problems);
        hold.Release();
        Catch(*record, b3);
    }
    BOOST_CHECK_EQUAL(RowsAt(*record, b1->nHeight).size(), 1U);
    BOOST_CHECK(RowsAt(*record, b1->nHeight).at(0).tx.txid == WITH_LOCK(m_blocks_mutex, return m_blocks.at(b1->GetBlockHash()))->vtx[1]->GetHash());
    record->stop();
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

BOOST_AUTO_TEST_CASE(tip_locator_every_batch)
{
    // Every batch of connected or disconnected blocks, with rows or without, writes the tip locator; a crash before
    // or after any batch, or a stop, leaves no covered height above it, and the next start holds the active chain's
    // rows wherever it covers.
    // A main branch M, whose last two blocks have no rows, and a side branch S from M's first block, whose last
    // block has none; the record is given their callbacks itself.
    std::vector<const CBlockIndex*> m;
    for (int i = 0; i < 4; ++i) m.push_back(Mine({Message({OpReturn(strprintf("m%d", i)), Script(MakeScript(4500 + i, 0x30 + i))})}));
    for (int i = 0; i < 2; ++i) m.push_back(Mine());
    std::vector<const CBlockIndex*> side;
    const CBlockIndex* prev = m[0];
    for (int i = 0; i < 3; ++i) {
        std::vector<CMutableTransaction> txs;
        if (i < 2) txs.push_back(Message({OpReturn(strprintf("s%d", i))}));
        side.push_back(Process(MakeBlock(prev, txs)));
        prev = side.back();
    }
    BOOST_REQUIRE(Tip() == m.back());
    const auto block_of = [&](const CBlockIndex* index) { return WITH_LOCK(m_blocks_mutex, return m_blocks.at(index->GetBlockHash())); };
    struct Step {
        bool connect;
        const CBlockIndex* index;
    };
    const std::vector<Step> steps{{true, m[0]}, {true, m[1]}, {true, m[2]}, {false, m[2]}, {false, m[1]}, {true, side[0]},
                                  {true, side[1]}, {true, side[2]}, {false, side[2]}, {false, side[1]}, {false, side[0]},
                                  {true, m[1]}, {true, m[2]}, {true, m[3]}, {true, m[4]}, {true, m[5]}};
    // Runs the steps on a new record until batch `at`, where it ends: crashed before or after the write, or stopped.
    enum class End { CRASH_BEFORE, CRASH_AFTER, STOP };
    int runs = 0;
    for (int at = 1; at < 24; ++at) {
        for (const End end : {End::CRASH_BEFORE, End::CRASH_AFTER, End::STOP}) {
            const fs::path path = GetDataDir() / strprintf("crash-%d-%d", at, int(end));
            std::atomic<int> batches{0};
            std::atomic<bool> ended{false};
            // The tip of the last batch written that writes the tip locator: every batch of connected or
            // disconnected blocks, with or without rows, and the last batch of a stop.
            std::atomic<const CBlockIndex*> locator_tip{nullptr};
            std::atomic<bool> locator_written{false};
            RecordTestHooks hooks;
            hooks.before_write = [&](const BatchInfo&) {
                if (ended) return;
                if (++batches == at && end == End::CRASH_BEFORE) {
                    ended = true;
                    throw TestCrash();
                }
            };
            hooks.after_write = [&](const BatchInfo& info) {
                if (info.kind == BatchInfo::Kind::ITEMS || info.kind == BatchInfo::Kind::LAST) {
                    locator_tip = info.tip;
                    locator_written = true;
                }
                if (!ended && batches == at && end != End::STOP) {
                    ended = true;
                    throw TestCrash();
                }
                if (batches == at) ended = true;
            };
            auto record = std::make_shared<Record>(path, RecordOptions(), hooks);
            record->load();
            for (const Step& step : steps) {
                const std::shared_ptr<const CBlock> block = block_of(step.index);
                if (step.connect) {
                    record->BlockConnected(block, step.index);
                } else {
                    record->BlockDisconnected(block, step.index);
                }
            }
            if (!WaitUntil([&] { return ended.load() || record->getVisibility()->live_tip_hash == m.back()->GetBlockHash(); })) {
                BOOST_ERROR("the record neither ended nor finished");
            }
            record->stop();
            if (!ended) continue; // fewer batches than `at`
            ++runs;
            // What a crash or a stop leaves: the tip locator that batch wrote, and no covered height above its first
            // block.
            {
                CDBWrapper db(path, 1 << 20, false, false, true);
                StoredCoverage stored;
                CBlockLocator locator;
                const bool have_locator = db.Read(uint8_t{'T'}, locator);
                BOOST_CHECK_EQUAL(have_locator, locator_written.load());
                if (have_locator && locator_written) {
                    const CBlockIndex* tip = locator_tip.load();
                    BOOST_CHECK(!locator.vHave.empty() && tip && locator.vHave[0] == tip->GetBlockHash());
                }
                if (db.Read(uint8_t{'C'}, stored) && !stored.ranges.empty()) {
                    BOOST_REQUIRE(have_locator);
                    const CBlockIndex* t = WITH_LOCK(cs_main, return LookupBlockIndex(locator.vHave.at(0)));
                    BOOST_REQUIRE(t);
                    BOOST_CHECK(stored.ranges.back().hi <= t->nHeight);
                }
            }
            // The next start holds exactly the active chain's rows wherever it covers.
            Problems problems;
            auto again = std::make_shared<Record>(path, RecordOptions());
            again->load();
            CheckHolds(*again, again->getVisibility(), Tip(), problems);
            again->stop();
            for (const std::string& problem : problems.List()) BOOST_ERROR(strprintf("batch %d, end %d: %s", at, int(end), problem));
        }
    }
    BOOST_CHECK(runs >= 30);
}

BOOST_AUTO_TEST_CASE(shared_registration)
{
    std::atomic<bool> armed{false};
    std::promise<void> inside;
    std::promise<void> go;
    std::shared_future<void> released = go.get_future().share();
    RecordTestHooks hooks;
    hooks.in_block_connected = [&] {
        if (!armed.exchange(false)) return;
        inside.set_value();
        released.wait();
    };
    std::shared_ptr<Record> record = Start(hooks);
    std::weak_ptr<Record> weak = record;
    armed = true;
    Mine({Message({OpReturn("queued")})});
    inside.get_future().wait();
    // Switched off while one of its callbacks runs on the scheduler thread.
    record->setRecordEnabled(false);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::OFF; }));
    record.reset();
    // The callback still runs, so the record is still there.
    BOOST_CHECK(!weak.expired());
    {
        // The record goes when the callback returns, and its destructor takes no node lock: it ends while this
        // thread holds cs_main.
        LOCK(cs_main);
        go.set_value();
        BOOST_CHECK(WaitUntil([&] { return weak.expired(); }, Ms{10000}));
    }
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK(weak.expired());
}

BOOST_AUTO_TEST_CASE(released_without_stop)
{
    // Owners that let go of a record without stopping it: its worker, which holds the record while it runs, finds
    // itself alone and ends, writing nothing more, and the record goes with it. Its registration for the callbacks
    // does not keep it, also when its worker fails meanwhile.
    std::atomic<bool> fail{false};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo&) {
        if (fail) throw dbwrapper_error("a write that fails");
    };
    std::shared_ptr<Record> record = Start(hooks);
    Mine({Message({OpReturn("kept")})});
    Catch(*record);
    SyncWithValidationInterfaceQueue();
    std::weak_ptr<Record> weak = record;
    record.reset();
    fail = true;
    const CBlockIndex* after = Mine({Message({OpReturn("after")})});
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK(WaitUntil([&] { return weak.expired(); }, Ms{10000}));
    BOOST_REQUIRE(WaitUntilClosed(m_path));
    // What it wrote stays, and the block after it is a hole.
    record = Start();
    BOOST_CHECK_EQUAL(AllRows(*record, false).size(), 1U);
    BOOST_CHECK(!record->getVisibility()->IsVisible(after->nHeight));
    record->stop();
}

BOOST_AUTO_TEST_CASE(node_state_snapshot)
{
    std::shared_ptr<Record> record = Make();
    record->load();
    BOOST_CHECK(!record->getStatus().node_known);
    record->start();
    RecordStatus status = record->getStatus();
    BOOST_CHECK(status.node_known);
    BOOST_CHECK_EQUAL(status.node_tip, 50);
    BOOST_CHECK(status.node_tip_hash == Tip()->GetBlockHash());
    BOOST_CHECK_EQUAL(status.node_tip_time, Tip()->GetBlockTime());
    BOOST_CHECK_EQUAL(status.header_tip, 50);
    BOOST_CHECK(!status.initial_download);
    BOOST_CHECK(status.state == State::BACKFILLING);
    BOOST_CHECK(status.explicit_options == EXPLICIT_RECORD);
    BOOST_CHECK(!status.pruned);

    // From the notifications: a block, and a header whose block the node does not have yet.
    const CBlockIndex* mined = Mine();
    BOOST_CHECK_EQUAL(record->getStatus().node_tip, mined->nHeight);
    const std::shared_ptr<CBlock> next = MakeBlock(mined, {});
    BlockValidationState state;
    BOOST_REQUIRE(m_node.chainman->ProcessNewBlockHeaders({next->GetBlockHeader()}, state, Params()));
    status = record->getStatus();
    BOOST_CHECK_EQUAL(status.header_tip, mined->nHeight + 1);
    BOOST_CHECK_EQUAL(status.node_tip, mined->nHeight);

    // It does not wait for cs_main: it returns while another thread holds it, before that thread lets go.
    std::promise<void> held;
    std::promise<void> done;
    std::atomic<bool> released{false};
    std::thread holder([&] {
        LOCK(cs_main);
        held.set_value();
        done.get_future().wait_for(std::chrono::seconds{20});
        released = true;
    });
    held.get_future().wait();
    status = record->getStatus();
    BOOST_CHECK(!released);
    done.set_value();
    holder.join();
    BOOST_CHECK_EQUAL(status.node_tip, mined->nHeight);
    record->stop();
}

BOOST_AUTO_TEST_CASE(paid_by_sources)
{
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    const auto settled = [&](const CBlockIndex* index, uint32_t n = 0) {
        return WaitUntil([&] {
            RowView row;
            return record->readRow({index->nHeight, n}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
        });
    };
    const auto first_tx = [&](const CBlockIndex* index) {
        TxView view;
        BOOST_REQUIRE(record->readTransaction({index->nHeight, 1}, SCRIPT_HEAD_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
        return view;
    };

    // From the block's undo data: the coin, what paid, the fee and the outputs to scripts the transaction does not
    // spend.
    const TestCoin coin = Take();
    const CScript other = CScript() << OP_TRUE;
    const CMutableTransaction paid = Spend({coin}, {CTxOut(0, OpReturn("paid")), CTxOut(7000, other), CTxOut(coin.value - 12000, m_script)});
    const CBlockIndex* a = Mine({paid}, {CTxOut(0, OpReturn("from the coinbase"))});
    Catch(*record);
    BOOST_REQUIRE(settled(a, 1));
    RowView row;
    BOOST_REQUIRE(record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::COINBASE);
    BOOST_REQUIRE(record->readRow({a->nHeight, 1}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::SCRIPT);
    BOOST_CHECK(row.tx.paid_by == m_script);
    BOOST_CHECK_EQUAL(row.tx.fee, 5000);
    BOOST_CHECK_EQUAL(row.tx.n_other, 1);
    BOOST_CHECK_EQUAL(row.tx.other_value, 7000);
    BOOST_CHECK(!(row.tx.tx_flags & TX_SPENT_PENDING));
    TxView view = first_tx(a);
    BOOST_CHECK(view.vin[0].spent_state == SpentState::KNOWN);
    BOOST_CHECK_EQUAL(view.vin[0].spent.out.nValue, coin.value);
    BOOST_CHECK(view.vin[0].spent.out.scriptPubKey == m_script);
    BOOST_CHECK(view.vin[0].spent.fCoinBase);
    BOOST_CHECK_EQUAL(view.fee, 5000);

    // Undo data that cannot be read: the coins are not available.
    hooks.hold->Close();
    BOOST_REQUIRE(WaitUntil([&] { return hooks.hold->WaitUntilHeld(Ms{10}); }));
    const CBlockIndex* b = Mine({Message({OpReturn("unreadable")})});
    {
        LOCK(cs_main);
        BOOST_REQUIRE(b->nStatus & BLOCK_HAVE_UNDO);
        std::string path = GetBlockPosFilename(FlatFilePos(b->nFile, 0)).string();
        path.replace(path.rfind("blk"), 3, "rev");
        std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
        BOOST_REQUIRE(file.good());
        file.seekp(b->nUndoPos);
        const std::string junk(16, '\xee');
        file.write(junk.data(), junk.size());
    }
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    Catch(*record, b);
    BOOST_REQUIRE(settled(b));
    BOOST_REQUIRE(record->readRow({b->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::UNAVAILABLE);
    BOOST_CHECK(row.tx.paid_by.empty());
    BOOST_CHECK_EQUAL(row.tx.fee, -1);
    BOOST_CHECK_EQUAL(row.tx.n_other, N_OTHER_UNKNOWN);
    view = first_tx(b);
    BOOST_CHECK(view.vin[0].spent_state == SpentState::UNAVAILABLE);

    // Undo data deleted by pruning before the coins were looked up: not available either.
    hooks.hold->Close();
    BOOST_REQUIRE(WaitUntil([&] { return hooks.hold->WaitUntilHeld(Ms{10}); }));
    const CBlockIndex* c = Mine({Message({OpReturn("pruned")})});
    {
        LOCK(cs_main);
        ::g_chainman.m_blockman.PruneOneBlockFile(c->nFile);
        BOOST_REQUIRE(!(c->nStatus & BLOCK_HAVE_UNDO));
    }
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    Catch(*record, c);
    BOOST_REQUIRE(settled(c));
    BOOST_REQUIRE(record->readRow({c->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::UNAVAILABLE);
    record->stop();
}

BOOST_AUTO_TEST_CASE(damaged_and_foreign)
{
    std::shared_ptr<Record> record = Start();
    Mine({Message({OpReturn("kept")})});
    Catch(*record);
    record->stop();
    const auto fails_with = [&](FailReason reason) {
        LogLines lines;
        std::shared_ptr<Record> failed = Start();
        const RecordStatus status = failed->getStatus();
        BOOST_CHECK(status.state == State::FAILED);
        BOOST_CHECK(status.reason == reason);
        BOOST_CHECK_EQUAL(lines.Containing(strprintf("%s state FAILED", LOG_PREFIX)).size(), 1U);
        BOOST_CHECK(AllRows(*failed, false).empty());
        BOOST_CHECK(!failed->waitForTip(100));
        // The remedy, a rebuild: a new record at the node's tip.
        failed->requestRebuild();
        BOOST_CHECK(WaitUntil([&] { return failed->getStatus().state == State::BACKFILLING; }));
        const CBlockIndex* index = Mine({Message({OpReturn("new")})});
        Catch(*failed, index);
        BOOST_CHECK_EQUAL(AllRows(*failed, false).size(), 1U);
        failed->stop();
    };
    // Made by a newer version.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(uint8_t{'V'}, RawValue{{2, 0, 0, 0, 7, 7, 7}}));
    }
    fails_with(FailReason::NEWER_FORMAT);
    // Of another chain.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        RecordMeta meta;
        meta.genesis_hash = uint256::ONE;
        BOOST_REQUIRE(db.Write(uint8_t{'V'}, meta));
    }
    fails_with(FailReason::OTHER_CHAIN);
    // Damaged: the end key gone, a value that cannot be read, the record's own facts gone, the database's files.
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(uint8_t{'z'}));
    }
    fails_with(FailReason::OPEN_FAILED);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(uint8_t{'C'}, RawValue{{0xff, 0xff}}));
    }
    fails_with(FailReason::OPEN_FAILED);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(uint8_t{'V'}));
    }
    fails_with(FailReason::OPEN_FAILED);
    {
        std::ofstream current((m_path / "CURRENT").string(), std::ios::trunc);
        current << "MANIFEST-999999\n";
    }
    fails_with(FailReason::OPEN_FAILED);
}

BOOST_AUTO_TEST_CASE(rebuild_and_reindex)
{
    std::shared_ptr<Record> record = Start();
    Mine({Message({OpReturn("one")})});
    Mine({Message({OpReturn("two")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(record->getStatus().rows, 2U);
    // A rebuild while running: a new record, live at the node's tip, holding nothing below it.
    record->requestRebuild();
    BOOST_REQUIRE(WaitUntil([&] {
        const RecordStatus status = record->getStatus();
        return status.state == State::BACKFILLING && status.rows == 0 && status.coverage && status.coverage->empty() && status.live_tip == 52;
    }));
    const CBlockIndex* three = Mine({Message({OpReturn("three")})});
    Catch(*record);
    BOOST_CHECK(*record->getVisibility()->coverage == std::vector<HeightRange>({{three->nHeight, three->nHeight}}));
    BOOST_CHECK_EQUAL(AllRows(*record, false).size(), 1U);
    record->stop();
    // Rebuilt at start (the reindex option): nothing of the old record is left.
    record = Start({}, /*reindex=*/true);
    BOOST_CHECK(record->getVisibility()->coverage->empty());
    BOOST_CHECK_EQUAL(record->getStatus().rows, 0U);
    record->stop();
    BOOST_CHECK_EQUAL(CountPrefix(Keys(m_path), 'r'), 0U);
}

BOOST_AUTO_TEST_CASE(live_switch)
{
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    record->setRecordEnabled(false);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::OFF; }));
    BOOST_CHECK(AllRows(*record, false).empty());
    BOOST_CHECK(!record->waitForTip(100));
    const CBlockIndex* b = Mine({Message({OpReturn("b")})});
    // Switched on again: the files were kept, and what connected while off is a hole.
    record->setRecordEnabled(true);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING && record->getVisibility()->live_tip == b->nHeight; }));
    BOOST_CHECK(*record->getVisibility()->coverage == std::vector<HeightRange>({{a->nHeight, a->nHeight}}));
    BOOST_CHECK_EQUAL(RowsAt(*record, a->nHeight).size(), 1U);
    const CBlockIndex* c = Mine({Message({OpReturn("c")})});
    Catch(*record, c);
    BOOST_CHECK(!record->getVisibility()->IsVisible(b->nHeight));
    BOOST_CHECK(record->getVisibility()->IsVisible(c->nHeight));
    record->stop();
}

BOOST_AUTO_TEST_CASE(flood_queue_bound)
{
    // With a bound no block with rows fits in, every such block becomes a hole; blocks without rows still go in.
    RecordTestHooks hooks;
    hooks.queue_bound = 1;
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    const CBlockIndex* b = Mine();
    const CBlockIndex* c = Mine({Message({OpReturn("c")})});
    Catch(*record, c);
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(!v->IsVisible(a->nHeight));
    BOOST_CHECK(v->IsVisible(b->nHeight));
    BOOST_CHECK(!v->IsVisible(c->nHeight));
    BOOST_CHECK(*v->gaps == std::vector<GapRange>({{0, a->nHeight, GapReason::PENDING}, {c->nHeight, c->nHeight, GapReason::PENDING}}));
    BOOST_CHECK_EQUAL(record->getStatus().rows, 0U);
    record->stop();

    // A block left out at a height the record still covered from before, with another block's rows: the height
    // leaves coverage, so that those rows are not shown for the new block.
    std::shared_ptr<Record> full = Start();
    const CBlockIndex* x1 = Mine({Message({OpReturn("x1")})});
    const CBlockIndex* x2 = Mine({Message({OpReturn("x2")})});
    Catch(*full);
    full->stop();
    Invalidate(x2);
    record = Start(hooks);
    BOOST_CHECK(record->getVisibility()->coverage->back() == HeightRange({x1->nHeight, x2->nHeight}));
    const CBlockIndex* y2 = Mine({Message({OpReturn("y2")})});
    Catch(*record, y2);
    BOOST_CHECK(!record->getVisibility()->IsVisible(y2->nHeight));
    BOOST_CHECK(RowsAt(*record, y2->nHeight).empty());
    record->stop();
}

BOOST_AUTO_TEST_CASE(shutdown_order)
{
    std::atomic<bool> slow{false};
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.before_write = [&](const BatchInfo&) {
        if (slow) std::this_thread::sleep_for(Ms{300});
    };
    std::shared_ptr<Record> record = Start(hooks);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    for (int i = 0; i < 30; ++i) Mine({Message({OpReturn(strprintf("queued %d", i))})});
    SyncWithValidationInterfaceQueue();
    slow = true;
    // flush() returns within its second although the worker does not get to a step's end.
    auto begun = Clock::now();
    record->flush();
    BOOST_CHECK(Clock::now() - begun < Ms{5000}); // FLUSH_WAIT, and what a loaded machine adds
    // stop() writes what is queued until its deadline, then a last batch, and returns.
    begun = Clock::now();
    record->stop();
    const auto took = Clock::now() - begun;
    BOOST_CHECK(took >= Ms{4500});
    BOOST_CHECK(took < Ms{10000}); // STOP_DEADLINE, the batch in progress and the last one, and what load adds
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    BOOST_CHECK(AllRows(*record, false).empty());
    // The blocks it did not write are holes at the next start.
    record = Start();
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK_EQUAL(v->live_tip, 80);
    uint64_t covered = 0;
    for (const HeightRange& range : *v->coverage) covered += uint64_t(range.hi - range.lo + 1);
    BOOST_CHECK(covered > 0);
    BOOST_CHECK(covered < 30);
    record->stop();
}

BOOST_AUTO_TEST_CASE(values_obfuscated)
{
    const std::string marker = "zq-the-marker-of-values-obfuscated-zq";
    std::shared_ptr<Record> record = Start();
    std::vector<unsigned char> long_script = MakeScript(6000, 0x20);
    std::copy(marker.begin(), marker.end(), long_script.begin() + 5000);
    const CBlockIndex* index = Mine({Message({OpReturn(marker), Script(long_script)})});
    Catch(*record);
    record->stop();
    BOOST_CHECK(FilesUnder(m_path).find(marker) == std::string::npos);
    // The search finds the marker where it is not hidden: in the node's block file.
    BOOST_CHECK(FilesUnder(GetBlockPosFilename(FlatFilePos(index->nFile, 0)).parent_path()).find(marker) != std::string::npos);
}

BOOST_AUTO_TEST_CASE(coverage_limit)
{
    // Blocks given to the record itself: one without rows, then one with rows that the queue leaves out, and so
    // on, so that each block without rows is a covered range of its own.
    RecordTestHooks hooks;
    hooks.queue_bound = 0;
    LogLines lines;
    std::shared_ptr<Record> record = Start(hooks);
    const std::shared_ptr<const CBlock> without = LooseBlock({CTxOut(1, CScript() << OP_TRUE)});
    const std::shared_ptr<const CBlock> with = LooseBlock({CTxOut(0, OpReturn("left out"))});
    const CBlockIndex* prev = Tip();
    const size_t blocks = 2 * MAX_COVERAGE_RANGES + 6;
    for (size_t i = 0; i < blocks; ++i) {
        const CBlockIndex* index = Fake(prev, ArithToUint256(arith_uint256(i + 1) << 128));
        record->BlockConnected(i % 2 == 0 ? without : with, index);
        prev = index;
    }
    BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip == prev->nHeight; }, Ms{120000}));
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    BOOST_CHECK_EQUAL(v->coverage->size(), MAX_COVERAGE_RANGES);
    // The ranges that left were the oldest of the smallest; the newest blocks are covered.
    BOOST_CHECK(v->IsVisible(prev->nHeight - 1));
    BOOST_CHECK(!v->IsVisible(51));
    BOOST_CHECK_EQUAL(v->coverage->front().lo, 51 + 2 * 3);
    BOOST_CHECK(!lines.Containing(strprintf("%s the list of covered ranges is full at 10000 ranges", LOG_PREFIX)).empty());
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        StoredCoverage stored;
        BOOST_REQUIRE(db.Read(uint8_t{'C'}, stored));
        BOOST_CHECK_EQUAL(stored.ranges.size(), MAX_COVERAGE_RANGES);
    }
}

BOOST_AUTO_TEST_CASE(readers_never_ahead)
{
    // Just before each batch is written, what readers see still describes the database: the snapshot of the batch
    // before, whose rows are all there.
    Problems problems;
    Record* raw = nullptr;
    const CBlockIndex* published_tip = nullptr;
    std::atomic<int> checked{0};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo&) {
        CheckHolds(*raw, raw->getVisibility(), published_tip, problems);
        ++checked;
    };
    hooks.after_write = [&](const BatchInfo& info) { published_tip = info.tip; };
    std::shared_ptr<Record> record = Make(hooks);
    raw = record.get();
    published_tip = Tip();
    record->load();
    record->start();
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    for (int i = 0; i < 3; ++i) Mine({Message({OpReturn(strprintf("b%d", i))})});
    Catch(*record);
    Invalidate(a);
    for (int i = 0; i < 2; ++i) Mine({Message({OpReturn(strprintf("c%d", i))})});
    Catch(*record);
    record->stop();
    BOOST_CHECK(checked > 8);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

BOOST_AUTO_TEST_CASE(log_lines_bounded)
{
    LogLines lines;
    std::shared_ptr<Record> record = Start();
    size_t mined = 0;
    for (const opreturn::test::HostileSample& sample : opreturn::test::HostileCorpus()) {
        Mine({Message({Script(OneDataPush(sample.bytes))})});
        ++mined;
        if (mined == 8) break;
    }
    Catch(*record);
    record->stop();
    const std::string prefix = std::string(LOG_PREFIX) + " block ";
    const std::vector<std::string> blocks = lines.Containing(prefix);
    BOOST_CHECK(blocks.size() >= mined);
    for (const std::string& line : blocks) {
        // Only the height, the number of rows and the microseconds.
        std::string rest = line.substr(line.find(prefix) + prefix.size());
        size_t at = 0;
        const auto number = [&]() {
            const size_t from = at;
            while (at < rest.size() && rest[at] >= '0' && rest[at] <= '9') ++at;
            return at > from;
        };
        const auto literal = [&](const std::string& text) {
            if (rest.compare(at, text.size(), text) != 0) return false;
            at += text.size();
            return true;
        };
        BOOST_CHECK_MESSAGE(number() && literal(", ") && number() && literal(" rows, ") && number() && literal(" us\n") && at == rest.size(), line);
    }
    for (const std::string& line : lines.Lines()) BOOST_CHECK(line.find(opreturn::test::HOSTILE_MARKER) == std::string::npos);
}

BOOST_AUTO_TEST_CASE(failures_stop_only_record)
{
    // A write that fails: the record fails alone, keeps what it had written readable, and the node goes on.
    {
        std::atomic<bool> fail{false};
        RecordTestHooks hooks;
        hooks.before_write = [&](const BatchInfo&) {
            if (fail) throw dbwrapper_error("injected");
        };
        std::shared_ptr<Record> record = Start(hooks);
        const CBlockIndex* a = Mine({Message({OpReturn("a")})});
        Catch(*record);
        fail = true;
        const CBlockIndex* b = Mine({Message({OpReturn("b")})});
        BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
        BOOST_CHECK(record->getStatus().reason == FailReason::WRITE_FAILED);
        const CBlockIndex* c = Mine({Message({OpReturn("c")})});
        BOOST_CHECK(Tip() == c);
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK_EQUAL(RowsAt(*record, a->nHeight).size(), 1U);
        BOOST_CHECK(!record->getVisibility()->IsVisible(b->nHeight));
        BOOST_CHECK(!record->waitForTip(100));
        record->stop();
        BOOST_CHECK(record->getStatus().state == State::STOPPED);
    }
    // A read of the worker's that fails.
    {
        std::atomic<bool> fail{false};
        RecordTestHooks hooks;
        hooks.on_read = [&](uint8_t, bool worker) {
            if (fail && worker) throw dbwrapper_error("injected");
        };
        auto record = std::make_shared<Record>(GetDataDir() / "read", RecordOptions(), hooks);
        record->load();
        record->start();
        fail = true;
        Mine({Message({OpReturn("d")})});
        BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
        BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
        Mine();
        record->stop();
    }
}

BOOST_AUTO_TEST_CASE(format_bounds)
{
    std::shared_ptr<Record> record = Start();
    // At each bound, in a block of the chain: a scriptSig of 10,000 bytes, outputs that are not rows of 10,000
    // and 10,001 bytes, and rows of 4,096 and 4,097 bytes.
    const TestCoin coin = Take();
    CMutableTransaction tx;
    tx.vin.emplace_back(coin.outpoint);
    tx.vout = {CTxOut(0, Script(MakeScript(4096, 0x61))), CTxOut(0, Script(MakeScript(4097, 0x62))),
               CTxOut(1, Script(std::vector<unsigned char>(10000, 0x51))), CTxOut(1, Script(std::vector<unsigned char>(10001, 0x52))),
               CTxOut(coin.value - 10000, m_script)};
    const uint256 hash = SignatureHash(m_script, tx, 0, SIGHASH_ALL, coin.value, SigVersion::BASE);
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(coinbaseKey.Sign(hash, signature));
    signature.push_back(uint8_t(SIGHASH_ALL));
    tx.vin[0].scriptSig = ScriptSigOfSize(MAX_SCRIPT_SIZE, signature);
    const CBlockIndex* index = Mine({tx});
    Catch(*record);
    TxView view;
    BOOST_REQUIRE(record->readTransaction({index->nHeight, 1}, MAX_OUTPUT_SCRIPT_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
    BOOST_CHECK_EQUAL(view.vin[0].script_sig.size(), MAX_SCRIPT_SIZE);
    CMutableTransaction rebuilt;
    BOOST_REQUIRE(view.ToMutableTransaction(rebuilt));
    BOOST_CHECK(rebuilt.GetHash() == tx.GetHash());
    BOOST_CHECK_EQUAL(view.vout[3].script_size, 10001U);
    record->stop();

    // One past a bound, in a block given to the record: the format refuses the value, the record fails, and the
    // callback does not throw.
    const auto refused = [&](const std::shared_ptr<const CBlock>& block, bool expect_failure, const std::string& name) {
        auto r = std::make_shared<Record>(GetDataDir() / name, RecordOptions());
        r->load();
        const CBlockIndex* fake = Fake(Tip(), InsecureRand256());
        BOOST_CHECK_NO_THROW(r->BlockConnected(block, fake));
        if (expect_failure) {
            BOOST_CHECK(WaitUntil([&] { return r->getStatus().state == State::FAILED; }));
            BOOST_CHECK(r->getStatus().reason == FailReason::WRITE_FAILED);
        } else {
            BOOST_CHECK(WaitUntil([&] { return r->getVisibility()->live_tip_hash == fake->GetBlockHash(); }));
            BOOST_CHECK(r->getStatus().state != State::FAILED);
        }
        r->stop();
        return r;
    };
    refused(LooseBlock({CTxOut(0, OpReturn("x"))}, Script(std::vector<unsigned char>(MAX_SCRIPT_SIZE + 1, 0x51))), true, "sig");
    refused(LooseBlock({CTxOut(0, Script(MakeScript(MAX_OUTPUT_SCRIPT_SIZE + 1, 0x63)))}), true, "big");
    // The largest output a block can hold is stored and read back whole.
    const std::shared_ptr<const CBlock> largest = LooseBlock({CTxOut(0, Script(MakeScript(MAX_OUTPUT_SCRIPT_SIZE, 0x64)))});
    auto r = std::make_shared<Record>(GetDataDir() / "largest", RecordOptions());
    r->load();
    const CBlockIndex* fake = Fake(Tip(), InsecureRand256());
    r->BlockConnected(largest, fake);
    BOOST_REQUIRE(WaitUntil([&] { return r->getVisibility()->live_tip_hash == fake->GetBlockHash(); }));
    std::vector<unsigned char> bytes;
    BOOST_CHECK(r->readScript({fake->nHeight, 0}, 0, MAX_OUTPUT_SCRIPT_SIZE, bytes, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(CScript(bytes.begin(), bytes.end()) == largest->vtx[1]->vout[0].scriptPubKey);
    r->stop();
}

BOOST_AUTO_TEST_CASE(wait_bounded)
{
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    BOOST_CHECK(record->waitForTip(2000));
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    {
        // The worker is held and the callbacks wait in the queue: the record cannot catch up.
        QueueHold queue;
        Mine({Message({OpReturn("late")})});
        BOOST_CHECK_EQUAL(record->getStatus().node_tip, 51);
        const auto begun = Clock::now();
        BOOST_CHECK(!record->waitForTip(2000));
        const auto took = Clock::now() - begun;
        BOOST_CHECK(took >= Ms{1990});
        BOOST_CHECK(took < Ms{4000}); // the timeout is the bound; a loaded machine may add to it
        // Any timeout is the bound, also none.
        BOOST_CHECK(!record->waitForTip(0));
        BOOST_CHECK(!record->waitForTip(-5));
    }
    hooks.hold->Open();
    Catch(*record);
    BOOST_CHECK(record->waitForTip(2000));
    record->stop();
    BOOST_CHECK(!record->waitForTip(2000));
}

BOOST_AUTO_TEST_CASE(wait_once_per_stall)
{
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    Mine({Message({OpReturn("late")})});
    auto begun = Clock::now();
    BOOST_CHECK(!record->waitForTip(300));
    BOOST_CHECK(Clock::now() - begun >= Ms{290});
    // The stall is noted: the next calls do not wait.
    begun = Clock::now();
    BOOST_CHECK(!record->waitForTip(2000));
    BOOST_CHECK(!record->waitForTip(2000));
    BOOST_CHECK(Clock::now() - begun < Ms{2000}); // each would otherwise have waited 2 s
    // Caught up when a call begins: the stall is over, and a later call waits again.
    hooks.hold->Open();
    Catch(*record);
    BOOST_CHECK(record->waitForTip(2000));
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    Mine({Message({OpReturn("later")})});
    begun = Clock::now();
    BOOST_CHECK(!record->waitForTip(300));
    BOOST_CHECK(Clock::now() - begun >= Ms{290});
    hooks.hold->Open();
    Catch(*record);
    record->stop();
}

BOOST_AUTO_TEST_CASE(wait_after_reorg_and_back)
{
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a1 = Mine({Message({OpReturn("a1")})});
    Catch(*record);
    // The record follows the node from branch A to branch B.
    Invalidate(a1);
    Mine({Message({OpReturn("b1")})});
    const CBlockIndex* b2 = Mine({Message({OpReturn("b2")})});
    Catch(*record, b2);
    // The node goes back to A, now longer, while the worker is held.
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    {
        LOCK(cs_main);
        ResetBlockFailureFlags(const_cast<CBlockIndex*>(a1));
    }
    const CBlockIndex* a2 = Process(MakeBlock(a1, {}));
    const CBlockIndex* a3 = Process(MakeBlock(a2, {}));
    BOOST_REQUIRE(Tip() == a3);
    std::future<bool> waiting = std::async(std::launch::async, [&] { return record->waitForTip(20000); });
    BOOST_CHECK(waiting.wait_for(Ms{300}) == std::future_status::timeout);
    hooks.hold->Open();
    BOOST_CHECK(waiting.get());
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(v->live_tip_hash == a3->GetBlockHash());
    record->stop();
}

BOOST_AUTO_TEST_CASE(wait_when_node_went_back)
{
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* x = Mine({Message({OpReturn("x")})});
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    // The node leaves the record's tip; the record has not followed yet.
    Invalidate(x);
    std::future<bool> waiting = std::async(std::launch::async, [&] { return record->waitForTip(20000); });
    BOOST_CHECK(waiting.wait_for(Ms{300}) == std::future_status::timeout);
    hooks.hold->Open();
    BOOST_CHECK(waiting.get());
    // The record's tip is then on the active chain.
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(WITH_LOCK(cs_main, return ::ChainActive()[v->live_tip]->GetBlockHash()) == v->live_tip_hash);
    BOOST_CHECK_EQUAL(v->live_tip, x->nHeight - 1);
    record->stop();
}

BOOST_AUTO_TEST_CASE(concurrent_readers)
{
    std::shared_ptr<Record> record = Start();
    Problems problems;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> consistent{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&, i] {
            while (!done) {
                const VisibilityRef v = record->getVisibility();
                Problems found;
                RowRequest request;
                request.older = i % 2 == 1;
                request.from = request.older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
                request.max_rows = 2000;
                request.visibility = v;
                const RowBatch batch = record->readRows(request);
                if (batch.failed) found.Add("a read failed");
                for (const RowView& row : batch.rows) {
                    std::vector<unsigned char> bytes;
                    if (record->readScript(row.key, 0, row.script_size, bytes, v) == ReadResult::FAILED) found.Add("a script read failed");
                    TxView view;
                    if (record->readTransaction({row.key.height, row.txpos}, 1 << 20, ALL_SPENT, view, v) == ReadResult::FAILED) found.Add("a transaction read failed");
                }
                const CBlockIndex* tip = WITH_LOCK(cs_main, return v->live_tip < 0 ? nullptr : LookupBlockIndex(v->live_tip_hash));
                if (tip) CheckHolds(*record, v, tip, found);
                // What was read describes the snapshot only while nothing was removed meanwhile.
                if (record->getRemovals() == v->removals) {
                    ++consistent;
                    for (const std::string& problem : found.List()) problems.Add(problem);
                } else if (batch.failed) {
                    problems.Add("a read failed");
                }
            }
        });
    }
    for (int round = 0; round < 6; ++round) {
        const CBlockIndex* first = Mine({Message({OpReturn(strprintf("r%d", round)), Script(MakeScript(5000 + round, 0x40 + round))})});
        Mine({Message({OpReturn(strprintf("s%d", round))})});
        Invalidate(first);
        Mine({Message({OpReturn(strprintf("t%d", round))})});
        Mine({Message({OpReturn(strprintf("u%d", round))})});
    }
    Catch(*record);
    BOOST_CHECK(WaitUntil([&] { return consistent > 20; }));
    done = true;
    for (std::thread& reader : readers) reader.join();
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 0U);
    record->stop();
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
}

BOOST_AUTO_TEST_CASE(long_script_pieces)
{
    std::shared_ptr<Record> record = Start();
    const std::vector<size_t> sizes{SCRIPT_HEAD_SIZE, SCRIPT_HEAD_SIZE + 1, SCRIPT_PIECE_SIZE, SCRIPT_PIECE_SIZE + 1};
    std::vector<CScript> scripts;
    for (size_t i = 0; i < sizes.size(); ++i) scripts.push_back(Script(MakeScript(sizes[i], -1, uint32_t(i))));
    const CMutableTransaction several = Message(scripts);
    const CBlockIndex* a = Mine({several});
    const CScript largest = Script(MakeScript(998000, -1, 99));
    const TestCoin coin = Take();
    const CMutableTransaction big = Spend({coin}, {CTxOut(0, largest), CTxOut(coin.value - 100000, m_script)});
    const CBlockIndex* b = Mine({big});
    Catch(*record);
    std::vector<std::pair<RowKey, CScript>> rows;
    for (size_t i = 0; i < scripts.size(); ++i) rows.emplace_back(RowKey{a->nHeight, uint32_t(i)}, scripts[i]);
    rows.emplace_back(RowKey{b->nHeight, 0}, largest);
    for (const auto& entry : rows) {
        const CScript& script = entry.second;
        RowView row;
        BOOST_REQUIRE(record->readRow(entry.first, row, nullptr) == ReadResult::FOUND);
        BOOST_CHECK_EQUAL(row.script_size, script.size());
        BOOST_CHECK_EQUAL(row.script_prefix.size(), std::min<size_t>(script.size(), SCRIPT_HEAD_SIZE));
        BOOST_CHECK(std::equal(row.script_prefix.begin(), row.script_prefix.end(), script.begin()));
        std::vector<unsigned char> bytes;
        BOOST_REQUIRE(record->readScript(entry.first, 0, script.size(), bytes, nullptr) == ReadResult::FOUND);
        BOOST_CHECK(CScript(bytes.begin(), bytes.end()) == script);
        // By pieces, across every boundary of the head and of the pieces.
        const size_t size = script.size();
        for (const size_t offset : {size_t{0}, size_t{1}, size_t{4095}, size_t{4096}, size_t{4097}, size_t{65535}, size_t{65536}, size_t{65537}, size - 1, size}) {
            for (const size_t length : {size_t{1}, size_t{2}, size_t{4096}, size_t{70000}}) {
                BOOST_REQUIRE(record->readScript(entry.first, offset, length, bytes, nullptr) == ReadResult::FOUND);
                const size_t from = std::min(offset, size);
                const size_t to = std::min(size, offset + length);
                BOOST_CHECK(bytes == std::vector<unsigned char>(script.begin() + from, script.begin() + to));
            }
        }
    }
    // The transactions read whole hash to their txids.
    for (const auto& tx : {std::make_pair(a, several.GetHash()), std::make_pair(b, big.GetHash())}) {
        TxView view;
        BOOST_REQUIRE(record->readTransaction({tx.first->nHeight, 1}, MAX_OUTPUT_SCRIPT_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
        CMutableTransaction rebuilt;
        BOOST_REQUIRE(view.ToMutableTransaction(rebuilt));
        BOOST_CHECK(rebuilt.GetHash() == tx.second);
        BOOST_CHECK(view.txid == tx.second);
        // Read with the heads only, it cannot be rebuilt.
        BOOST_REQUIRE(record->readTransaction({tx.first->nHeight, 1}, SCRIPT_HEAD_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
        BOOST_CHECK(!view.ToMutableTransaction(rebuilt));
    }
    record->stop();
    // A script longer than its head lies in pieces of 65,536 bytes; the others in no piece.
    const size_t pieces = 1 + 1 + 2 + (998000 + SCRIPT_PIECE_SIZE - 1) / SCRIPT_PIECE_SIZE;
    BOOST_CHECK_EQUAL(CountPrefix(Keys(m_path), 's'), pieces);
}

BOOST_AUTO_TEST_CASE(newest_first_exact)
{
    std::atomic<uint64_t> row_reads{0};
    RecordTestHooks hooks;
    hooks.on_read = [&](uint8_t p, bool worker) {
        if (!worker && p == 'r') ++row_reads;
    };
    std::shared_ptr<Record> record = Start(hooks);
    // Around the rows a page returns: a transaction of 150 rows, blocks of one row, transactions of 40 rows, and
    // rows of 3,000 bytes; and covered heights without rows.
    std::vector<CScript> many;
    for (int i = 0; i < 150; ++i) many.push_back(OpReturn(strprintf("many %d", i)));
    Mine({Message(many)});
    for (int i = 0; i < 3; ++i) Mine({Message({OpReturn(strprintf("one %d", i))})});
    Mine();
    std::vector<CMutableTransaction> forty;
    for (int t = 0; t < 3; ++t) {
        std::vector<CScript> scripts;
        for (int i = 0; i < 40; ++i) scripts.push_back(OpReturn(strprintf("forty %d %d", t, i)));
        forty.push_back(Message(scripts));
    }
    Mine(forty);
    std::vector<CScript> wide;
    for (int i = 0; i < 70; ++i) wide.push_back(Script(MakeScript(3000, -1, uint32_t(i))));
    Mine({Message(wide)});
    Mine();
    Catch(*record);
    const std::vector<RowView> all = AllRows(*record, false);
    BOOST_REQUIRE_EQUAL(all.size(), 150U + 3 + 120 + 70);
    for (size_t i = 1; i < all.size(); ++i) BOOST_CHECK(all[i - 1].key < all[i].key);
    const std::vector<RowView> reversed = AllRows(*record, true);
    BOOST_REQUIRE_EQUAL(reversed.size(), all.size());
    for (size_t i = 0; i < all.size(); ++i) {
        BOOST_CHECK(reversed[all.size() - 1 - i].key == all[i].key);
        BOOST_CHECK(reversed[all.size() - 1 - i].tx.txid == all[i].tx.txid);
    }

    // Every page, in either order, from every kind of position: what it returns, and what it reads besides.
    std::vector<RowKey> starts{ROW_KEY_BEFORE_ALL, ROW_KEY_AFTER_ALL};
    for (const size_t i : {size_t{0}, size_t{1}, size_t{70}, size_t{149}, size_t{150}, size_t{152}, size_t{170}, size_t{200}, size_t{272}, size_t{300}, all.size() - 1}) {
        starts.push_back(all[i].key);
    }
    for (const bool older : {false, true}) {
        for (const RowKey& from : starts) {
            for (const size_t k : {size_t{1}, size_t{2}, size_t{63}, size_t{64}, size_t{65}, size_t{100}, size_t{200}, size_t{1000}}) {
                for (const size_t max_bytes : {std::numeric_limits<size_t>::max(), size_t{10000}}) {
                    RowRequest request;
                    request.from = from;
                    request.older = older;
                    request.max_rows = k;
                    request.max_bytes = max_bytes;
                    row_reads = 0;
                    const RowBatch batch = record->readRows(request);
                    BOOST_REQUIRE(!batch.failed);
                    // Exactly the rows after (or before) the position, in order.
                    std::vector<RowView> expected;
                    for (const RowView& row : all) {
                        if (older ? row.key < from : from < row.key) expected.push_back(row);
                    }
                    if (older) std::reverse(expected.begin(), expected.end());
                    size_t bytes = 0;
                    size_t want = 0;
                    const auto cost = [&](size_t i) { return expected[i].script_prefix.size() + expected[i].tx.paid_by.size(); };
                    while (want < expected.size() && want < k && (want == 0 || bytes + cost(want) <= max_bytes)) {
                        bytes += cost(want);
                        ++want;
                    }
                    BOOST_REQUIRE_EQUAL(batch.rows.size(), want);
                    for (size_t i = 0; i < want; ++i) BOOST_CHECK(batch.rows[i].key == expected[i].key && batch.rows[i].tx.txid == expected[i].tx.txid);
                    // The end is told when the page found it; a page that stops at its count does not look further.
                    if (want < expected.size()) BOOST_CHECK(!batch.end);
                    if (want == expected.size() && want < k) BOOST_CHECK(batch.end);
                    // The reads beyond the rows returned: the rest of a range a page stops in, and one first row
                    // for each range that begins inside a transaction (each block begins a range).
                    std::set<int32_t> heights;
                    for (const RowView& row : batch.rows) heights.insert(row.key.height);
                    const uint64_t ranges = (want + READ_RANGE_ROWS - 1) / READ_RANGE_ROWS + heights.size();
                    BOOST_CHECK_MESSAGE(row_reads <= want + 63 + ranges,
                                        "order " << older << " k " << k << ": " << row_reads << " reads for " << want << " rows");
                }
            }
        }
    }
    // Paging with the position returned gives every row once.
    for (const bool older : {false, true}) {
        RowRequest request;
        request.from = older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
        request.older = older;
        request.max_rows = 7;
        std::vector<RowView> paged;
        while (true) {
            const RowBatch batch = record->readRows(request);
            paged.insert(paged.end(), batch.rows.begin(), batch.rows.end());
            request.from = batch.next;
            if (batch.end) break;
        }
        BOOST_REQUIRE_EQUAL(paged.size(), all.size());
        for (size_t i = 0; i < all.size(); ++i) BOOST_CHECK(paged[older ? all.size() - 1 - i : i].key == all[i].key);
    }
    // Heights asked for bound a read in both directions.
    RowRequest request;
    request.heights = {all[200].key.height, all[200].key.height};
    request.max_rows = 1000;
    row_reads = 0;
    BOOST_CHECK_EQUAL(record->readRows(request).rows.size(), 120U);
    BOOST_CHECK(row_reads <= 120U);
    request.older = true;
    request.from = ROW_KEY_AFTER_ALL;
    row_reads = 0;
    BOOST_CHECK_EQUAL(record->readRows(request).rows.size(), 120U);
    BOOST_CHECK(row_reads <= 120U + 3);
    record->stop();
}

BOOST_AUTO_TEST_CASE(reader_error_is_counted)
{
    std::atomic<int> reader_failures{0};
    std::atomic<bool> worker_fails{false};
    std::atomic<int> worker_row_reads{0};
    RecordTestHooks hooks;
    hooks.on_read = [&](uint8_t p, bool worker) {
        if (p != 'r') return;
        if (worker) {
            ++worker_row_reads;
            if (worker_fails) throw dbwrapper_error("injected");
        } else if (reader_failures > 0) {
            --reader_failures;
            throw dbwrapper_error("injected");
        }
    };
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* index = Mine({Message({OpReturn("read")})});
    Catch(*record);
    BOOST_REQUIRE(WaitUntil([&] {
        RowView row;
        return record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    // A read that fails once: the call gets the failure and the count grows; the worker's own read of the key
    // works, so the record goes on.
    const int worker_reads = worker_row_reads;
    reader_failures = 1;
    RowView row;
    BOOST_CHECK(record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 1U);
    BOOST_CHECK(WaitUntil([&] { return worker_row_reads > worker_reads; }));
    BOOST_CHECK(record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    const int before_two = worker_row_reads;
    reader_failures = 1;
    const RowBatch batch = record->readRows(RowRequest());
    BOOST_CHECK(batch.failed);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 2U);
    std::vector<unsigned char> bytes;
    reader_failures = 1;
    BOOST_CHECK(record->readScript({index->nHeight, 0}, 0, 10, bytes, nullptr) == ReadResult::FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 3U);
    // The worker has made both reads again, each reading the row, and they worked.
    BOOST_CHECK(WaitUntil([&] { return worker_row_reads >= before_two + 2; }));
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    // A read that fails again when the worker reads the key: the record fails, with READ_FAILED.
    worker_fails = true;
    reader_failures = 1;
    BOOST_CHECK(record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FAILED);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 4U);
    // What it holds stays readable.
    worker_fails = false;
    BOOST_CHECK(record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    record->stop();
}

BOOST_AUTO_TEST_CASE(rows_from_mweb_vectors)
{
    const UniValue vectors = Vectors();
    const int first_mweb = vectors["first_mweb_height"].get_int();
    size_t rows_seen = 0;
    for (const UniValue& entry : vectors["blocks"].getValues()) {
        const std::shared_ptr<CBlock> block = VectorBlock(entry);
        const bool mweb = entry["height"].get_int() >= first_mweb;
        BOOST_CHECK_EQUAL(entry["mweb"].get_bool(), mweb);
        // A block of MWEB ends with its integrating transaction and carries the extension block.
        BOOST_CHECK_EQUAL(block->vtx.back()->IsHogEx(), mweb);
        BOOST_CHECK_EQUAL(!block->mweb_block.IsNull(), mweb);
        const std::shared_ptr<BlockRows> rows = ReadBlockRows(*block, block->GetHash());
        const std::vector<UniValue>& expected = entry["rows"].getValues();
        BOOST_REQUIRE_EQUAL(rows ? rows->rows.size() : 0, expected.size());
        if (!rows) continue;
        rows_seen += rows->rows.size();
        for (size_t i = 0; i < expected.size(); ++i) {
            const Row& row = rows->rows[i];
            BOOST_CHECK_EQUAL(row.txpos, uint32_t(expected[i]["txpos"].get_int()));
            BOOST_CHECK_EQUAL(row.vout, uint32_t(expected[i]["vout"].get_int()));
            BOOST_CHECK_EQUAL(row.value, expected[i]["value"].get_int64());
            BOOST_CHECK_EQUAL(HexStr(WholeScript(*rows, i)), expected[i]["script"].get_str());
        }
        // The transactions with rows, their kinds, and the coins they spend from the block's undo data.
        const CBlockUndo undo = VectorUndo(entry);
        BOOST_CHECK_EQUAL(undo.vtxundo.size(), block->vtx.size() - 1);
        const std::vector<UniValue>& txs = entry["txs"].getValues();
        BOOST_REQUIRE_EQUAL(rows->txs.size(), txs.size());
        for (size_t t = 0; t < txs.size(); ++t) {
            const uint32_t txpos = rows->txs[t].first;
            TxRecord record = rows->txs[t].second;
            BOOST_CHECK_EQUAL(txpos, uint32_t(txs[t]["txpos"].get_int()));
            BOOST_CHECK_EQUAL(record.txid.GetHex(), txs[t]["txid"].get_str());
            BOOST_CHECK_EQUAL(bool(record.tx_flags & TX_COINBASE), txs[t]["coinbase"].get_bool());
            BOOST_CHECK_EQUAL(bool(record.tx_flags & TX_HOGEX), txs[t]["hogex"].get_bool());
            BOOST_CHECK_EQUAL(bool(record.tx_flags & TX_HAS_PEGIN), txs[t]["pegin"].get_bool());
            // A confirmed transaction has no MWEB part: the miner left it in the extension block.
            BOOST_CHECK(!(record.tx_flags & TX_HAS_MWEB));
            Row first = *std::find_if(rows->rows.begin(), rows->rows.end(), [&](const Row& r) { return r.txpos == txpos && r.IsFirst(); });
            const std::vector<Coin>* coins = txpos == 0 ? nullptr : &undo.vtxundo.at(txpos - 1).vprevout;
            ResolveSpentCoins(record, first.brief, coins);
            const std::vector<UniValue>& inputs = txs[t]["inputs"].getValues();
            if (record.tx_flags & TX_COINBASE) {
                BOOST_CHECK(first.brief.paid_by_kind == PaidByKind::COINBASE);
                BOOST_CHECK(record.vin[0].spent_state == SpentState::NONE);
                continue;
            }
            BOOST_REQUIRE_EQUAL(record.vin.size(), inputs.size());
            CAmount in = 0;
            for (size_t i = 0; i < inputs.size(); ++i) {
                BOOST_REQUIRE(record.vin[i].spent_state == SpentState::KNOWN);
                BOOST_CHECK_EQUAL(record.vin[i].coin.out.nValue, inputs[i]["value"].get_int64());
                BOOST_CHECK_EQUAL(HexStr(record.vin[i].coin.out.scriptPubKey), inputs[i]["script"].get_str());
                BOOST_CHECK_EQUAL(int(record.vin[i].coin.nHeight), inputs[i]["height"].get_int());
                BOOST_CHECK_EQUAL(bool(record.vin[i].coin.fCoinBase), inputs[i]["coinbase"].get_bool());
                in += inputs[i]["value"].get_int64();
            }
            BOOST_CHECK(first.brief.paid_by_kind == PaidByKind::SCRIPT);
            BOOST_CHECK_EQUAL(HexStr(first.brief.paid_by), inputs[0]["script"].get_str());
            BOOST_CHECK_EQUAL(first.brief.fee, in - block->vtx[txpos]->GetValueOut());
        }
    }
    BOOST_CHECK_EQUAL(rows_seen, 7U);

    // The integrating transaction of a block with a peg-out, given an OP_RETURN script for it: a row paid by the
    // integrating transaction, whose coins are known and whose fee is not computed.
    const UniValue& pegout = vectors["blocks"][4];
    const std::shared_ptr<CBlock> block = VectorBlock(pegout);
    CMutableTransaction hogex(*block->vtx.back());
    BOOST_REQUIRE(hogex.m_hogEx);
    BOOST_REQUIRE(hogex.vout.size() >= 2);
    hogex.vout[1].scriptPubKey = OpReturn("pegged out");
    block->vtx.back() = MakeTransactionRef(hogex);
    std::shared_ptr<BlockRows> rows = ReadBlockRows(*block, block->GetHash());
    BOOST_REQUIRE(rows);
    BOOST_REQUIRE_EQUAL(rows->rows.size(), 1U);
    BOOST_CHECK_EQUAL(rows->rows[0].txpos, block->vtx.size() - 1);
    BOOST_CHECK(rows->rows[0].brief.paid_by_kind == PaidByKind::HOGEX);
    TxRecord record = rows->txs[0].second;
    BOOST_CHECK(record.tx_flags & TX_HOGEX);
    TxBrief brief = rows->rows[0].brief;
    const CBlockUndo undo = VectorUndo(pegout);
    ResolveSpentCoins(record, brief, &undo.vtxundo.back().vprevout);
    BOOST_CHECK(brief.paid_by_kind == PaidByKind::HOGEX);
    BOOST_CHECK(record.vin[0].spent_state == SpentState::KNOWN);
    BOOST_CHECK_EQUAL(brief.fee, -1);

    // A transaction with a peg-in output and a row: its fee is not computed either.
    const CScript pegin = CScript() << OP_9 << std::vector<unsigned char>(32, 0x77);
    BOOST_REQUIRE(pegin.IsMWEBPegin());
    const std::shared_ptr<const CBlock> with_pegin = LooseBlock({CTxOut(1000, pegin), CTxOut(0, OpReturn("in"))});
    rows = ReadBlockRows(*with_pegin, with_pegin->GetHash());
    BOOST_REQUIRE(rows);
    record = rows->txs[0].second;
    brief = rows->rows[0].brief;
    BOOST_CHECK(record.tx_flags & TX_HAS_PEGIN);
    const std::vector<Coin> coins{Coin(CTxOut(5000, CScript() << OP_TRUE), 1, false, false)};
    ResolveSpentCoins(record, brief, &coins);
    BOOST_CHECK(brief.paid_by_kind == PaidByKind::SCRIPT);
    BOOST_CHECK_EQUAL(brief.fee, -1);
}

BOOST_AUTO_TEST_CASE(txrecord_hashes_to_txid)
{
    // Every transaction record of the MWEB vectors (legacy, wrapped and native segwit inputs, a coinbase, an MWEB
    // block's transactions, an integrating transaction given a row), stored and read back, hashes to its txid.
    const UniValue vectors = Vectors();
    size_t records = 0;
    for (const UniValue& entry : vectors["blocks"].getValues()) {
        std::shared_ptr<CBlock> block = VectorBlock(entry);
        if (block->vtx.back()->IsHogEx()) {
            CMutableTransaction hogex(*block->vtx.back());
            hogex.vout.emplace_back(0, OpReturn("a row of the integrating transaction"));
            block->vtx.back() = MakeTransactionRef(hogex);
        }
        const std::shared_ptr<BlockRows> rows = ReadBlockRows(*block, block->GetHash());
        if (!rows) continue;
        for (const auto& tx : rows->txs) {
            BOOST_CHECK(Rebuild(tx.second, *rows).GetHash() == block->vtx[tx.first]->GetHash());
            ++records;
        }
    }
    BOOST_CHECK(records >= 9);

    // And through the running record, for transactions of the chain.
    std::shared_ptr<Record> record = Start();
    const TestCoin one = Take();
    const TestCoin two = Take();
    const CMutableTransaction both = Spend({one, two}, {CTxOut(0, OpReturn("x")), CTxOut(one.value + two.value - 5000, m_script), CTxOut(0, OpReturn("y"))});
    const CBlockIndex* index = Mine({both}, {CTxOut(0, OpReturn("coinbase"))});
    Catch(*record);
    const std::shared_ptr<const CBlock> block = WITH_LOCK(m_blocks_mutex, return m_blocks.at(index->GetBlockHash()));
    for (const uint32_t txpos : {0U, 1U}) {
        TxView view;
        BOOST_REQUIRE(record->readTransaction({index->nHeight, txpos}, MAX_OUTPUT_SCRIPT_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
        CMutableTransaction rebuilt;
        BOOST_REQUIRE(view.ToMutableTransaction(rebuilt));
        BOOST_CHECK(rebuilt.GetHash() == block->vtx[txpos]->GetHash());
        BOOST_CHECK(view.block_hash == index->GetBlockHash());
    }
    record->stop();
}

BOOST_AUTO_TEST_CASE(mweb_blocks_reorg)
{
    // The MWEB vectors given to the record as blocks after the chain's tip, then a reorganisation back to the
    // block before MWEB and the same blocks again.
    const UniValue vectors = Vectors();
    std::shared_ptr<Record> record = Start();
    std::vector<std::pair<const CBlockIndex*, std::shared_ptr<CBlock>>> chain;
    const CBlockIndex* prev = Tip();
    for (const UniValue& entry : vectors["blocks"].getValues()) {
        const std::shared_ptr<CBlock> block = VectorBlock(entry);
        chain.emplace_back(Fake(prev, block->GetHash()), block);
        prev = chain.back().first;
    }
    const auto expected_rows = [&](size_t i) { return vectors["blocks"][i]["rows"].size(); };
    const auto check_all = [&](size_t connected) {
        for (size_t i = 0; i < chain.size(); ++i) {
            BOOST_CHECK_EQUAL(RowsAt(*record, chain[i].first->nHeight).size(), i < connected ? expected_rows(i) : 0U);
        }
    };
    for (const auto& block : chain) record->BlockConnected(block.second, block.first);
    Catch(*record, chain.back().first);
    check_all(chain.size());
    const uint256 txid = chain[2].second->vtx[1]->GetHash();
    TxKey key;
    BOOST_CHECK(record->findTransaction(txid, key, nullptr) == ReadResult::FOUND);
    size_t first_mweb = 0;
    while (!chain[first_mweb].second->vtx.back()->IsHogEx()) ++first_mweb;
    for (size_t i = chain.size(); i-- > first_mweb;) record->BlockDisconnected(chain[i].second, chain[i].first);
    Catch(*record, chain[first_mweb - 1].first);
    check_all(first_mweb);
    BOOST_CHECK(record->findTransaction(txid, key, nullptr) == ReadResult::NOT_FOUND);
    for (size_t i = first_mweb; i < chain.size(); ++i) record->BlockConnected(chain[i].second, chain[i].first);
    Catch(*record, chain.back().first);
    check_all(chain.size());
    BOOST_CHECK(record->findTransaction(txid, key, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(key == TxKey({chain[2].first->nHeight, 1}));
    // The blocks have no undo data here: their coins are not available, and the record goes on.
    BOOST_CHECK(WaitUntil([&] {
        RowView row;
        return record->readRow({chain[2].first->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind == PaidByKind::UNAVAILABLE;
    }));
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    record->stop();
}

BOOST_AUTO_TEST_CASE(damaged_rows)
{
    // Rows in a table file of the database whose bytes are damaged: a reader's scan and the worker's read of the
    // same place both fail, so the record fails with READ_FAILED; the node goes on.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* first = nullptr;
    for (int b = 0; b < 3; ++b) {
        std::vector<CScript> scripts;
        for (int i = 0; i < 200; ++i) scripts.push_back(Script(MakeScript(4000, -1, uint32_t(b * 1000 + i))));
        const CBlockIndex* index = Mine({Message(scripts)});
        if (!first) first = index;
    }
    Catch(*record);
    // Until the database has moved the rows from its log into a table file, which its manifest then lists.
    BOOST_REQUIRE(WaitUntil([&] {
        for (fs::directory_iterator it(m_path), end; it != end; ++it) {
            if (it->path().filename().string().compare(0, 9, "MANIFEST-") == 0 && fs::file_size(it->path()) > 100) return true;
        }
        return false;
    }));
    record->stop();
    // The largest table file holds the rows; damage the middle of it.
    fs::path table;
    uintmax_t size = 0;
    for (fs::directory_iterator it(m_path), end; it != end; ++it) {
        if (it->path().extension() == ".ldb" && fs::file_size(it->path()) > size) {
            table = it->path();
            size = fs::file_size(table);
        }
    }
    BOOST_REQUIRE_MESSAGE(size > 1000000, "no table file of the rows");
    {
        std::fstream file(table.string(), std::ios::in | std::ios::out | std::ios::binary);
        file.seekp(size / 2);
        const std::string junk(4096, '\x5a');
        file.write(junk.data(), junk.size());
    }
    record = Start();
    BOOST_REQUIRE(record->getStatus().state == State::BACKFILLING);
    const RowBatch batch = record->readRows(RowRequest{ROW_KEY_BEFORE_ALL, false, {0, std::numeric_limits<int32_t>::max()}, 1000, SCRIPT_HEAD_SIZE, std::numeric_limits<size_t>::max(), nullptr});
    BOOST_CHECK(batch.failed);
    BOOST_CHECK(batch.rows.size() < 600);
    BOOST_CHECK(record->getStatus().read_errors >= 1);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    Mine();
    record->stop();
    // The worker meets the damage itself when the blocks are disconnected and it erases their rows.
    record = Start();
    BOOST_REQUIRE(record->getStatus().state == State::BACKFILLING);
    Invalidate(first);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 0U);
    record->stop();
}

BOOST_AUTO_TEST_CASE(lost_keys)
{
    // Keys missing from the database, as a scan meets them when a part of a table file cannot be read: a block
    // entry, a block's last row; and a row more than its block entry counts. Reads in either order fail, and so
    // does the worker's own read of the same heights, so the record fails with READ_FAILED.
    std::shared_ptr<Record> record = Start();
    std::vector<const CBlockIndex*> blocks;
    for (int i = 0; i < 4; ++i) blocks.push_back(Mine({Message({OpReturn(strprintf("lost %d a", i)), OpReturn(strprintf("lost %d b", i))})}));
    Catch(*record);
    BOOST_REQUIRE(WaitUntil([&] {
        RowView row;
        return record->readRow({blocks[0]->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    record->stop();
    struct Raw {
        std::vector<unsigned char> bytes;
        void Unserialize(CDataStream& s)
        {
            bytes.resize(s.size());
            if (!bytes.empty()) s.read((char*)bytes.data(), bytes.size());
        }
        void Serialize(CDataStream& s) const { s.write((const char*)bytes.data(), bytes.size()); }
    };
    const auto both_orders_fail = [&](Record& r) {
        for (const bool older : {false, true}) {
            RowRequest request;
            request.older = older;
            request.from = older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
            request.max_rows = 1000;
            BOOST_CHECK_MESSAGE(r.readRows(request).failed, "order " << older);
        }
    };
    // A block entry missing: its rows are there without it.
    const HeightKey entry{'b', blocks[1]->nHeight};
    Raw saved;
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Read(entry, saved));
        BOOST_REQUIRE(db.Erase(entry));
    }
    record = Start();
    both_orders_fail(*record);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(entry, saved));
    }
    // A row more than its block's entry counts.
    const RowDbKey extra(RowKey{blocks[3]->nHeight, 2});
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Read(RowDbKey(RowKey{blocks[3]->nHeight, 1}), saved));
        BOOST_REQUIRE(db.Write(extra, saved));
    }
    record = Start();
    both_orders_fail(*record);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(extra));
    }
    // A block's last row missing: the reads fail, and the worker's erase of the height when its block is
    // disconnected fails too.
    const RowDbKey last(RowKey{blocks[2]->nHeight, 1});
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(last));
    }
    record = Start();
    std::vector<RowView> before = RowsAt(*record, blocks[1]->nHeight);
    BOOST_CHECK_EQUAL(before.size(), 2U);
    RowRequest request;
    request.from = {blocks[2]->nHeight - 1, NO_N};
    request.heights = {blocks[2]->nHeight, blocks[2]->nHeight};
    BOOST_CHECK(record->readRows(request).failed);
    record->stop();
    record = Start();
    Invalidate(blocks[2]);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 0U);
    record->stop();
}

BOOST_AUTO_TEST_CASE(publish_after_write)
{
    // A reader that reads a height the moment it becomes visible finds all of its block's rows: the worker writes a
    // batch before it publishes what readers may see. Blocks of large rows, so that each write takes a while.
    std::shared_ptr<Record> record = Start();
    const int blocks = 3;
    const size_t per_block = 250;
    std::vector<std::shared_ptr<const CBlock>> made;
    std::vector<const CBlockIndex*> indexes;
    const CBlockIndex* prev = Tip();
    for (int b = 0; b < blocks; ++b) {
        std::vector<CTxOut> outputs;
        for (size_t i = 0; i < per_block; ++i) outputs.emplace_back(0, Script(MakeScript(60000, -1, uint32_t(b * 1000 + i))));
        made.push_back(LooseBlock(outputs));
        indexes.push_back(Fake(prev, made.back()->GetHash()));
        prev = indexes.back();
    }
    std::atomic<bool> done{false};
    std::atomic<int> seen{0}, wrong{0};
    std::thread reader([&] {
        int32_t last = record->getVisibility()->live_tip;
        while (!done) {
            const VisibilityRef v = record->getVisibility();
            if (v->live_tip == last) continue;
            last = v->live_tip;
            if (!v->IsVisible(last)) continue;
            RowRequest request;
            request.from = {last - 1, NO_N};
            request.heights = {last, last};
            request.max_rows = 100000;
            request.max_bytes = std::numeric_limits<size_t>::max();
            request.visibility = v;
            const RowBatch batch = record->readRows(request);
            ++seen;
            // Fewer rows, while nothing was removed: the snapshot was ahead of the database.
            if ((batch.failed || batch.rows.size() != per_block) && record->getRemovals() == v->removals) ++wrong;
        }
    });
    for (int b = 0; b < blocks; ++b) record->BlockConnected(made[b], indexes[b]);
    BOOST_CHECK(WaitUntil([&] { return record->getVisibility()->live_tip_hash == prev->GetBlockHash(); }, Ms{120000}));
    BOOST_CHECK(WaitUntil([&] { return seen > 0; }));
    done = true;
    reader.join();
    record->stop();
    BOOST_CHECK_EQUAL(wrong.load(), 0);
}

BOOST_AUTO_TEST_CASE(confirm_makes_the_same_read)
{
    // A transaction read that fails on a piece of a script the database lacks: the call fails and is counted, and
    // the worker's own read, the same read, fails too, so the record fails with READ_FAILED.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({Script(MakeScript(5000, 0x41))})});
    Catch(*record);
    BOOST_REQUIRE(WaitUntil([&] {
        RowView row;
        return record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Exists(PieceDbKey(RowKey{a->nHeight, 0}, 0)));
        BOOST_REQUIRE(db.Erase(PieceDbKey(RowKey{a->nHeight, 0}, 0)));
    }
    record = Start();
    RowView row;
    BOOST_CHECK(record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    TxView view;
    BOOST_CHECK(record->readTransaction({a->nHeight, 1}, MAX_OUTPUT_SCRIPT_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, 1U);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    record->stop();
}

BOOST_AUTO_TEST_CASE(listener_throws)
{
    // A listener that throws, on the worker's thread and on that of a validation callback in which the record fails:
    // its exception goes no further, the record goes on as before, and so does the node.
    std::atomic<bool> fail_callback{false};
    RecordTestHooks hooks;
    hooks.in_block_connected = [&] {
        if (fail_callback) throw std::runtime_error("a fault of the record's");
    };
    std::shared_ptr<Record> record = Start(hooks);
    std::atomic<int> calls{0};
    std::unique_ptr<interfaces::Handler> handler = record->handleChanged([&](const RecordChange&) {
        ++calls;
        throw std::runtime_error("a listener's");
    });
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    BOOST_CHECK(WaitUntil([&] { return calls > 0; }));
    BOOST_CHECK_EQUAL(RowsAt(*record, a->nHeight).size(), 1U);
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    // The record fails in a callback that the test makes itself: nothing leaves it.
    fail_callback = true;
    const std::shared_ptr<const CBlock> block = LooseBlock({CTxOut(0, OpReturn("b"))});
    const int before = calls;
    bool escaped = false;
    try {
        record->BlockConnected(block, Fake(Tip(), block->GetHash()));
    } catch (...) {
        escaped = true;
    }
    BOOST_CHECK(!escaped);
    BOOST_CHECK(record->getStatus().state == State::FAILED);
    BOOST_CHECK(calls > before);
    fail_callback = false;
    Mine();
    handler->disconnect();
    record->stop();
}

BOOST_AUTO_TEST_CASE(listener_lets_go)
{
    // A listener that calls the record's stop() and then lets go of the last reference to the record, on the worker's
    // thread: the stop does nothing, and the record is not destroyed under its worker, which ends by itself.
    std::shared_ptr<Record> record = Start();
    Mine({Message({OpReturn("a")})});
    Catch(*record);
    std::weak_ptr<Record> weak = record;
    Record* raw = record.get();
    auto holder = std::make_shared<std::shared_ptr<Record>>(std::move(record));
    std::atomic<bool> called{false}, gone_in_call{false};
    std::unique_ptr<interfaces::Handler> handler = raw->handleChanged([holder, &weak, &called, &gone_in_call, raw](const RecordChange&) {
        if (called.exchange(true) || !*holder) return;
        raw->stop();
        holder->reset();
        gone_in_call = weak.expired();
    });
    Mine({Message({OpReturn("b")})});
    BOOST_CHECK(WaitUntil([&] { return called.load(); }));
    BOOST_CHECK(WaitUntil([&] { return weak.expired(); }, Ms{10000}));
    BOOST_CHECK(!gone_in_call);
    handler.reset();
    BOOST_REQUIRE(WaitUntilClosed(m_path));
    // Ended without a stop: the next start finds the record as a crash leaves it.
    record = Start();
    BOOST_CHECK(record->getStatus().state != State::FAILED);
    record->stop();
}

BOOST_AUTO_TEST_CASE(queue_bound_counts_taken_items)
{
    // The bound of the queue counts the blocks the worker has taken until it has written them: while it holds a full
    // batch, a block with rows is refused.
    std::vector<std::shared_ptr<const CBlock>> made;
    for (int i = 0; i < 6; ++i) made.push_back(LooseBlock({CTxOut(0, Script(MakeScript(3000, -1, uint32_t(i))))}));
    const size_t each = ReadBlockRows(*made[0], made[0]->GetHash())->bytes;
    std::promise<void> in_write;
    std::promise<void> go;
    std::shared_future<void> released = go.get_future().share();
    std::atomic<bool> armed{false};
    RecordTestHooks hooks;
    hooks.queue_bound = 3 * each + 64;
    hooks.before_write = [&](const BatchInfo& info) {
        if (info.kind == BatchInfo::Kind::ITEMS && armed.exchange(false)) {
            in_write.set_value();
            released.wait_for(std::chrono::seconds{20});
        }
    };
    auto record = std::make_shared<Record>(GetDataDir() / "taken", RecordOptions(), hooks);
    record->load();
    std::vector<const CBlockIndex*> indexes;
    const CBlockIndex* prev = Tip();
    for (const auto& b : made) {
        indexes.push_back(Fake(prev, b->GetHash()));
        prev = indexes.back();
    }
    armed = true;
    // Three blocks fill the queue; the worker is held while it writes the first.
    for (int i = 0; i < 3; ++i) record->BlockConnected(made[i], indexes[i]);
    BOOST_REQUIRE(in_write.get_future().wait_for(std::chrono::seconds{20}) == std::future_status::ready);
    for (int i = 3; i < 6; ++i) record->BlockConnected(made[i], indexes[i]);
    go.set_value();
    BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip_hash == indexes[5]->GetBlockHash(); }));
    const VisibilityRef v = record->getVisibility();
    for (int i = 0; i < 6; ++i) BOOST_CHECK_MESSAGE(v->IsVisible(indexes[i]->nHeight) == (i < 3), "block " << i);
    record->stop();
}

BOOST_AUTO_TEST_CASE(switch_while_ending)
{
    // A switch on asked while the worker ends its switch off, and a rebuild asked while it ends after a failure:
    // each is made once the run has ended. The listener holds the worker in its end until the test has asked.
    std::atomic<bool> fail{false};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo&) {
        if (fail) throw dbwrapper_error("injected");
    };
    std::shared_ptr<Record> record = Start(hooks);
    Mine({Message({OpReturn("a")})});
    Catch(*record);
    Record* raw = record.get();
    std::atomic<int> wait_for{-1}; // the state at which the listener holds the worker
    std::promise<void> held_promise;
    std::future<void> held = held_promise.get_future();
    std::promise<void> go_promise;
    std::shared_future<void> go = go_promise.get_future().share();
    std::unique_ptr<interfaces::Handler> handler = record->handleChanged([&, raw](const RecordChange&) {
        const int state = int(raw->getStatus().state);
        if (state != wait_for) return;
        wait_for = -1;
        held_promise.set_value();
        go.wait_for(std::chrono::seconds{20});
    });
    const auto running = [&] {
        const State state = record->getStatus().state;
        return state == State::RUNNING || state == State::BACKFILLING;
    };
    // Off, then on while the worker still ends.
    wait_for = int(State::OFF);
    record->setRecordEnabled(false);
    BOOST_REQUIRE(held.wait_for(std::chrono::seconds{20}) == std::future_status::ready);
    record->setRecordEnabled(true);
    go_promise.set_value();
    BOOST_CHECK(WaitUntil(running, Ms{10000}));
    // A failure, then a rebuild while the worker still ends.
    held_promise = std::promise<void>();
    held = held_promise.get_future();
    go_promise = std::promise<void>();
    go = go_promise.get_future().share();
    wait_for = int(State::FAILED);
    fail = true;
    Mine({Message({OpReturn("b")})});
    BOOST_REQUIRE(held.wait_for(std::chrono::seconds{20}) == std::future_status::ready);
    fail = false;
    record->requestRebuild();
    go_promise.set_value();
    BOOST_CHECK(WaitUntil([&] { return running() && record->getStatus().rows == 0; }, Ms{10000}));
    handler.reset();
    record->stop();
}

BOOST_AUTO_TEST_CASE(wait_zero_is_a_poll)
{
    // A wait with no time left only looks: it notes no stall, so the next caller with time still waits.
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    Mine({Message({OpReturn("behind")})});
    BOOST_CHECK(!record->waitForTip(0));
    BOOST_CHECK(!record->waitForTip(-5));
    const auto begun = Clock::now();
    BOOST_CHECK(!record->waitForTip(300));
    BOOST_CHECK(Clock::now() - begun >= Ms{250});
    hooks.hold->Open();
    Catch(*record);
    BOOST_CHECK(record->waitForTip(0));
    record->stop();
}

BOOST_AUTO_TEST_CASE(wait_during_initial_download)
{
    // While the node is in its initial block download, a record that has not caught up does not wait.
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    std::shared_ptr<Record> record = Start(hooks);
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    const CBlockIndex* behind = Mine({Message({OpReturn("behind")})});
    uiInterface.NotifyBlockTip(SynchronizationState::INIT_DOWNLOAD, behind);
    BOOST_CHECK(record->getStatus().initial_download);
    const auto begun = Clock::now();
    BOOST_CHECK(!record->waitForTip(10000));
    BOOST_CHECK(Clock::now() - begun < Ms{5000});
    hooks.hold->Open();
    Catch(*record);
    // Caught up, it says so also during initial download.
    BOOST_CHECK(record->waitForTip(3000));
    uiInterface.NotifyBlockTip(SynchronizationState::POST_INIT, behind);
    BOOST_CHECK(!record->getStatus().initial_download);
    record->stop();
}

BOOST_AUTO_TEST_CASE(tx_moves_down_keeps_location)
{
    // A transaction T is in the block at height 53 of a branch, and after a reorganisation while the record was
    // stopped, in the block at height 52 of another. The record starts with the node behind its tip on the first
    // branch, so the new block at 52 is written first, and then the new block at 53 replaces the old one, which held
    // T: T's location, which points to height 52, stays.
    const CMutableTransaction moving = Message({OpReturn("moving")});
    const CBlockIndex* a1 = Mine({Message({OpReturn("a1")})});
    const CBlockIndex* a2 = Mine({Message({OpReturn("a2")})});
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a3 = Mine({moving});
    Mine({Message({OpReturn("a4")})});
    Catch(*record);
    record->stop();
    Invalidate(a2);
    BOOST_REQUIRE(Tip() == a1);
    record = Start();
    const CBlockIndex* b2 = Process(MakeBlock(Tip(), {moving}));
    BOOST_REQUIRE(Tip() == b2);
    const CBlockIndex* b3 = Mine({Message({OpReturn("b3")})});
    BOOST_REQUIRE(b3->nHeight == a3->nHeight);
    Catch(*record);
    TxKey key;
    BOOST_REQUIRE(record->findTransaction(moving.GetHash(), key, nullptr) == ReadResult::FOUND);
    BOOST_CHECK_EQUAL(key.height, b2->nHeight);
    record->stop();
}

BOOST_AUTO_TEST_CASE(start_behind_on_another_branch)
{
    // The node is behind the record's tip, but on another branch: the heights above the fork leave coverage.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a1 = Mine({Message({OpReturn("a1")})});
    const CBlockIndex* a2 = Mine({Message({OpReturn("a2")})});
    Mine({Message({OpReturn("a3")})});
    Mine({Message({OpReturn("a4")})});
    Catch(*record);
    record->stop();
    Invalidate(a2);
    const CBlockIndex* b2 = Mine({Message({OpReturn("b2")})});
    BOOST_REQUIRE(b2->nHeight == a2->nHeight && Tip() == b2);
    record = Start();
    const VisibilityRef v = record->getVisibility();
    BOOST_CHECK(*v->coverage == std::vector<HeightRange>({{a1->nHeight, a1->nHeight}}));
    Problems problems;
    CheckHolds(*record, v, Tip(), problems);
    const CBlockIndex* b3 = Mine({Message({OpReturn("b3")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(RowsAt(*record, b3->nHeight).size(), 1U);
    CheckHolds(*record, record->getVisibility(), Tip(), problems);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
    record->stop();
}

BOOST_AUTO_TEST_CASE(disconnect_of_another_block)
{
    // A disconnection names a block that is not the one stored at its height: the stored rows are not erased.
    std::shared_ptr<Record> record = Start();
    Catch(*record);
    const std::shared_ptr<const CBlock> stored = LooseBlock({CTxOut(0, OpReturn("stored")), CTxOut(0, OpReturn("too"))});
    const CBlockIndex* stored_index = Fake(Tip(), stored->GetHash());
    record->BlockConnected(stored, stored_index);
    BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip_hash == stored_index->GetBlockHash(); }));
    BOOST_REQUIRE_EQUAL(record->getStatus().rows, 2U);
    const std::shared_ptr<const CBlock> other = LooseBlock({CTxOut(0, OpReturn("other"))});
    const CBlockIndex* other_index = Fake(Tip(), other->GetHash());
    BOOST_REQUIRE(other_index->nHeight == stored_index->nHeight);
    record->BlockDisconnected(other, other_index);
    BOOST_REQUIRE(WaitUntil([&] { return record->getVisibility()->live_tip_hash == Tip()->GetBlockHash(); }));
    BOOST_CHECK(!record->getVisibility()->IsVisible(stored_index->nHeight));
    BOOST_CHECK_EQUAL(record->getStatus().rows, 2U);
    record->stop();
    const std::vector<std::vector<unsigned char>> keys = Keys(m_path);
    BOOST_CHECK_EQUAL(CountPrefix(keys, 'r'), 2U);
}

BOOST_AUTO_TEST_CASE(no_undo_read_after_flush)
{
    // After flush() the worker reads no undo data: blocks connected afterwards are written, with their coins still
    // to be looked up, which the next start does.
    std::atomic<bool> flushed{false};
    std::atomic<int> looked_up_after{0};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo& info) {
        if (flushed && info.kind == BatchInfo::Kind::PENDING) ++looked_up_after;
    };
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a = Mine({Message({OpReturn("before")})});
    Catch(*record);
    // Nothing is left to look up, so flush() finds the worker idle.
    BOOST_REQUIRE(WaitUntil([&] {
        RowView row;
        return record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    record->flush();
    flushed = true;
    const CBlockIndex* b = Mine({Message({OpReturn("after")})});
    Catch(*record, b);
    std::this_thread::sleep_for(Ms{300});
    BOOST_CHECK_EQUAL(looked_up_after.load(), 0);
    RowView row;
    BOOST_REQUIRE(record->readRow({b->nHeight, 0}, row, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::PENDING);
    record->stop();
    record = Start();
    BOOST_CHECK(WaitUntil([&] {
        RowView again;
        return record->readRow({b->nHeight, 0}, again, nullptr) == ReadResult::FOUND && again.tx.paid_by_kind == PaidByKind::SCRIPT;
    }));
    record->stop();
}

BOOST_AUTO_TEST_CASE(heights_near_max)
{
    // A snapshot, made by a caller, whose tip is the largest height: reads in both orders find the rows below it.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({OpReturn("a"), OpReturn("b")})});
    Catch(*record);
    auto v = std::make_shared<Visibility>(*record->getVisibility());
    v->coverage = std::make_shared<const std::vector<HeightRange>>(std::vector<HeightRange>{{a->nHeight, std::numeric_limits<int32_t>::max()}});
    v->live_tip = std::numeric_limits<int32_t>::max();
    for (const bool older : {false, true}) {
        RowRequest request;
        request.older = older;
        request.from = older ? ROW_KEY_AFTER_ALL : ROW_KEY_BEFORE_ALL;
        request.visibility = v;
        const RowBatch batch = record->readRows(request);
        BOOST_CHECK_EQUAL(batch.rows.size(), 2U);
        BOOST_CHECK(!batch.failed && batch.end);
        request.from = older ? RowKey{std::numeric_limits<int32_t>::max(), 0} : RowKey{a->nHeight, 1};
        BOOST_CHECK(!record->readRows(request).failed);
    }
    RowView row;
    BOOST_CHECK(record->readRow({std::numeric_limits<int32_t>::max(), 0}, row, v) == ReadResult::NOT_FOUND);
    record->stop();
}

BOOST_AUTO_TEST_CASE(spent_scripts_kept_apart)
{
    // A transaction spends coins whose scripts are 68, 10,000, 67, 10,000 and 10,000 bytes long, and pays change to a
    // script it spends. Its record holds the coin of 67 bytes whole and the others without their scripts, which have
    // keys of their own, so that the record stays within three times the size of the transaction. readTransaction
    // gives the scripts back in input order while they fit its budget, and the keys go with the height.
    const std::vector<size_t> sizes{SPENT_SCRIPT_INLINE_SIZE + 1, MAX_SCRIPT_SIZE, SPENT_SCRIPT_INLINE_SIZE, MAX_SCRIPT_SIZE, MAX_SCRIPT_SIZE};
    const TestCoin coin = Take();
    std::vector<CTxOut> funding;
    for (size_t i = 0; i < sizes.size(); ++i) funding.emplace_back(10000 + CAmount(i), SpendableScript(sizes[i], uint8_t(i)));
    funding.emplace_back(20000, CScript()); // an empty script, which a scriptSig of OP_1 spends
    funding.emplace_back(coin.value - 100000, m_script);
    const CMutableTransaction fund = Spend({coin}, funding);
    Mine({fund});
    std::shared_ptr<Record> record = Start();
    CMutableTransaction spend;
    CAmount in_value = 0;
    for (size_t i = 0; i < sizes.size(); ++i) {
        spend.vin.emplace_back(COutPoint(fund.GetHash(), uint32_t(i)));
        in_value += funding[i].nValue;
    }
    spend.vin.emplace_back(COutPoint(fund.GetHash(), uint32_t(sizes.size())), CScript() << OP_1);
    in_value += funding[sizes.size()].nValue;
    spend.vout.emplace_back(0, OpReturn("kept apart"));
    spend.vout.emplace_back(1000, funding[1].scriptPubKey); // to a script it spends: not another payee
    spend.vout.emplace_back(2000, m_script);
    const CBlockIndex* index = Mine({spend});
    const size_t tx_size = ::GetSerializeSize(spend, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    Catch(*record);
    RowView row;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::SCRIPT);
    BOOST_CHECK(row.tx.paid_by == funding[0].scriptPubKey);
    BOOST_CHECK_EQUAL(row.tx.fee, in_value - 3000);
    BOOST_CHECK_EQUAL(int{row.tx.n_other}, 1);
    BOOST_CHECK_EQUAL(row.tx.other_value, 2000);

    const TxKey key{index->nHeight, 1};
    TxView view;
    BOOST_REQUIRE(record->readTransaction(key, SCRIPT_HEAD_SIZE, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
    BOOST_REQUIRE_EQUAL(view.vin.size(), sizes.size() + 1);
    for (size_t i = 0; i < sizes.size(); ++i) {
        BOOST_CHECK(view.vin[i].spent_state == SpentState::KNOWN);
        BOOST_CHECK(view.vin[i].spent.out == funding[i]);
        BOOST_CHECK_EQUAL(view.vin[i].spent_script_size, sizes[i]);
    }
    // The coin of the empty script: known, whole, of size 0, and so not one whose script was left out.
    BOOST_CHECK(view.vin.back().spent_state == SpentState::KNOWN);
    BOOST_CHECK(view.vin.back().spent.out == funding[sizes.size()]);
    BOOST_CHECK_EQUAL(view.vin.back().spent_script_size, 0U);
    BOOST_CHECK_EQUAL(view.fee, in_value - 3000);
    // A budget for the first two scripts kept apart but one byte: the first is read, and nothing after the second.
    BOOST_REQUIRE(record->readTransaction(key, 0, sizes[0] + sizes[1] - 1, view, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(view.vin[0].spent.out == funding[0]);
    for (const size_t i : {size_t{1}, size_t{3}, size_t{4}}) {
        BOOST_CHECK(view.vin[i].spent.out.scriptPubKey.empty());
        BOOST_CHECK_EQUAL(view.vin[i].spent.out.nValue, funding[i].nValue);
        BOOST_CHECK_EQUAL(view.vin[i].spent_script_size, sizes[i]);
    }
    BOOST_CHECK(view.vin[2].spent.out == funding[2]); // held by the record
    BOOST_REQUIRE(record->readTransaction(key, SCRIPT_HEAD_SIZE, 0, view, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(view.vin[0].spent.out.scriptPubKey.empty() && view.vin[2].spent.out == funding[2]);
    BOOST_CHECK(view.vin.back().spent.out == funding[sizes.size()]);
    CMutableTransaction rebuilt;
    BOOST_CHECK(view.ToMutableTransaction(rebuilt) && rebuilt.GetHash() == spend.GetHash());
    record->stop();

    const HeightBytes stored = BytesAt(m_path, index->nHeight);
    BOOST_CHECK_EQUAL(stored.keys.at(prefix::SPENT), 4U); // neither the 67-byte script nor the empty one
    BOOST_CHECK_EQUAL(stored.largest.at(prefix::SPENT), MAX_SCRIPT_SIZE + 3U);
    // The record of the transaction: its own bytes and, for each input, at most 83 more.
    BOOST_CHECK(stored.largest.at(prefix::TX) <= tx_size + 83 * spend.vin.size() + 2 * spend.vout.size() + 80);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BlockMeta meta;
        BOOST_REQUIRE(db.Read(HeightKey(prefix::BLOCK, index->nHeight), meta));
        BOOST_CHECK_EQUAL(meta.logical_bytes, stored.total);
    }

    // A spent script of another size than its input says, and a lost one, are read errors for a read that needs them;
    // a read that does not need them finds the transaction.
    for (const bool lose : {false, true}) {
        {
            CDBWrapper db(m_path, 1 << 20, false, false, true);
            if (lose) {
                db.Erase(SpentDbKey(key, 1), true);
            } else {
                SpentScript shorter;
                shorter.script = SpendableScript(MAX_SCRIPT_SIZE - 1, 1);
                db.Write(SpentDbKey(key, 1), shorter, true);
            }
        }
        record = Start();
        BOOST_CHECK(record->readTransaction(key, 0, sizes[0], view, nullptr) == ReadResult::FOUND);
        BOOST_CHECK(record->readTransaction(key, 0, sizes[0] + sizes[1], view, nullptr) == ReadResult::FAILED);
        // The worker makes the same read, with the same budget, and fails too.
        BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
        BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
        record->stop();
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        SpentScript whole;
        whole.script = funding[1].scriptPubKey;
        db.Write(SpentDbKey(key, 1), whole, true);
    }

    // Disconnected, the height loses every key, its spent scripts too; a reader that holds a snapshot from before
    // finds nothing, and no read error.
    record = Start();
    BOOST_REQUIRE(record->readTransaction(key, 0, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
    const VisibilityRef before = record->getVisibility();
    Invalidate(index);
    Catch(*record, index->pprev);
    const uint64_t errors = record->getStatus().read_errors;
    BOOST_CHECK(record->readTransaction(key, 0, ALL_SPENT, view, before) == ReadResult::NOT_FOUND);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, errors);
    BOOST_CHECK(record->getRemovals() != before->removals);
    record->stop();
    BOOST_CHECK_EQUAL(BytesAt(m_path, index->nHeight).total, 0U);
    Reconsider(index);
}

BOOST_AUTO_TEST_CASE(spent_scripts_in_bounded_batches)
{
    // The spent scripts of one block, in three transactions, come to more than SPENT_BATCH_BYTES, so they are written
    // in more than one batch of about that size, which do not end at a transaction's end, before the batch that holds
    // the transaction records. A crash between two of them leaves the height pending; the next start writes them all
    // again, and the height's logical bytes count every key once.
    const size_t inputs = SPENT_BATCH_BYTES / MAX_SCRIPT_SIZE + 12;
    TestCoin coin = Take();
    std::vector<COutPoint> spent;
    std::vector<CTxOut> outputs_spent;
    while (spent.size() < inputs) {
        std::vector<CTxOut> outputs;
        const size_t n = std::min<size_t>(99, inputs - spent.size());
        for (size_t i = 0; i < n; ++i) outputs.emplace_back(1000, SpendableScript(MAX_SCRIPT_SIZE, uint8_t(spent.size() + i)));
        outputs.emplace_back(coin.value - CAmount(1000 * n), m_script);
        const CMutableTransaction tx = Spend({coin}, outputs);
        Mine({tx});
        for (size_t i = 0; i < n; ++i) {
            spent.emplace_back(tx.GetHash(), uint32_t(i));
            outputs_spent.push_back(outputs[i]);
        }
        coin = {COutPoint(tx.GetHash(), uint32_t(n)), outputs.back().nValue};
    }
    // Inputs 0 to 299, 300 to 599 and the rest, each transaction with a row and its change.
    const std::vector<size_t> firsts{0, 300, 600, inputs};
    std::vector<CMutableTransaction> spends(3);
    for (size_t t = 0; t < 3; ++t) {
        for (size_t i = firsts[t]; i < firsts[t + 1]; ++i) spends[t].vin.emplace_back(spent[i]);
        spends[t].vout.emplace_back(0, OpReturn(strprintf("many %u", t)));
        spends[t].vout.emplace_back(CAmount(1000 * (firsts[t + 1] - firsts[t])) - 10000, m_script);
    }

    std::atomic<int> pending_batches{0};
    std::atomic<size_t> largest{0};
    RecordTestHooks crashing;
    crashing.before_write = [&](const BatchInfo& info) {
        if (info.kind != BatchInfo::Kind::PENDING) return;
        if (++pending_batches == 2) throw TestCrash();
    };
    std::shared_ptr<Record> record = Start(crashing);
    const CBlockIndex* index = Mine(spends);
    Catch(*record);
    BOOST_REQUIRE(WaitUntil([&] { return pending_batches.load() >= 2; }));
    record->stop();
    HeightBytes stored = BytesAt(m_path, index->nHeight);
    BOOST_CHECK_EQUAL(stored.keys.count(prefix::PENDING), 1U);
    BOOST_CHECK(stored.keys.count(prefix::SPENT) && stored.keys.at(prefix::SPENT) < inputs);

    pending_batches = 0;
    RecordTestHooks counting;
    counting.before_write = [&](const BatchInfo& info) {
        if (info.kind != BatchInfo::Kind::PENDING) return;
        ++pending_batches;
        largest = std::max(largest.load(), info.bytes);
    };
    record = Start(counting);
    RowView row;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    BOOST_CHECK(pending_batches.load() >= 3);
    BOOST_CHECK(largest.load() <= SPENT_BATCH_BYTES + MAX_SCRIPT_SIZE + 100);
    for (size_t t = 0; t < 3; ++t) {
        TxView view;
        BOOST_REQUIRE(record->readTransaction({index->nHeight, uint32_t(t + 1)}, 0, ALL_SPENT, view, nullptr) == ReadResult::FOUND);
        BOOST_REQUIRE_EQUAL(view.vin.size(), firsts[t + 1] - firsts[t]);
        for (size_t i = 0; i < view.vin.size(); ++i) BOOST_CHECK(view.vin[i].spent.out == outputs_spent[firsts[t] + i]);
        BOOST_CHECK(view.txid == spends[t].GetHash());
    }
    record->stop();
    stored = BytesAt(m_path, index->nHeight);
    BOOST_CHECK_EQUAL(stored.keys.count(prefix::PENDING), 0U);
    BOOST_CHECK_EQUAL(stored.keys.at(prefix::SPENT), inputs);
    CDBWrapper db(m_path, 1 << 20, false, false, true);
    BlockMeta meta;
    BOOST_REQUIRE(db.Read(HeightKey(prefix::BLOCK, index->nHeight), meta));
    BOOST_CHECK_EQUAL(meta.logical_bytes, stored.total);
}

BOOST_AUTO_TEST_CASE(empty_height_replaced)
{
    // The record holds b1, b2 (no rows) and b3. Then the callback of a block c2 of another branch at b2's height, with
    // rows, reaches it, as a callback queued before the record read the node's tip does at a start
    // (tip_move_rule_on_start). A snapshot taken before shows b2's height visible and empty, so the batch that writes
    // c2's rows there raises the removal counter; and the heights that stop being visible, b2's and b3's, are reported
    // removed.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* b1 = Mine({Message({OpReturn("b1")})});
    const CBlockIndex* b2 = Mine();
    const CBlockIndex* b3 = Mine({Message({OpReturn("b3")})});
    const CBlockIndex* c2 = Process(MakeBlock(b1, {Message({OpReturn("c2")})}));
    BOOST_REQUIRE(Tip() == b3);
    Catch(*record);
    const VisibilityRef v = record->getVisibility();
    BOOST_REQUIRE(v->IsVisible(b2->nHeight) && v->IsVisible(b3->nHeight));
    BOOST_REQUIRE(RowsAt(*record, b2->nHeight).empty());
    const auto block_of = [&](const CBlockIndex* index) { return WITH_LOCK(m_blocks_mutex, return m_blocks.at(index->GetBlockHash())); };
    Changes changes(*record);
    record->BlockConnected(block_of(c2), c2);
    Catch(*record, c2);
    RowRequest request;
    request.from = {b2->nHeight - 1, NO_N};
    request.heights = {b2->nHeight, b2->nHeight};
    request.visibility = v;
    const RowBatch batch = record->readRows(request);
    BOOST_CHECK_EQUAL(batch.rows.size(), 1U); // c2's row, under the snapshot of b3's branch
    BOOST_CHECK(record->getRemovals() != v->removals);
    BOOST_CHECK(WaitUntil([&] { return changes.Reported(RecordChange::ROWS_REMOVED, b2->nHeight, b3->nHeight); }));
    BOOST_CHECK(!record->getVisibility()->IsVisible(b3->nHeight));

    // The callbacks that follow bring the record back to the active chain.
    record->BlockDisconnected(block_of(c2), c2);
    record->BlockConnected(block_of(b2), b2);
    record->BlockConnected(block_of(b3), b3);
    Catch(*record, b3);
    Problems problems;
    CheckHolds(*record, record->getVisibility(), b3, problems);
    for (const std::string& problem : problems.List()) BOOST_ERROR(problem);
    BOOST_CHECK(record->getVisibility()->IsVisible(b2->nHeight) && RowsAt(*record, b2->nHeight).empty());
    record->stop();
}

BOOST_AUTO_TEST_CASE(cs_main_back_off)
{
    // The worker times its own holds of cs_main, not its wait for the lock: after another thread held cs_main for a
    // long time, the worker's next hold follows at once. After a hold of its own longer than CS_MAIN_SLICE, its next
    // hold waits twice as long; meanwhile it writes the blocks that connect, and stop() does not wait for it.
    LogInstance().EnableCategory(BCLog::BENCH);
    LogLines log;
    auto hold = std::make_shared<WorkerHold>();
    std::atomic<int> holds{0};
    std::atomic<int> slow{0}; // holds to make long
    Mutex times_mutex;
    std::vector<std::pair<Clock::time_point, Clock::time_point>> times; // of each hold: its start and its end
    RecordTestHooks hooks;
    hooks.hold = hold;
    hooks.in_cs_main = [&] {
        const Clock::time_point begun = Clock::now();
        if (slow > 0) {
            --slow;
            std::this_thread::sleep_for(Ms{1000});
        }
        LOCK(times_mutex);
        times.emplace_back(begun, Clock::now());
        ++holds;
    };
    const auto hold_times = [&](size_t i) { return WITH_LOCK(times_mutex, return times.at(i)); };
    std::shared_ptr<Record> record = Start(hooks);
    Catch(*record);
    hold->Close();
    BOOST_REQUIRE(hold->WaitUntilHeld(Ms{20000}));
    Mine({Message({OpReturn("a")})});
    const CBlockIndex* b = Mine({Message({OpReturn("b")})});
    SyncWithValidationInterfaceQueue();

    // Another thread holds cs_main for 1.5 s while the worker waits for it to look up the coins of the newest block.
    // (Nothing here may take cs_main meanwhile: the record's tip is watched through its snapshot.)
    std::promise<void> locked;
    std::promise<void> release;
    std::thread holder([&] {
        LOCK(cs_main);
        locked.set_value();
        release.get_future().wait();
    });
    locked.get_future().wait();
    hold->Open();
    BOOST_CHECK(WaitUntil([&] { return record->getVisibility()->live_tip_hash == b->GetBlockHash(); }));
    std::this_thread::sleep_for(Ms{1500});
    release.set_value();
    holder.join();
    BOOST_REQUIRE(WaitUntil([&] { return holds.load() >= 2; }));
    BOOST_CHECK(hold_times(1).first - hold_times(0).second < Ms{1500}); // the wait does not count as a hold
    BOOST_CHECK(log.Containing("cs_main held").empty());

    // Now a hold of the worker's own lasts 1 s: its next hold waits 2 s, and a block connected meanwhile is written.
    slow = 1;
    Mine({Message({OpReturn("c")})});
    Mine({Message({OpReturn("d")})});
    BOOST_REQUIRE(WaitUntil([&] { return holds.load() >= 3; }));
    const CBlockIndex* e = Mine({Message({OpReturn("e")})});
    BOOST_CHECK(WaitUntil([&] { return record->getVisibility()->live_tip_hash == e->GetBlockHash(); }, Ms{1500}));
    BOOST_CHECK_EQUAL(holds.load(), 3);
    BOOST_REQUIRE(WaitUntil([&] { return holds.load() >= 5; }));
    BOOST_CHECK(hold_times(3).first - hold_times(2).second >= Ms{1990});
    BOOST_CHECK_EQUAL(log.Containing("cs_main held").size(), 1U);

    // And while the worker waits out another back-off with coins still to look up, stop() does not wait for it.
    slow = 1;
    const int before = holds.load();
    Mine({Message({OpReturn("f")})});
    Mine({Message({OpReturn("g")})});
    BOOST_REQUIRE(WaitUntil([&] { return holds.load() > before; }));
    std::this_thread::sleep_for(Ms{300}); // the worker is in the 2 s back-off now
    const Clock::time_point stop_begun = Clock::now();
    record->stop();
    BOOST_CHECK(Clock::now() - stop_begun < Ms{1000});
    LogInstance().DisableCategory(BCLog::BENCH);
}

BOOST_AUTO_TEST_CASE(rebuild_survives_failing_old_database)
{
    // A rebuild is asked while the record runs, and in the step that takes the request the worker's confirmation of a
    // reader's failed read fails on the old database too. The record is built again all the same.
    auto hold = std::make_shared<WorkerHold>();
    std::atomic<bool> fail_rows{false};
    std::atomic<bool> fail_first{false};
    std::atomic<int> first_failures{0};
    RecordTestHooks hooks;
    hooks.hold = hold;
    hooks.on_read = [&](uint8_t prefix_byte, bool) {
        if (fail_rows && prefix_byte == prefix::ROW) throw dbwrapper_error("a read that fails");
    };
    hooks.before_write = [&](const BatchInfo& info) {
        if (fail_first && info.kind == BatchInfo::Kind::FIRST) {
            ++first_failures;
            throw dbwrapper_error("a write that fails");
        }
    };
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    BOOST_REQUIRE_EQUAL(record->getStatus().rows, 1U);
    hold->Close();
    BOOST_REQUIRE(hold->WaitUntilHeld(Ms{20000}));
    fail_rows = true;
    RowView row;
    BOOST_CHECK(record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FAILED);
    record->requestRebuild();
    hold->Open();
    BOOST_CHECK(WaitUntil([&] {
        const RecordStatus status = record->getStatus();
        return (status.state == State::BACKFILLING || status.state == State::RUNNING) && status.rows == 0;
    }));
    fail_rows = false;
    const CBlockIndex* b = Mine({Message({OpReturn("b")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(RowsAt(*record, b->nHeight).size(), 1U);
    BOOST_CHECK(RowsAt(*record, a->nHeight).empty());

    // A rebuild whose own first write fails leaves the record failed, once: it does not build itself again.
    fail_first = true;
    record->requestRebuild();
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    std::this_thread::sleep_for(Ms{500});
    BOOST_CHECK_EQUAL(first_failures.load(), 1);
    BOOST_CHECK(record->getStatus().reason == FailReason::WRITE_FAILED);

    // From there, with writes working again, a rebuild works; and from the same state a stop closes the database.
    fail_first = false;
    record->requestRebuild();
    BOOST_CHECK(WaitUntil([&] {
        const State state = record->getStatus().state;
        return state == State::BACKFILLING || state == State::RUNNING;
    }));
    const CBlockIndex* d = Mine({Message({OpReturn("d")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(RowsAt(*record, d->nHeight).size(), 1U);
    fail_first = true;
    record->requestRebuild();
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    record->stop();
    BOOST_CHECK(WaitUntilClosed(m_path));
}

BOOST_AUTO_TEST_CASE(wait_follows_state_and_chain)
{
    // A wait for the tip ends as soon as the record leaves RUNNING and BACKFILLING: when it is switched off, and when
    // it stops while its last write is slow. And when the node leaves the block a wait began with, the wait is for
    // the node's best block from then on: it ends when the record has that one, and notes no stall.
    std::atomic<bool> slow_last{false};
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.before_write = [&](const BatchInfo& info) {
        if (slow_last && info.kind == BatchInfo::Kind::LAST) std::this_thread::sleep_for(Ms{2000});
    };
    std::shared_ptr<Record> record = Start(hooks);
    Catch(*record);
    const auto wait_for_a_block_never_written = [&](const uint256& hash) {
        // The node reports a best block that the record is never given.
        uiInterface.NotifyBlockTip(SynchronizationState::POST_INIT, Fake(Tip(), hash));
        return std::async(std::launch::async, [&] { return record->waitForTip(10000); });
    };

    std::future<bool> waiting = wait_for_a_block_never_written(uint256S("f1"));
    BOOST_CHECK(waiting.wait_for(Ms{300}) == std::future_status::timeout);
    Clock::time_point begun = Clock::now();
    record->setRecordEnabled(false);
    BOOST_CHECK(!waiting.get());
    BOOST_CHECK(Clock::now() - begun < Ms{3000});
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::OFF; }));

    record->setRecordEnabled(true);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING; }));
    // The real best block again, then the record's tip is caught up with it.
    uiInterface.NotifyBlockTip(SynchronizationState::POST_INIT, Tip());
    BOOST_REQUIRE(record->waitForTip(10000));
    const CBlockIndex* p = Tip();
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{5000}));
    // The node moves on to x, which the held record has not written; a wait begins with x as its target, and then
    // the node leaves x again. The record's tip is the node's best block once more, without the record doing
    // anything.
    const CBlockIndex* x = Mine({Message({OpReturn("x")})});
    waiting = std::async(std::launch::async, [&] { return record->waitForTip(5000); });
    BOOST_CHECK(waiting.wait_for(Ms{300}) == std::future_status::timeout);
    begun = Clock::now();
    Invalidate(x);
    BOOST_CHECK(Tip() == p);
    BOOST_CHECK(waiting.get());
    BOOST_CHECK(Clock::now() - begun < Ms{3000});
    hooks.hold->Open();
    Reconsider(x);
    Catch(*record);

    // Stopping while the last write is slow.
    slow_last = true;
    waiting = wait_for_a_block_never_written(uint256S("f2"));
    BOOST_CHECK(waiting.wait_for(Ms{300}) == std::future_status::timeout);
    std::thread stopping([&] { record->stop(); });
    begun = Clock::now();
    BOOST_CHECK(!waiting.get());
    BOOST_CHECK(Clock::now() - begun < Ms{1500});
    stopping.join();
}

BOOST_AUTO_TEST_CASE(rebuild_hides_new_database)
{
    // While a rebuild has opened its new database and not yet written its first batch, the database holds no end key.
    // A reader with a snapshot from before the rebuild does not find it then: it gets nothing, and no read error.
    std::atomic<bool> armed{false};
    std::promise<void> in_first;
    std::promise<void> go;
    std::shared_future<void> released = go.get_future().share();
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo& info) {
        if (info.kind == BatchInfo::Kind::FIRST && armed.exchange(false)) {
            in_first.set_value();
            released.wait();
        }
    };
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    const VisibilityRef v = record->getVisibility();
    BOOST_REQUIRE(v->IsVisible(a->nHeight));
    armed = true;
    record->requestRebuild();
    in_first.get_future().wait();
    const uint64_t errors = record->getStatus().read_errors;
    RowRequest request;
    request.visibility = v;
    const RowBatch batch = record->readRows(request);
    BOOST_CHECK(!batch.failed);
    BOOST_CHECK(batch.rows.empty());
    request.older = true;
    request.from = ROW_KEY_AFTER_ALL;
    BOOST_CHECK(!record->readRows(request).failed);
    RowView row;
    BOOST_CHECK(record->readRow({a->nHeight, 0}, row, v) == ReadResult::NOT_FOUND);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, errors);
    BOOST_CHECK(record->getRemovals() != v->removals);
    go.set_value();
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING; }));
    BOOST_CHECK(!record->readRows(RowRequest()).failed);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, errors);
    record->stop();
}

BOOST_AUTO_TEST_CASE(case_b_stop_before_a_block)
{
    // Start-up case B (the node is behind the record's tip, on its branch), then a stop before any block connects:
    // the last batch keeps the stored tip locator. So the next start, after the node has connected those blocks again
    // while the record was stopped, still covers the heights above the node's tip of then.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    const CBlockIndex* b = Mine({Message({OpReturn("b")})});
    const CBlockIndex* c = Mine({Message({OpReturn("c")})});
    Catch(*record);
    record->stop();
    Invalidate(b);
    BOOST_REQUIRE(Tip() == a);
    record = Start();
    BOOST_CHECK(!record->getVisibility()->IsVisible(c->nHeight));
    record->stop();
    Reconsider(b);
    BOOST_REQUIRE(Tip() == c);
    record = Start();
    BOOST_CHECK(record->getVisibility()->IsVisible(b->nHeight));
    BOOST_CHECK(record->getVisibility()->IsVisible(c->nHeight));
    BOOST_CHECK_EQUAL(RowsAt(*record, c->nHeight).size(), 1U);
    record->stop();
}

BOOST_AUTO_TEST_CASE(queue_bytes_count_the_copy)
{
    // What the queue's bound counts of a block is at least what the record's copy of it holds: its rows, the inputs
    // and outputs of its transaction records and the rows' script bytes. A block of many tiny rows, or of many inputs
    // or outputs, takes several times its own size so.
    const CScript p2pkh = CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 7) << OP_EQUALVERIFY << OP_CHECKSIG;
    const auto check = [&](std::vector<CTxOut> outputs, size_t inputs) {
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].scriptSig = CScript() << OP_0 << OP_0;
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        CMutableTransaction tx;
        for (size_t i = 0; i < inputs; ++i) tx.vin.emplace_back(COutPoint(InsecureRand256(), 0));
        tx.vout = std::move(outputs);
        CBlock block;
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.push_back(MakeTransactionRef(tx));
        const std::shared_ptr<BlockRows> rows = ReadBlockRows(block, block.GetHash());
        BOOST_REQUIRE(rows);
        size_t held = rows->rows.size() * (sizeof(Row) + sizeof(std::vector<unsigned char>));
        for (const Row& row : rows->rows) held += row.head.size();
        for (const auto& record : rows->txs) {
            held += record.second.vin.size() * sizeof(TxInRecord) + record.second.vout.size() * sizeof(TxOutRecord);
        }
        BOOST_CHECK_MESSAGE(rows->bytes >= held, "counted " << rows->bytes << ", held " << held);
        return rows->bytes;
    };
    check(std::vector<CTxOut>(99980, CTxOut(0, CScript() << OP_RETURN)), 1);
    check({CTxOut(0, CScript() << OP_RETURN)}, 24390);
    std::vector<CTxOut> payees(34000, CTxOut(1000, p2pkh));
    payees.emplace_back(0, CScript() << OP_RETURN);
    check(payees, 1);
    // An ordinary message is counted at a few hundred bytes, not more.
    BOOST_CHECK(check({CTxOut(0, OpReturn("hello")), CTxOut(100000, p2pkh)}, 1) < 4096);
}

BOOST_AUTO_TEST_CASE(paying_scripts_count_in_budget)
{
    // Every row read carries its transaction's paying script, which can be 10,000 bytes long: a batch's byte budget
    // counts it, in both orders, so that a batch holds at most that many bytes of scripts (or one row).
    const TestCoin coin = Take();
    const CScript paying = SpendableScript(MAX_SCRIPT_SIZE, 7);
    const CMutableTransaction fund = Spend({coin}, {CTxOut(100000, paying), CTxOut(coin.value - 200000, m_script)});
    Mine({fund});
    std::shared_ptr<Record> record = Start();
    CMutableTransaction spend;
    spend.vin.emplace_back(COutPoint(fund.GetHash(), 0));
    for (int i = 0; i < 200; ++i) spend.vout.emplace_back(0, OpReturn(strprintf("r%d", i)));
    spend.vout.emplace_back(90000, m_script);
    const CBlockIndex* index = Mine({spend});
    Catch(*record);
    RowView row;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({index->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind == PaidByKind::SCRIPT;
    }));
    BOOST_REQUIRE(row.tx.paid_by == paying);
    for (const bool older : {false, true}) {
        RowRequest request;
        request.older = older;
        request.from = older ? ROW_KEY_AFTER_ALL : RowKey{index->nHeight - 1, NO_N};
        request.heights = {index->nHeight, index->nHeight};
        request.max_rows = 1000;
        request.max_bytes = 100000;
        const RowBatch batch = record->readRows(request);
        BOOST_REQUIRE(!batch.failed && !batch.rows.empty());
        size_t carried = 0;
        for (const RowView& view : batch.rows) carried += view.script_prefix.size() + view.tx.paid_by.size();
        BOOST_CHECK_MESSAGE(carried <= request.max_bytes, "carried " << carried << " in " << batch.rows.size() << " rows");
        BOOST_CHECK(!batch.end);
        request.max_bytes = 1;
        BOOST_CHECK_EQUAL(record->readRows(request).rows.size(), 1U); // a batch holds at least one row
    }
    record->stop();
}

BOOST_AUTO_TEST_CASE(stored_flags_and_unreadable_kept)
{
    // Bits of the record's flags that this program does not know are not read further, and stay as they were: the
    // record opens and runs. The heights it could not read stay listed, as ranges, until they are covered, up to
    // MAX_UNREADABLE_RANGES of them; the live path adds none.
    std::shared_ptr<Record> record = Start();
    Mine({Message({OpReturn("a")})});
    Catch(*record);
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        RecordMeta meta;
        BOOST_REQUIRE(db.Read(uint8_t{prefix::META}, meta));
        meta.flags = 0xffffffff;
        BOOST_REQUIRE(db.Write(uint8_t{prefix::META}, meta));
        StoredCoverage stored;
        BOOST_REQUIRE(db.Read(uint8_t{prefix::COVERAGE}, stored));
        // The next block's height and the one after it, single heights far above, and one long stretch.
        stored.unreadable.push_back({Tip()->nHeight + 1, Tip()->nHeight + 2});
        for (int32_t i = 0; i < int32_t(MAX_UNREADABLE_RANGES) - 2; ++i) stored.unreadable.push_back({10000 + 3 * i, 10000 + 3 * i});
        stored.unreadable.push_back({20000, 69999});
        BOOST_REQUIRE(db.Write(uint8_t{prefix::COVERAGE}, stored));
    }
    record = Start();
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    const CBlockIndex* b = Mine({Message({OpReturn("b")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(RowsAt(*record, b->nHeight).size(), 1U);
    record->stop();
    CDBWrapper db(m_path, 1 << 20, false, false, true);
    RecordMeta meta;
    BOOST_REQUIRE(db.Read(uint8_t{prefix::META}, meta));
    BOOST_CHECK_EQUAL(meta.flags, 0xffffffffU);
    StoredCoverage stored;
    BOOST_REQUIRE(db.Read(uint8_t{prefix::COVERAGE}, stored));
    // The height b covers has left the list; the rest stays as it was.
    BOOST_CHECK_EQUAL(stored.unreadable.size(), MAX_UNREADABLE_RANGES);
    BOOST_CHECK(stored.unreadable.front() == HeightRange({b->nHeight + 1, b->nHeight + 1}));
    BOOST_CHECK(stored.unreadable.back() == HeightRange({20000, 69999}));
}

BOOST_AUTO_TEST_CASE(read_block_and_proof)
{
    // readBlock gives a block with rows: its facts and a proof of its transactions with rows against its merkle root.
    // A covered block without rows, a height not covered and a stopped record give nothing; a lost proof is a read
    // error, which the worker's own read confirms.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({OpReturn("a1")}), Message({OpReturn("a2"), OpReturn("a3")})});
    const CBlockIndex* empty = Mine();
    Catch(*record);
    const std::shared_ptr<const CBlock> block = WITH_LOCK(m_blocks_mutex, return m_blocks.at(a->GetBlockHash()));
    BlockView view;
    BOOST_REQUIRE(record->readBlock(a->nHeight, view, nullptr) == ReadResult::FOUND);
    BOOST_CHECK_EQUAL(view.height, a->nHeight);
    BOOST_CHECK(view.hash == a->GetBlockHash());
    BOOST_CHECK_EQUAL(view.time, block->nTime);
    BOOST_CHECK_EQUAL(view.ntx, block->vtx.size());
    BOOST_CHECK_EQUAL(view.nrows, 3U);
    std::vector<uint256> matches;
    std::vector<unsigned int> positions;
    BOOST_CHECK(view.proof.ExtractMatches(matches, positions) == block->hashMerkleRoot);
    BOOST_CHECK(positions == std::vector<unsigned int>({1, 2}));
    BOOST_CHECK(matches == std::vector<uint256>({block->vtx[1]->GetHash(), block->vtx[2]->GetHash()}));
    BOOST_CHECK(record->readBlock(empty->nHeight, view, nullptr) == ReadResult::NOT_FOUND);
    BOOST_CHECK(record->readBlock(a->nHeight - 5, view, nullptr) == ReadResult::NOT_FOUND); // a hole
    record->stop();
    BOOST_CHECK(record->readBlock(a->nHeight, view, nullptr) == ReadResult::NOT_FOUND);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(HeightKey(prefix::PROOF, a->nHeight)));
    }
    record = Start();
    BOOST_CHECK(record->readBlock(a->nHeight, view, nullptr) == ReadResult::FAILED);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::READ_FAILED);
    record->stop();
}

BOOST_AUTO_TEST_CASE(find_transaction_respects_visibility)
{
    // findTransaction finds a transaction only at a height the snapshot shows: after the node went back below it while
    // the record was stopped (start-up case B), its location is still stored and it is not found. A location that
    // cannot be read is a read error, which the worker's own read confirms.
    std::shared_ptr<Record> record = Start();
    const CMutableTransaction tx = Message({OpReturn("found")});
    const CBlockIndex* a = Mine({tx});
    Mine({Message({OpReturn("after")})});
    Catch(*record);
    TxKey key;
    BOOST_REQUIRE(record->findTransaction(tx.GetHash(), key, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(key == TxKey({a->nHeight, 1}));
    BOOST_CHECK(record->findTransaction(InsecureRand256(), key, nullptr) == ReadResult::NOT_FOUND);
    record->stop();
    Invalidate(a);
    record = Start();
    BOOST_CHECK(record->findTransaction(tx.GetHash(), key, nullptr) == ReadResult::NOT_FOUND);
    record->stop();
    Reconsider(a);
    {
        struct Short {
            void Serialize(CDataStream& s) const { s << uint8_t{1}; }
        };
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Write(TxidDbKey(tx.GetHash()), Short()));
    }
    record = Start();
    BOOST_CHECK(record->findTransaction(tx.GetHash(), key, nullptr) == ReadResult::FAILED);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    record->stop();
}

BOOST_AUTO_TEST_CASE(read_row_non_first_brief)
{
    // A row that is not its transaction's first takes the brief from that first row; without the first row it cannot
    // be read, a read error that the worker's own read confirms.
    std::shared_ptr<Record> record = Start();
    const CMutableTransaction tx = Message({OpReturn("zero"), OpReturn("one"), OpReturn("two")});
    const CBlockIndex* a = Mine({tx});
    Catch(*record);
    RowView first;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({a->nHeight, 0}, first, nullptr) == ReadResult::FOUND && first.tx.paid_by_kind == PaidByKind::SCRIPT;
    }));
    RowView third;
    BOOST_REQUIRE(record->readRow({a->nHeight, 2}, third, nullptr) == ReadResult::FOUND);
    BOOST_CHECK(third.tx.txid == tx.GetHash());
    BOOST_CHECK_EQUAL(third.tx.n_rows, 3U);
    BOOST_CHECK(third.tx.paid_by_kind == PaidByKind::SCRIPT && third.tx.paid_by == first.tx.paid_by);
    BOOST_CHECK_EQUAL(third.tx.fee, first.tx.fee);
    BOOST_CHECK_EQUAL(third.vout, 2U);
    record->stop();
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BOOST_REQUIRE(db.Erase(RowDbKey(RowKey{a->nHeight, 0})));
    }
    record = Start();
    BOOST_CHECK(record->readRow({a->nHeight, 2}, third, nullptr) == ReadResult::FAILED);
    BOOST_CHECK(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    record->stop();
}

BOOST_AUTO_TEST_CASE(failed_while_running_then_rebuilt)
{
    // A record that fails while it runs lets go of its registration: the next block reaches none of its callbacks.
    // Built again, it registers again and records blocks.
    std::atomic<bool> fail_writes{false};
    std::atomic<int> callbacks{0};
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo& info) {
        if (fail_writes && info.kind == BatchInfo::Kind::ITEMS) throw dbwrapper_error("a write that fails");
    };
    hooks.in_block_connected = [&] { ++callbacks; };
    std::shared_ptr<Record> record = Start(hooks);
    Mine({Message({OpReturn("a")})});
    Catch(*record);
    fail_writes = true;
    Mine({Message({OpReturn("b")})});
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::FAILED; }));
    BOOST_CHECK(record->getStatus().reason == FailReason::WRITE_FAILED);
    const int before = callbacks.load();
    Mine();
    SyncWithValidationInterfaceQueue();
    BOOST_CHECK_EQUAL(callbacks.load(), before);
    fail_writes = false;
    record->requestRebuild();
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING; }));
    const CBlockIndex* c = Mine({Message({OpReturn("c")})});
    Catch(*record);
    BOOST_CHECK_EQUAL(RowsAt(*record, c->nHeight).size(), 1U);
    record->stop();
}

BOOST_AUTO_TEST_CASE(stop_is_final)
{
    // After stop(), a second stop(), a switch on, a rebuild and a block change nothing: the record stays STOPPED, holds
    // its database closed, and finds nothing.
    std::shared_ptr<Record> record = Start();
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    record->stop();
    record->stop();
    record->setRecordEnabled(true);
    record->requestRebuild();
    record->BlockConnected(WITH_LOCK(m_blocks_mutex, return m_blocks.at(a->GetBlockHash())), a);
    BOOST_CHECK(WaitUntilClosed(m_path));
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
    RowView row;
    BOOST_CHECK(record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::NOT_FOUND);
    BOOST_CHECK(!record->waitForTip(100));
    BOOST_CHECK(record->getStatus().state == State::STOPPED);
}

BOOST_AUTO_TEST_CASE(confirmations_bounded)
{
    // At most 64 failed reads wait for the worker's confirmation: a hundred failed reads are all counted, and the
    // worker makes 64 of them again.
    std::atomic<bool> fail_readers{false};
    std::atomic<int> worker_row_reads{0};
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.on_read = [&](uint8_t prefix_byte, bool worker) {
        if (prefix_byte != prefix::ROW) return;
        if (worker) {
            ++worker_row_reads;
        } else if (fail_readers) {
            throw dbwrapper_error("a read that fails");
        }
    };
    std::shared_ptr<Record> record = Start(hooks);
    const CBlockIndex* a = Mine({Message({OpReturn("a")})});
    Catch(*record);
    RowView row;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{20000}));
    worker_row_reads = 0;
    fail_readers = true;
    const uint64_t errors = record->getStatus().read_errors;
    for (int i = 0; i < 100; ++i) BOOST_CHECK(record->readRow({a->nHeight, 0}, row, nullptr) == ReadResult::FAILED);
    BOOST_CHECK_EQUAL(record->getStatus().read_errors, errors + 100);
    hooks.hold->Open();
    BOOST_REQUIRE(WaitUntil([&] { return worker_row_reads.load() >= 64; }));
    // Once the worker is back at its hold it has made every confirmation it was given.
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{20000}));
    BOOST_CHECK_EQUAL(worker_row_reads.load(), 64);
    BOOST_CHECK(record->getStatus().state == State::BACKFILLING);
    hooks.hold->Open();
    record->stop();
}

BOOST_AUTO_TEST_CASE(rowless_blocks_joined)
{
    // Blocks without rows are written together, at most MAX_JOINED_ITEMS in a batch.
    std::atomic<int> item_batches{0};
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.before_write = [&](const BatchInfo& info) {
        if (info.kind == BatchInfo::Kind::ITEMS) ++item_batches;
    };
    std::shared_ptr<Record> record = Start(hooks);
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{20000}));
    const std::shared_ptr<const CBlock> block = LooseBlock({CTxOut(1, CScript() << OP_TRUE)});
    const CBlockIndex* prev = Tip();
    for (size_t i = 0; i < MAX_JOINED_ITEMS + 5; ++i) {
        prev = Fake(prev, InsecureRand256());
        record->BlockConnected(block, prev);
    }
    item_batches = 0;
    hooks.hold->Open();
    Catch(*record, prev);
    BOOST_CHECK_EQUAL(item_batches.load(), 2);
    record->stop();
}

BOOST_AUTO_TEST_CASE(last_batch_synced)
{
    // Only the last batch, and at most one batch in SYNC_INTERVAL, is written with sync: within a test's few seconds,
    // the batches of blocks are not, and the last batch of a stop and of a switch off are.
    Mutex mutex;
    std::vector<std::pair<BatchInfo::Kind, bool>> batches;
    RecordTestHooks hooks;
    hooks.before_write = [&](const BatchInfo& info) {
        LOCK(mutex);
        batches.emplace_back(info.kind, info.sync);
    };
    std::shared_ptr<Record> record = Start(hooks);
    for (int i = 0; i < 3; ++i) Mine({Message({OpReturn(strprintf("s%d", i))})});
    Catch(*record);
    record->setRecordEnabled(false);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::OFF; }));
    record->setRecordEnabled(true);
    BOOST_REQUIRE(WaitUntil([&] { return record->getStatus().state == State::BACKFILLING; }));
    Mine({Message({OpReturn("t")})});
    Catch(*record);
    record->stop();
    LOCK(mutex);
    int last = 0;
    int items = 0;
    for (const auto& batch : batches) {
        if (batch.first == BatchInfo::Kind::LAST) {
            BOOST_CHECK(batch.second);
            ++last;
        } else if (batch.first == BatchInfo::Kind::ITEMS) {
            BOOST_CHECK(!batch.second);
            ++items;
        }
    }
    BOOST_CHECK_EQUAL(last, 2);
    BOOST_CHECK(items >= 4);
}

BOOST_AUTO_TEST_CASE(coins_newest_first)
{
    // The coins of the transactions of the newest pending block are looked up first.
    Mutex mutex;
    std::vector<int32_t> heights;
    std::vector<int32_t> order;
    Record* raw = nullptr;
    RecordTestHooks hooks;
    hooks.hold = std::make_shared<WorkerHold>();
    hooks.after_write = [&](const BatchInfo& info) {
        if (info.kind != BatchInfo::Kind::PENDING) return;
        LOCK(mutex);
        for (const int32_t height : heights) {
            if (std::count(order.begin(), order.end(), height)) continue;
            RowView row;
            if (raw->readRow({height, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING) {
                order.push_back(height);
            }
        }
    };
    std::shared_ptr<Record> record = Start(hooks);
    raw = record.get();
    Catch(*record);
    hooks.hold->Close();
    BOOST_REQUIRE(hooks.hold->WaitUntilHeld(Ms{20000}));
    {
        std::vector<int32_t> mined;
        for (int i = 0; i < 3; ++i) mined.push_back(Mine({Message({OpReturn(strprintf("n%d", i))})})->nHeight);
        LOCK(mutex);
        heights = mined;
    }
    SyncWithValidationInterfaceQueue();
    hooks.hold->Open();
    BOOST_REQUIRE(WaitUntil([&] { return WITH_LOCK(mutex, return order.size()) == 3; }));
    LOCK(mutex);
    BOOST_CHECK(order == std::vector<int32_t>({heights[2], heights[1], heights[0]}));
    record->stop();
}

BOOST_AUTO_TEST_CASE(status_after_stop_needs_no_index)
{
    // After stop(), getStatus() reports the node's best block as the node told it, without reading the block index
    // entry again: the node may free its entries then (the tree's test setups and the GUI's tests do).
    std::shared_ptr<Record> record = Start();
    Catch(*record);
    const uint256 hash = InsecureRand256();
    auto index = std::make_unique<CBlockIndex>();
    index->pprev = const_cast<CBlockIndex*>(Tip());
    index->nHeight = Tip()->nHeight + 1;
    index->nTime = 1234567;
    index->phashBlock = &hash;
    index->BuildSkip();
    uiInterface.NotifyBlockTip(SynchronizationState::POST_INIT, index.get());
    BOOST_REQUIRE_EQUAL(record->getStatus().node_tip, index->nHeight);
    const int32_t height = index->nHeight;
    record->stop();
    // The entry changes, as freed memory may.
    const uint256 other = InsecureRand256();
    index->nHeight = 999999;
    index->nTime = 1;
    index->phashBlock = &other;
    const RecordStatus status = record->getStatus();
    BOOST_CHECK_EQUAL(status.node_tip, height);
    BOOST_CHECK_EQUAL(status.node_tip_time, 1234567);
    BOOST_CHECK(status.node_tip_hash == hash);
    BOOST_CHECK(status.state == State::STOPPED);
    BOOST_CHECK(!record->waitForTip(100));
}

BOOST_AUTO_TEST_CASE(resolve_without_coins_keeps_scripts)
{
    // A transaction record whose first input's coin is known, with its script kept apart, and whose second is still
    // pending, filled in without undo data: the second becomes unavailable, and the brief keeps the paying script it
    // had, which the record itself does not hold.
    const CScript paying = SpendableScript(SPENT_SCRIPT_INLINE_SIZE + 33, 3);
    TxRecord tx;
    tx.txid = InsecureRand256();
    tx.tx_flags = TX_SPENT_PENDING;
    TxInRecord known;
    known.prevout = COutPoint(InsecureRand256(), 0);
    known.spent_state = SpentState::KNOWN;
    known.coin = Coin(CTxOut(5000, CScript()), 70, false, false);
    known.script_apart = uint32_t(paying.size());
    TxInRecord pending;
    pending.prevout = COutPoint(InsecureRand256(), 1);
    tx.vin = {known, pending};
    tx.vout.push_back(TxOutRecord::FromOutput(CTxOut(0, OpReturn("x")), true, 0));
    TxBrief brief;
    brief.txid = tx.txid;
    brief.paid_by_kind = PaidByKind::SCRIPT;
    brief.paid_by = paying;
    brief.n_in = 2;
    brief.n_out = 1;
    brief.n_rows = 1;
    ResolveSpentCoins(tx, brief, nullptr);
    BOOST_CHECK(tx.vin[0].spent_state == SpentState::KNOWN && tx.vin[0].script_apart == paying.size());
    BOOST_CHECK(tx.vin[1].spent_state == SpentState::UNAVAILABLE);
    BOOST_CHECK(brief.paid_by_kind == PaidByKind::SCRIPT);
    BOOST_CHECK(brief.paid_by == paying);
    BOOST_CHECK_EQUAL(brief.fee, -1);
    BOOST_CHECK(brief.n_other == N_OTHER_UNKNOWN);
    // Every coin known, but no undo data to give the scripts kept apart: the payees other than the spent scripts
    // are not counted from empty scripts.
    tx.vin[1].spent_state = SpentState::KNOWN;
    tx.vin[1].coin = Coin(CTxOut(3000, CScript() << OP_TRUE), 71, false, false);
    tx.vout.push_back(TxOutRecord::FromOutput(CTxOut(1000, paying), false, 0));
    ResolveSpentCoins(tx, brief, nullptr);
    BOOST_CHECK(brief.paid_by == paying);
    BOOST_CHECK(brief.n_other == N_OTHER_UNKNOWN);
    BOOST_CHECK_EQUAL(brief.fee, 7000);
}

BOOST_AUTO_TEST_CASE(spent_scripts_after_a_crash)
{
    // A crash after the spent scripts of a block are written and before the batch that refers to them leaves keys
    // that nothing refers to yet. Disconnected, the block's height loses them; filled in again without undo data (it
    // was pruned meanwhile), the height loses them too. Either way every key of every height is counted once.
    const TestCoin coin = Take();
    std::vector<CTxOut> funding;
    for (int i = 0; i < 6; ++i) funding.emplace_back(10000, SpendableScript(MAX_SCRIPT_SIZE, uint8_t(i)));
    funding.emplace_back(coin.value - 100000, m_script);
    const CMutableTransaction fund = Spend({coin}, funding);
    Mine({fund});
    const auto spend = [&](uint32_t first) {
        CMutableTransaction tx;
        for (uint32_t i = first; i < first + 3; ++i) tx.vin.emplace_back(COutPoint(fund.GetHash(), i));
        tx.vout.emplace_back(0, OpReturn(strprintf("after a crash %u", first)));
        tx.vout.emplace_back(20000, m_script);
        return tx;
    };
    const auto all_counted = [&]() {
        uint64_t total = 0;
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        std::unique_ptr<CDBIterator> it(db.NewIterator());
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            RawKey key;
            BOOST_REQUIRE(it->GetKey(key));
            if (key.bytes.size() > 1 && std::string("bpPrstix").find(char(key.bytes[0])) != std::string::npos) {
                total += key.bytes.size() + it->GetValueSize();
            }
        }
        Stats stats;
        BOOST_REQUIRE(db.Read(uint8_t{prefix::STATS}, stats));
        return stats.logical_bytes == total;
    };
    std::atomic<int> pending_batches{0};
    RecordTestHooks crashing;
    crashing.before_write = [&](const BatchInfo& info) {
        if (info.kind == BatchInfo::Kind::PENDING && ++pending_batches == 2) throw TestCrash();
    };
    const auto crash_in = [&](const CMutableTransaction& tx) {
        pending_batches = 0;
        std::shared_ptr<Record> record = Start(crashing);
        const CBlockIndex* index = Mine({tx});
        Catch(*record);
        BOOST_REQUIRE(WaitUntil([&] { return pending_batches.load() >= 2; }));
        record->stop();
        const HeightBytes stored = BytesAt(m_path, index->nHeight);
        BOOST_REQUIRE_EQUAL(stored.keys.count(prefix::PENDING), 1U);
        BOOST_REQUIRE_EQUAL(stored.keys.at(prefix::SPENT), 3U);
        return index;
    };

    // Disconnected before its coins are filled in again.
    const CBlockIndex* b = crash_in(spend(0));
    RecordTestHooks held;
    held.hold = std::make_shared<WorkerHold>();
    held.hold->Close();
    std::shared_ptr<Record> record = Start(held);
    BOOST_REQUIRE(held.hold->WaitUntilHeld(Ms{20000}));
    Invalidate(b);
    SyncWithValidationInterfaceQueue();
    held.hold->Open();
    Catch(*record, b->pprev);
    record->stop();
    BOOST_CHECK_EQUAL(BytesAt(m_path, b->nHeight).total, 0U);
    BOOST_CHECK(all_counted());

    // Filled in again after its undo data went.
    const CBlockIndex* c = crash_in(spend(3));
    {
        LOCK(cs_main);
        ::g_chainman.m_blockman.PruneOneBlockFile(c->nFile);
        BOOST_REQUIRE(!(c->nStatus & BLOCK_HAVE_UNDO));
    }
    record = Start();
    RowView row;
    BOOST_REQUIRE(WaitUntil([&] {
        return record->readRow({c->nHeight, 0}, row, nullptr) == ReadResult::FOUND && row.tx.paid_by_kind != PaidByKind::PENDING;
    }));
    BOOST_CHECK(row.tx.paid_by_kind == PaidByKind::UNAVAILABLE);
    record->stop();
    const HeightBytes stored = BytesAt(m_path, c->nHeight);
    BOOST_CHECK_EQUAL(stored.keys.count(prefix::SPENT), 0U);
    {
        CDBWrapper db(m_path, 1 << 20, false, false, true);
        BlockMeta meta;
        BOOST_REQUIRE(db.Read(HeightKey(prefix::BLOCK, c->nHeight), meta));
        BOOST_CHECK_EQUAL(meta.logical_bytes, stored.total);
    }
    BOOST_CHECK(all_counted());
}

BOOST_AUTO_TEST_CASE(any_key_is_a_record)
{
    // A database without the record's facts ('V') is new only when it holds no key of the record at all: with a key of
    // any one prefix, the spent scripts' among them, it fails to open.
    for (const uint8_t key_prefix : {prefix::TIP, prefix::COVERAGE, prefix::STATS, prefix::BLOCK, prefix::PROOF, prefix::ROW, prefix::PIECE,
                                     prefix::TX, prefix::SPENT, prefix::TXID, prefix::PENDING, prefix::END}) {
        const fs::path path = GetDataDir() / strprintf("lone-%c", char(key_prefix));
        {
            CDBWrapper db(path, 1 << 20, false, false, true);
            struct Key {
                uint8_t first;
                void Serialize(CDataStream& s) const { s << first << uint32_t{0}; }
            };
            BOOST_REQUIRE(db.Write(Key{key_prefix}, NoValue()));
        }
        auto record = std::make_shared<Record>(path, RecordOptions());
        record->load();
        BOOST_CHECK_MESSAGE(record->getStatus().state == State::FAILED && record->getStatus().reason == FailReason::OPEN_FAILED,
                            "prefix " << char(key_prefix));
        record->stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
