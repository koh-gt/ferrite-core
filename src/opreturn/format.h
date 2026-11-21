// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_FORMAT_H
#define BITCOIN_OPRETURN_FORMAT_H

#include <amount.h>
#include <clientversion.h>
#include <coins.h>
#include <compressor.h>
#include <consensus/consensus.h>
#include <opreturn/coverage.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <span.h>
#include <uint256.h>
#include <undo.h>
#include <version.h>

#include <cstdint>
#include <functional>
#include <ios>
#include <limits>
#include <string>
#include <vector>

/**
 * The database format of the OP_RETURN record.
 *
 * Keys are one prefix byte followed by big-endian integers, so that the database orders them by height. Values use
 * the tree's serialization; VARINT is its unsigned variable-length integer.
 *
 *   'V'                     RecordMeta       written when the record is created
 *   'T'                     CBlockLocator    the record's tip, in every batch of the live path
 *   'C'                     StoredCoverage   in every batch that changes it
 *   'S'                     Stats            in every batch
 *   'b' height              BlockMeta        a block with at least one row
 *   'p' height              the inclusion proof of the transactions with rows (CPartialMerkleTree), same blocks
 *   'r' height n            Row              row n of the block; rows are numbered 0 to nrows - 1 without holes
 *   's' height n k          ScriptPiece      bytes k * 65536 to (k + 1) * 65536 of the script of row n, only for a
 *                                            script longer than the 4,096 bytes a row holds
 *   't' height txpos        TxRecord         a transaction with at least one row
 *   'i' height txpos n      SpentScript      the script of the coin that input n of that transaction spends, when it
 *                                            is longer than SPENT_SCRIPT_INLINE_SIZE bytes
 *   'x' txid                TxLocation       where that transaction is
 *   'P' height              nothing          while the block's transaction records miss spent coins
 *   'z'                     nothing          written when the record is created and never erased
 *
 * 'z' sorts after every other key. The tree's database iterator reports no error: it passes over a block of the
 * table files that it cannot read, and becomes invalid at the end. So a scan of keys always ends on 'z', and one
 * that runs out of keys has failed; what a scan passes over, a reader finds by checking rows against block entries.
 *
 * Every later format keeps three rules, so that a program can always tell what it reads:
 *   1. every key prefix it adds sorts below the end key 'z' (0x7a), so that a scan still ends on 'z';
 *   2. the format number stays the first field of the 'V' value, so that a program finds the number of a format
 *      it does not know, and reads that record no further;
 *   3. readers ignore the bits of RecordMeta::flags and of a transaction's facts (TxFlag) that they do not know,
 *      so that an optional addition needs no new format.
 *
 * Every value has a bound, so that no read of one key loads more than about 64 KiB, except the coverage ('C', at
 * most about 86 KiB), a transaction record and a proof. A transaction record holds the transaction without its
 * witnesses and adds at most 83 bytes for each input (which has at least 41), 2 for each output and 80 in all, so it
 * is at most about three times the size of its transaction. Decoding a value that
 * breaks its bounds throws std::ios_base::failure, and so does writing one. Every read of a stored key or value
 * must go through Whole, which also refuses bytes after its end. The tree formats inside a value (compressed
 * outputs and spent coins, the locator, the proof) are read as the tree reads them: some of their values have more
 * than one encoding, and a compressed script longer than MAX_SCRIPT_SIZE reads as OP_RETURN.
 */
namespace opreturn {

//! The number of this format, the first field of RecordMeta.
static constexpr uint32_t FORMAT_VERSION{1};
//! Script bytes a row holds: the whole script when it is no longer, otherwise its first SCRIPT_HEAD_SIZE bytes.
static constexpr uint32_t SCRIPT_HEAD_SIZE{4096};
//! Script bytes per piece of a script longer than SCRIPT_HEAD_SIZE.
static constexpr uint32_t SCRIPT_PIECE_SIZE{65536};
//! The largest output script a block can hold: its bytes count four times against the block weight.
static constexpr uint32_t MAX_OUTPUT_SCRIPT_SIZE{MAX_BLOCK_WEIGHT / WITNESS_SCALE_FACTOR};
//! The most ranges of unreadable heights the record remembers.
static constexpr size_t MAX_UNREADABLE_RANGES{1000};
//! The longest version text in RecordMeta.
static constexpr size_t MAX_CREATED_BY_SIZE{256};
//! The most inputs or outputs a transaction record lists: more than a transaction in a block can have.
static constexpr uint64_t MAX_TX_ENTRIES{MAX_OUTPUT_SCRIPT_SIZE};
//! TxBrief::n_other while it is not known.
static constexpr uint8_t N_OTHER_UNKNOWN{255};
//! The longest script of a spent coin that a transaction record holds: that of a public key output with an
//! uncompressed key, the longest script that has an address. A longer one has a key of its own ('i').
static constexpr uint32_t SPENT_SCRIPT_INLINE_SIZE{67};

//! The first byte of every key. Every prefix sorts below END, the end key's, also in every later format.
namespace prefix {
static constexpr uint8_t META{'V'};
static constexpr uint8_t TIP{'T'};
static constexpr uint8_t COVERAGE{'C'};
static constexpr uint8_t STATS{'S'};
static constexpr uint8_t BLOCK{'b'};
static constexpr uint8_t PROOF{'p'};
static constexpr uint8_t ROW{'r'};
static constexpr uint8_t PIECE{'s'};
static constexpr uint8_t TX{'t'};
static constexpr uint8_t SPENT{'i'};
static constexpr uint8_t TXID{'x'};
static constexpr uint8_t PENDING{'P'};
static constexpr uint8_t END{'z'};
} // namespace prefix

//! Facts about a transaction with rows, written as a VARINT. A bit this program does not know is read, kept as it is
//! and means nothing to it, so that a later version can add a fact without a new format.
enum TxFlag : uint32_t {
    TX_COINBASE = 0x01,
    TX_HOGEX = 0x02,          //!< the block's MWEB integrating transaction
    TX_HAS_MWEB = 0x04,       //!< the transaction has an MWEB part
    TX_HAS_WITNESS = 0x08,
    TX_SPENT_PENDING = 0x10,  //!< a coin an input spends is not known yet
    TX_MULTI_OPRETURN = 0x20, //!< more than one row
    TX_HAS_PEGIN = 0x40,      //!< an output moves coins into MWEB
};
//! The bits this program knows.
static constexpr uint32_t TX_FLAGS_ALL{0x7f};

//! What paid for a transaction: the coin its first input spends.
enum class PaidByKind : uint8_t {
    PENDING = 0,     //!< not looked up yet
    SCRIPT = 1,      //!< the coin is known; its script is kept
    UNAVAILABLE = 2, //!< the block's undo data was deleted before the coin was looked up
    COINBASE = 3,    //!< new coins
    HOGEX = 4,       //!< the transaction is the block's MWEB integrating transaction
};

//! What is known of the coin an input spends.
enum class SpentState : uint8_t {
    PENDING = 0,
    KNOWN = 1,
    UNAVAILABLE = 2,
    NONE = 3, //!< the input of a coinbase spends no coin
};

//! A row's position: the height of its block, and its number among the block's rows. Rows are numbered from 0 in
//! the order of transactions and outputs, without the witness commitment. Ordered as the chain is.
struct RowKey {
    int32_t height{0};
    uint32_t n{0};

    friend bool operator==(const RowKey& a, const RowKey& b) { return a.height == b.height && a.n == b.n; }
    friend bool operator!=(const RowKey& a, const RowKey& b) { return !(a == b); }
    friend bool operator<(const RowKey& a, const RowKey& b) { return a.height < b.height || (a.height == b.height && a.n < b.n); }
};

//! A transaction's position: the height of its block and its position in the block.
struct TxKey {
    int32_t height{0};
    uint32_t txpos{0};

    friend bool operator==(const TxKey& a, const TxKey& b) { return a.height == b.height && a.txpos == b.txpos; }
    friend bool operator!=(const TxKey& a, const TxKey& b) { return !(a == b); }
};

namespace detail {
[[noreturn]] void Fail(const char* what);
void CheckHeight(int32_t height);

template <typename Stream>
void WriteKeyHeight(Stream& s, int32_t height)
{
    CheckHeight(height);
    ser_writedata32be(s, uint32_t(height));
}

template <typename Stream>
int32_t ReadKeyHeight(Stream& s)
{
    const uint32_t height = ser_readdata32be(s);
    if (height > uint32_t(std::numeric_limits<int32_t>::max())) Fail("OP_RETURN record key with a height above 2147483647");
    return int32_t(height);
}

template <typename Stream>
void ReadKeyPrefix(Stream& s, uint8_t expected)
{
    if (ser_readdata8(s) != expected) Fail("OP_RETURN record key of another kind");
}

template <typename Stream>
void WriteAmount(Stream& s, CAmount amount)
{
    if (!MoneyRange(amount)) Fail("OP_RETURN record amount out of range");
    s << VARINT(uint64_t(amount));
}

template <typename Stream>
CAmount ReadAmount(Stream& s)
{
    uint64_t amount;
    s >> VARINT(amount);
    if (amount > uint64_t(MAX_MONEY)) Fail("OP_RETURN record amount out of range");
    return CAmount(amount);
}

//! Writes bytes with their size first, as the tree writes a vector, after checking the size.
template <typename Stream, typename Bytes>
void WriteBytes(Stream& s, const Bytes& bytes, size_t max_size)
{
    if (bytes.size() > max_size) Fail("OP_RETURN record value too large");
    WriteCompactSize(s, bytes.size());
    if (!bytes.empty()) s.write((const char*)bytes.data(), bytes.size());
}

//! Reads what WriteBytes wrote, refusing a size above max_size before anything is allocated.
template <typename Stream, typename Bytes>
void ReadBytes(Stream& s, Bytes& bytes, size_t max_size)
{
    const uint64_t size = ReadCompactSize(s);
    if (size > max_size) Fail("OP_RETURN record value too large");
    bytes.resize(size);
    if (size > 0) s.read((char*)&bytes[0], size);
}
} // namespace detail

/**
 * Reads a key or value as the whole of what the stream holds: bytes left after its end throw
 * std::ios_base::failure. As `stream >> Whole<Row>(row)`, or with the database
 * `Whole<Row> whole(row); db.Read(key, whole)`.
 */
template <typename T>
class Whole
{
    T& m_value;

public:
    explicit Whole(T& value) : m_value(value) {}

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> m_value;
        if (!s.empty()) detail::Fail("OP_RETURN record value followed by other bytes");
    }
};

//! The key of a block entry, a proof or a pending mark: its prefix and a height.
struct HeightKey {
    uint8_t prefix;
    int32_t height;

    HeightKey(uint8_t prefix_in, int32_t height_in) : prefix(prefix_in), height(height_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix);
        detail::WriteKeyHeight(s, height);
    }
    //! Reads a key with the prefix this object was made with.
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix);
        height = detail::ReadKeyHeight(s);
    }
};

//! The key of a row.
struct RowDbKey {
    RowKey key;

    explicit RowDbKey(const RowKey& key_in = {}) : key(key_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix::ROW);
        detail::WriteKeyHeight(s, key.height);
        ser_writedata32be(s, key.n);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix::ROW);
        key.height = detail::ReadKeyHeight(s);
        key.n = ser_readdata32be(s);
    }
};

//! The key of piece k of a row's script.
struct PieceDbKey {
    RowKey key;
    uint32_t k{0};

    PieceDbKey() = default;
    PieceDbKey(const RowKey& key_in, uint32_t k_in) : key(key_in), k(k_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix::PIECE);
        detail::WriteKeyHeight(s, key.height);
        ser_writedata32be(s, key.n);
        ser_writedata32be(s, k);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix::PIECE);
        key.height = detail::ReadKeyHeight(s);
        key.n = ser_readdata32be(s);
        k = ser_readdata32be(s);
    }
};

//! The key of a transaction record.
struct TxDbKey {
    TxKey key;

    explicit TxDbKey(const TxKey& key_in = {}) : key(key_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix::TX);
        detail::WriteKeyHeight(s, key.height);
        ser_writedata32be(s, key.txpos);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix::TX);
        key.height = detail::ReadKeyHeight(s);
        key.txpos = ser_readdata32be(s);
    }
};

//! The key of the script of the coin that input n of a transaction spends.
struct SpentDbKey {
    TxKey tx;
    uint32_t n{0};

    SpentDbKey() = default;
    SpentDbKey(const TxKey& tx_in, uint32_t n_in) : tx(tx_in), n(n_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix::SPENT);
        detail::WriteKeyHeight(s, tx.height);
        ser_writedata32be(s, tx.txpos);
        ser_writedata32be(s, n);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix::SPENT);
        tx.height = detail::ReadKeyHeight(s);
        tx.txpos = ser_readdata32be(s);
        n = ser_readdata32be(s);
    }
};

//! The key of a transaction's location.
struct TxidDbKey {
    uint256 txid;

    explicit TxidDbKey(const uint256& txid_in = {}) : txid(txid_in) {}

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, prefix::TXID);
        s << txid;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadKeyPrefix(s, prefix::TXID);
        s >> txid;
    }
};

//! What the record is ('V'). The format number is its first field, in this format and in every later one. A record
//! of another format number is not read further: the rest of the value is that format's, and is skipped.
struct RecordMeta {
    uint32_t format{FORMAT_VERSION};
    uint256 genesis_hash;
    //! No bit is defined yet. A bit this program does not know is kept as it is and means nothing to it, so that a
    //! later version can add an optional fact without a new format.
    uint32_t flags{0};
    int64_t created_time{0};
    std::string created_by; //!< the version of the program that created the record

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (format != FORMAT_VERSION) detail::Fail("OP_RETURN record of another format");
        s << format << genesis_hash << flags << created_time;
        detail::WriteBytes(s, created_by, MAX_CREATED_BY_SIZE);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> format;
        if (format != FORMAT_VERSION) {
            s.ignore(s.size());
            return;
        }
        s >> genesis_hash >> flags >> created_time;
        detail::ReadBytes(s, created_by, MAX_CREATED_BY_SIZE);
    }
};

//! Which heights the record holds ('C'): the ranges of its coverage, its floor, and the heights it could not read.
struct StoredCoverage {
    std::vector<HeightRange> ranges; //!< canonical (IsCanonical), at most MAX_COVERAGE_RANGES
    int32_t floor{-1};               //!< heights below it were trimmed; -1: never trimmed
    std::vector<HeightRange> unreadable; //!< canonical (IsCanonical), at most MAX_UNREADABLE_RANGES

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        WriteCompactSize(s, ranges.size());
        for (const HeightRange& range : ranges) s << range.lo << range.hi;
        s << floor;
        WriteCompactSize(s, unreadable.size());
        for (const HeightRange& range : unreadable) s << range.lo << range.hi;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        const uint64_t n_ranges = ReadCompactSize(s);
        if (n_ranges > MAX_COVERAGE_RANGES) detail::Fail("OP_RETURN record coverage too large");
        ranges.resize(n_ranges);
        for (HeightRange& range : ranges) s >> range.lo >> range.hi;
        s >> floor;
        const uint64_t n_unreadable = ReadCompactSize(s);
        if (n_unreadable > MAX_UNREADABLE_RANGES) detail::Fail("OP_RETURN record coverage too large");
        unreadable.resize(n_unreadable);
        for (HeightRange& range : unreadable) s >> range.lo >> range.hi;
        Check();
    }
    void Check() const;
};

//! Totals of the record ('S').
struct Stats {
    uint64_t rows{0};
    uint64_t txs{0};
    uint64_t blocks{0};
    uint64_t script_bytes{0};
    uint64_t logical_bytes{0}; //!< the sum of the logical_bytes of every block entry

    SERIALIZE_METHODS(Stats, obj) { READWRITE(obj.rows, obj.txs, obj.blocks, obj.script_bytes, obj.logical_bytes); }
};

//! The facts of a block with rows ('b'). Its proof is kept under its own key ('p').
struct BlockMeta {
    uint256 hash;
    uint32_t time{0};
    uint32_t ntx{0};           //!< transactions in the block
    uint32_t nrows{0};         //!< rows in the block, at least one
    uint64_t logical_bytes{0}; //!< bytes of every key and value written for this height, this entry included

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << hash << time << VARINT(ntx) << VARINT(nrows) << VARINT(logical_bytes);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> hash >> time >> VARINT(ntx) >> VARINT(nrows) >> VARINT(logical_bytes);
        Check();
    }
    void Check() const;
};

//! The transaction's facts that every row of it needs. Stored in the transaction's first row.
struct TxBrief {
    uint256 txid;
    uint32_t block_time{0};
    uint32_t tx_flags{0}; //!< TxFlag bits
    PaidByKind paid_by_kind{PaidByKind::PENDING};
    CScript paid_by;        //!< the script of the coin the first input spends; empty unless paid_by_kind is SCRIPT
    CAmount fee{-1};        //!< -1: not known
    uint32_t vsize{0};
    uint32_t n_in{0};
    uint32_t n_out{0};
    uint32_t n_rows{0};     //!< at least one
    //! Outputs that are not OP_RETURN outputs and pay a script that no coin the transaction spends has, at most
    //! 254 counted; N_OTHER_UNKNOWN while not known.
    uint8_t n_other{N_OTHER_UNKNOWN};
    CAmount other_value{0}; //!< the value of all those outputs, also of those beyond the 254 counted

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << txid << block_time << VARINT(tx_flags) << uint8_t(paid_by_kind);
        detail::WriteBytes(s, paid_by, MAX_SCRIPT_SIZE);
        const uint64_t fee_plus_one{fee < 0 ? 0 : uint64_t(fee) + 1};
        const uint32_t n_other_plus_one{n_other == N_OTHER_UNKNOWN ? 0U : uint32_t{n_other} + 1};
        s << VARINT(fee_plus_one) << VARINT(vsize) << VARINT(n_in) << VARINT(n_out) << VARINT(n_rows);
        s << VARINT(n_other_plus_one);
        detail::WriteAmount(s, other_value);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t kind;
        uint64_t fee_plus_one;
        uint32_t n_other_plus_one;
        s >> txid >> block_time >> VARINT(tx_flags) >> kind;
        if (kind > uint8_t(PaidByKind::HOGEX)) detail::Fail("OP_RETURN record value invalid");
        paid_by_kind = PaidByKind(kind);
        detail::ReadBytes(s, paid_by, MAX_SCRIPT_SIZE);
        s >> VARINT(fee_plus_one) >> VARINT(vsize) >> VARINT(n_in) >> VARINT(n_out) >> VARINT(n_rows);
        s >> VARINT(n_other_plus_one);
        other_value = detail::ReadAmount(s);
        if (fee_plus_one > uint64_t(MAX_MONEY) + 1 || n_other_plus_one > N_OTHER_UNKNOWN) detail::Fail("OP_RETURN record value invalid");
        fee = fee_plus_one == 0 ? -1 : CAmount(fee_plus_one - 1);
        n_other = n_other_plus_one == 0 ? N_OTHER_UNKNOWN : uint8_t(n_other_plus_one - 1);
        Check();
    }
    void Check() const;
};

//! One OP_RETURN output ('r').
struct Row {
    uint32_t txpos{0};       //!< the position of the transaction in the block
    uint32_t vout{0};
    uint32_t first_delta{0}; //!< this row's number minus that of the transaction's first row
    uint16_t flags{0};       //!< RowFlag bits of the whole script
    CAmount value{0};
    uint32_t script_size{0}; //!< of the whole script
    uint32_t data_size{0};   //!< of the data of the whole script
    std::vector<unsigned char> head; //!< the whole script, or its first SCRIPT_HEAD_SIZE bytes
    TxBrief brief;           //!< only in the transaction's first row (first_delta 0)

    bool IsFirst() const { return first_delta == 0; }
    //! Whether the row holds the whole script.
    bool HeadIsWhole() const { return script_size <= SCRIPT_HEAD_SIZE; }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << VARINT(txpos) << VARINT(vout) << VARINT(first_delta) << flags;
        detail::WriteAmount(s, value);
        s << VARINT(script_size) << VARINT(data_size);
        detail::WriteBytes(s, head, SCRIPT_HEAD_SIZE);
        if (IsFirst()) s << brief;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> VARINT(txpos) >> VARINT(vout) >> VARINT(first_delta) >> flags;
        value = detail::ReadAmount(s);
        s >> VARINT(script_size) >> VARINT(data_size);
        detail::ReadBytes(s, head, SCRIPT_HEAD_SIZE);
        if (IsFirst()) {
            s >> brief;
        } else {
            brief = TxBrief();
        }
        Check();
    }
    void Check() const;
};

//! A piece of a long script ('s'). Every piece but the last holds SCRIPT_PIECE_SIZE bytes.
struct ScriptPiece {
    std::vector<unsigned char> bytes;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        if (bytes.empty()) detail::Fail("OP_RETURN record value invalid");
        detail::WriteBytes(s, bytes, SCRIPT_PIECE_SIZE);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadBytes(s, bytes, SCRIPT_PIECE_SIZE);
        if (bytes.empty()) detail::Fail("OP_RETURN record value invalid");
    }
};

//! The value of a pending mark ('P'): nothing.
struct NoValue {
    template <typename Stream>
    void Serialize(Stream&) const {}
    template <typename Stream>
    void Unserialize(Stream&) {}
};

/**
 * An input of a transaction record: the outpoint, the scriptSig, the sequence, and the coin it spends, written as the
 * block's undo data writes it. A coin's script longer than SPENT_SCRIPT_INLINE_SIZE is not held here: the coin is
 * written without it, followed by its size, under a state byte of its own, and the script has its own key ('i').
 */
struct TxInRecord {
    COutPoint prevout;
    CScript script_sig;
    uint32_t sequence{0};
    SpentState spent_state{SpentState::PENDING};
    Coin coin; //!< only for SpentState::KNOWN; without its script when script_apart is set
    //! SpentState::KNOWN: the size of the coin's script when it has its own key, otherwise 0.
    uint32_t script_apart{0};

    //! The state byte of a known coin whose script has its own key.
    static constexpr uint8_t KNOWN_APART{4};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << prevout;
        detail::WriteBytes(s, script_sig, MAX_SCRIPT_SIZE);
        s << sequence << (script_apart > 0 ? KNOWN_APART : uint8_t(spent_state));
        if (spent_state == SpentState::KNOWN) s << Using<TxInUndoFormatter>(coin);
        if (script_apart > 0) s << VARINT(script_apart);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t state;
        s >> prevout;
        detail::ReadBytes(s, script_sig, MAX_SCRIPT_SIZE);
        s >> sequence >> state;
        if (state > KNOWN_APART) detail::Fail("OP_RETURN record value invalid");
        spent_state = state == KNOWN_APART ? SpentState::KNOWN : SpentState(state);
        coin.Clear();
        script_apart = 0;
        if (spent_state == SpentState::KNOWN) s >> Using<TxInUndoFormatter>(coin);
        if (state == KNOWN_APART) {
            s >> VARINT(script_apart);
            if (script_apart == 0) detail::Fail("OP_RETURN record value invalid");
        }
        Check();
    }
    void Check() const;
};

//! The script of a spent coin that a transaction record does not hold ('i').
struct SpentScript {
    CScript script;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        detail::WriteBytes(s, script, MAX_SCRIPT_SIZE);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        detail::ReadBytes(s, script, MAX_SCRIPT_SIZE);
        Check();
    }
    void Check() const;
};

//! An output of a transaction record. An OP_RETURN output that is a row refers to its row, which has the script.
struct TxOutRecord {
    enum class Kind : uint8_t {
        SCRIPT = 0,      //!< value and script, compressed as the coins database compresses outputs
        ROW = 1,         //!< value, and the number of the row in the same block
        LONG_SCRIPT = 2, //!< value and a script longer than MAX_SCRIPT_SIZE, which that compression cannot keep
    };
    Kind kind{Kind::SCRIPT};
    CTxOut out;        //!< the value, and the script unless the output is a row
    uint32_t row_n{0}; //!< Kind::ROW only

    //! The record of an output: a row (`is_row`), or an output whose script is kept here.
    static TxOutRecord FromOutput(const CTxOut& txout, bool is_row, uint32_t row_n);

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << uint8_t(kind);
        if (kind == Kind::ROW) {
            detail::WriteAmount(s, out.nValue);
            s << VARINT(row_n);
        } else if (kind == Kind::LONG_SCRIPT) {
            detail::WriteAmount(s, out.nValue);
            detail::WriteBytes(s, out.scriptPubKey, MAX_OUTPUT_SCRIPT_SIZE);
        } else {
            s << Using<TxOutCompression>(out);
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t k;
        s >> k;
        if (k > uint8_t(Kind::LONG_SCRIPT)) detail::Fail("OP_RETURN record value invalid");
        kind = Kind(k);
        out.SetNull();
        row_n = 0;
        if (kind == Kind::ROW) {
            out.nValue = detail::ReadAmount(s);
            out.scriptPubKey.clear();
            s >> VARINT(row_n);
        } else if (kind == Kind::LONG_SCRIPT) {
            out.nValue = detail::ReadAmount(s);
            detail::ReadBytes(s, out.scriptPubKey, MAX_OUTPUT_SCRIPT_SIZE);
        } else {
            s >> Using<TxOutCompression>(out);
        }
        Check();
    }
    void Check() const;
};

//! A transaction with at least one row ('t'), without its witnesses and its MWEB part, whose counts it keeps.
struct TxRecord {
    uint256 txid;
    int32_t version{0};
    uint32_t locktime{0};
    uint32_t size{0};
    uint32_t weight{0};
    uint32_t vsize{0};
    uint32_t tx_flags{0}; //!< TxFlag bits
    std::vector<TxInRecord> vin;
    std::vector<TxOutRecord> vout;
    uint32_t n_pegin{0};  //!< with TX_HAS_MWEB only
    uint32_t n_pegout{0}; //!< with TX_HAS_MWEB only
    uint32_t n_kernel{0}; //!< with TX_HAS_MWEB only

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        Check();
        s << txid << version << locktime << VARINT(size) << VARINT(weight) << VARINT(vsize) << VARINT(tx_flags);
        s << VARINT(uint64_t(vin.size()));
        for (const TxInRecord& in : vin) s << in;
        s << VARINT(uint64_t(vout.size()));
        for (const TxOutRecord& o : vout) s << o;
        if (tx_flags & TX_HAS_MWEB) s << VARINT(n_pegin) << VARINT(n_pegout) << VARINT(n_kernel);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> txid >> version >> locktime >> VARINT(size) >> VARINT(weight) >> VARINT(vsize) >> VARINT(tx_flags);
        // Entries are read one at a time, so that a false count cannot make the decoder allocate more than the
        // value it reads holds.
        uint64_t count;
        s >> VARINT(count);
        if (count > MAX_TX_ENTRIES) detail::Fail("OP_RETURN record value too large");
        vin.clear();
        for (uint64_t i = 0; i < count; ++i) {
            vin.emplace_back();
            s >> vin.back();
        }
        s >> VARINT(count);
        if (count > MAX_TX_ENTRIES) detail::Fail("OP_RETURN record value too large");
        vout.clear();
        for (uint64_t i = 0; i < count; ++i) {
            vout.emplace_back();
            s >> vout.back();
        }
        n_pegin = n_pegout = n_kernel = 0;
        if (tx_flags & TX_HAS_MWEB) s >> VARINT(n_pegin) >> VARINT(n_pegout) >> VARINT(n_kernel);
        Check();
    }
    void Check() const;
};

//! Where a transaction with rows is ('x').
struct TxLocation {
    int32_t height{0};
    uint32_t txpos{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        detail::CheckHeight(height);
        s << height << txpos;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> height >> txpos;
        detail::CheckHeight(height);
    }
};

//! The bytes a key and its value take, as the record's size limit counts them.
template <typename K, typename V>
uint64_t StoredSize(const K& key, const V& value)
{
    return GetSerializeSize(key, CLIENT_VERSION) + GetSerializeSize(value, CLIENT_VERSION);
}

/**
 * Sets meta.logical_bytes to `others`, the bytes of every other key and value written for the block's height,
 * plus the bytes of the block entry's own key and value, which hold that number.
 */
void SetLogicalBytes(BlockMeta& meta, uint64_t others);

//! The number of pieces a script of `script_size` bytes is stored in: none when its row holds it whole.
uint32_t PieceCount(uint64_t script_size);
//! The size of piece k of such a script; 0 if it has no piece k.
uint32_t PieceSize(uint64_t script_size, uint32_t k);
//! What a row holds of a script.
Span<const unsigned char> ScriptHead(Span<const unsigned char> script);
//! Piece k of a script; empty if it has no piece k.
Span<const unsigned char> PieceOf(Span<const unsigned char> script, uint32_t k);

//! Reads piece k of a row's script into `piece`; false if it cannot.
using PieceReader = std::function<bool(uint32_t k, std::vector<unsigned char>& piece)>;

/**
 * Bytes [offset, offset + max_bytes) of a row's script, cut at its end: from the row when they lie in what it
 * holds, otherwise from the pieces `read_piece` gives, reading only the pieces that hold them. False when a piece
 * cannot be read, has the wrong size, or does not begin with what the row holds.
 */
bool ReadScriptBytes(const Row& row, uint64_t offset, uint64_t max_bytes, const PieceReader& read_piece,
                     std::vector<unsigned char>& out);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_FORMAT_H
