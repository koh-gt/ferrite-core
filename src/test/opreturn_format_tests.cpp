// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <compressor.h>
#include <merkleblock.h>
#include <opreturn/coverage.h>
#include <opreturn/format.h>
#include <opreturn/script.h>
#include <random.h>
#include <streams.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <version.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace opreturn;
using opreturn::test::MakeScript;

namespace {

constexpr int32_t MAX_HEIGHT{std::numeric_limits<int32_t>::max()};

template <typename T>
std::vector<unsigned char> Encode(const T& value)
{
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << value;
    return {stream.begin(), stream.end()};
}

//! Decodes a value as the record reads one, whole.
template <typename T>
T Decode(const std::vector<unsigned char>& bytes, T value = T())
{
    CDataStream stream(bytes, SER_DISK, CLIENT_VERSION);
    stream >> Whole<T>(value);
    return value;
}

template <typename T>
bool DecodeFails(const std::vector<unsigned char>& bytes, T value = T())
{
    try {
        Decode<T>(bytes, value);
    } catch (const std::ios_base::failure&) {
        return true;
    }
    return false;
}

//! Whether decoding is refused by a bound that is checked before what it bounds is read: such a refusal says "too
//! large", while a check of the decoded value and the end of the bytes give other messages.
template <typename T>
bool TooLarge(const std::vector<unsigned char>& bytes, T value = T())
{
    try {
        Decode<T>(bytes, value);
    } catch (const std::ios_base::failure& e) {
        return std::string(e.what()).find("too large") != std::string::npos;
    }
    return false;
}

//! Encodes, decodes and encodes again; every shorter encoding fails (all of them for small values).
template <typename T>
void CheckRoundTrip(const T& value, T empty = T())
{
    const std::vector<unsigned char> bytes = Encode(value);
    const T decoded = Decode<T>(bytes, empty);
    BOOST_CHECK(Encode(decoded) == bytes);
    const size_t step = bytes.size() <= 300 ? 1 : bytes.size() / 97;
    for (size_t n = 0; n < bytes.size(); n += step) {
        BOOST_CHECK(DecodeFails<T>(std::vector<unsigned char>(bytes.begin(), bytes.begin() + n), empty));
    }
    BOOST_CHECK(DecodeFails<T>(std::vector<unsigned char>(bytes.begin(), bytes.end() - 1), empty));
}

template <typename T>
bool EncodeFails(const T& value)
{
    try {
        Encode(value);
    } catch (const std::ios_base::failure&) {
        return true;
    }
    return false;
}

CScript ScriptOf(size_t size, unsigned char byte)
{
    const std::vector<unsigned char> bytes(size, byte);
    return CScript(bytes.begin(), bytes.end());
}

TxBrief MakeBrief()
{
    TxBrief brief;
    brief.txid = uint256S("5e1f00000000000000000000000000000000000000000000000000000000c0de");
    brief.block_time = 1700000000;
    brief.tx_flags = TX_HAS_WITNESS;
    brief.paid_by_kind = PaidByKind::SCRIPT;
    brief.paid_by = CScript() << OP_0 << std::vector<unsigned char>(20, 0x42);
    brief.fee = 1450;
    brief.vsize = 141;
    brief.n_in = 1;
    brief.n_out = 2;
    brief.n_rows = 1;
    brief.n_other = 0;
    brief.other_value = 0;
    return brief;
}

Row MakeRow(const std::vector<unsigned char>& script, bool first)
{
    Row row;
    row.txpos = 3;
    row.vout = 1;
    row.first_delta = first ? 0 : 2;
    const ParsedScript parsed = ParseOpReturn(script, PARSE_FACTS);
    row.flags = parsed.flags;
    row.script_size = script.size();
    row.data_size = parsed.data_size;
    const Span<const unsigned char> head = ScriptHead(script);
    row.head.assign(head.begin(), head.end());
    if (first) row.brief = MakeBrief();
    return row;
}

//! A row's encoding written field by field, so that a test can write values the format refuses.
struct RawRow {
    uint64_t first_delta{1};
    uint16_t flags{ROW_PUSH_ONLY | ROW_UTF8};
    uint64_t value{0};
    uint64_t script_size{3};
    uint64_t data_size{1};
    std::vector<unsigned char> head{OP_RETURN, 0x01, 'a'};
    std::vector<unsigned char> brief;

    std::vector<unsigned char> Encode() const
    {
        CDataStream s(SER_DISK, CLIENT_VERSION);
        s << VARINT(uint32_t{3}) << VARINT(uint32_t{0}) << VARINT(first_delta) << flags << VARINT(value);
        s << VARINT(script_size) << VARINT(data_size);
        WriteCompactSize(s, head.size());
        s.write((const char*)head.data(), head.size());
        s.write((const char*)brief.data(), brief.size());
        return {s.begin(), s.end()};
    }
};

//! A brief's encoding written field by field.
struct RawBrief {
    uint32_t tx_flags{0};
    uint8_t kind{uint8_t(PaidByKind::SCRIPT)};
    std::vector<unsigned char> paid_by{0x51};
    uint64_t fee_plus_one{0};
    uint64_t n_out{1};
    uint64_t n_rows{1};
    uint64_t n_other_plus_one{0};
    uint64_t other_value{0};

    std::vector<unsigned char> Encode() const
    {
        CDataStream s(SER_DISK, CLIENT_VERSION);
        s << uint256::ONE << uint32_t{0} << VARINT(tx_flags) << kind;
        WriteCompactSize(s, paid_by.size());
        s.write((const char*)paid_by.data(), paid_by.size());
        s << VARINT(fee_plus_one) << VARINT(uint32_t{100}) << VARINT(uint32_t{1}) << VARINT(n_out) << VARINT(n_rows);
        s << VARINT(n_other_plus_one) << VARINT(other_value);
        return {s.begin(), s.end()};
    }
};

std::vector<unsigned char> RawCoverage(const std::vector<std::pair<int32_t, int32_t>>& ranges, int32_t floor,
                                       const std::vector<std::pair<int32_t, int32_t>>& unreadable)
{
    CDataStream s(SER_DISK, CLIENT_VERSION);
    s << ranges << floor << unreadable;
    return {s.begin(), s.end()};
}

TxRecord MakeTxRecord()
{
    TxRecord record;
    record.txid = uint256S("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    record.version = 2;
    record.locktime = 500;
    record.size = 300;
    record.weight = 900;
    record.vsize = 225;
    record.tx_flags = TX_HAS_WITNESS | TX_HAS_MWEB | TX_SPENT_PENDING;
    for (const SpentState state : {SpentState::PENDING, SpentState::KNOWN, SpentState::UNAVAILABLE, SpentState::NONE}) {
        TxInRecord in;
        in.prevout = COutPoint(uint256S("aa"), uint32_t(state));
        in.script_sig = CScript() << std::vector<unsigned char>(72, 0x30);
        in.sequence = 0xfffffffd;
        in.spent_state = state;
        if (state == SpentState::KNOWN) {
            in.coin = Coin(CTxOut(123456789, CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 7) << OP_EQUALVERIFY << OP_CHECKSIG),
                           /*nHeightIn=*/77, /*fCoinBaseIn=*/true, /*fPegoutIn=*/false);
        }
        record.vin.push_back(in);
    }
    record.vout.push_back(TxOutRecord::FromOutput(CTxOut(1000, CScript() << OP_0 << std::vector<unsigned char>(32, 9)), false, 0));
    record.vout.push_back(TxOutRecord::FromOutput(CTxOut(0, CScript() << OP_RETURN << std::vector<unsigned char>{'h', 'i'}), true, 4));
    record.vout.push_back(TxOutRecord::FromOutput(CTxOut(5, ScriptOf(MAX_SCRIPT_SIZE + 1, 0x51)), false, 0));
    record.n_pegin = 1;
    record.n_pegout = 2;
    record.n_kernel = 3;
    return record;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_format_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(key_layout)
{
    // One prefix byte, then big-endian numbers.
    BOOST_CHECK_EQUAL(HexStr(Encode(HeightKey{prefix::BLOCK, 0x01020304})), "6201020304");
    BOOST_CHECK_EQUAL(HexStr(Encode(HeightKey{prefix::PROOF, 5})), "7000000005");
    BOOST_CHECK_EQUAL(HexStr(Encode(HeightKey{prefix::PENDING, 5})), "5000000005");
    BOOST_CHECK_EQUAL(HexStr(Encode(RowDbKey{{0x01020304, 0x05060708}})), "720102030405060708");
    BOOST_CHECK_EQUAL(HexStr(Encode(PieceDbKey{{1, 2}, 3})), "73000000010000000200000003");
    BOOST_CHECK_EQUAL(HexStr(Encode(TxDbKey{{1, 2}})), "740000000100000002");
    BOOST_CHECK_EQUAL(HexStr(Encode(SpentDbKey{{1, 2}, 3})), "69000000010000000200000003");
    BOOST_CHECK_EQUAL(HexStr(Encode(TxidDbKey{uint256::ONE})), "78" + HexStr(uint256::ONE));
    // Every prefix, a new one included, sorts below the end key 'z', which ends every scan.
    const std::set<uint8_t> prefixes{prefix::META, prefix::TIP, prefix::COVERAGE, prefix::STATS, prefix::BLOCK,
                                     prefix::PROOF, prefix::ROW, prefix::PIECE, prefix::TX, prefix::SPENT,
                                     prefix::TXID, prefix::PENDING, prefix::END};
    BOOST_CHECK_EQUAL(prefixes.size(), 13U);
    BOOST_CHECK_EQUAL(prefix::END, 0x7a);
    for (const uint8_t p : prefixes) {
        if (p != prefix::END) BOOST_CHECK_MESSAGE(p < prefix::END, "prefix " << int(p));
    }

    // The database orders keys byte by byte, which is the order of rows in the chain.
    std::vector<RowKey> keys;
    for (const int32_t height : {0, 1, 255, 256, 65535, 65536, 16777216, MAX_HEIGHT - 1, MAX_HEIGHT}) {
        for (const uint32_t n : {0U, 1U, 255U, 256U, std::numeric_limits<uint32_t>::max()}) keys.push_back({height, n});
    }
    std::vector<std::vector<unsigned char>> encoded;
    for (const RowKey& key : keys) encoded.push_back(Encode(RowDbKey{key}));
    BOOST_CHECK(std::is_sorted(keys.begin(), keys.end()));
    BOOST_CHECK(std::is_sorted(encoded.begin(), encoded.end()));
    BOOST_CHECK(std::adjacent_find(encoded.begin(), encoded.end()) == encoded.end());
    for (size_t i = 0; i < keys.size(); ++i) BOOST_CHECK(Decode<RowDbKey>(encoded[i]).key == keys[i]);

    // A key of another kind, a height above 2147483647, a short key, and a negative height are refused.
    BOOST_CHECK(DecodeFails<RowDbKey>(Encode(PieceDbKey{{1, 2}, 3})));
    BOOST_CHECK(DecodeFails<HeightKey>(Encode(HeightKey{prefix::PROOF, 1}), HeightKey{prefix::BLOCK, 0}));
    BOOST_CHECK(DecodeFails<RowDbKey>(ParseHex("728000000000000000")));
    BOOST_CHECK(DecodeFails<HeightKey>(ParseHex("62ffffffff"), HeightKey{prefix::BLOCK, 0}));
    BOOST_CHECK(DecodeFails<TxDbKey>(ParseHex("7400000001000000")));
    BOOST_CHECK(DecodeFails<RowDbKey>(ParseHex("72010203040506070800"))); // a byte after the key
    BOOST_CHECK(EncodeFails(RowDbKey{{-1, 0}}));
    BOOST_CHECK(EncodeFails(HeightKey{prefix::BLOCK, -1}));
    BOOST_CHECK_EQUAL(Decode<HeightKey>(Encode(HeightKey{prefix::BLOCK, MAX_HEIGHT}), HeightKey{prefix::BLOCK, 0}).height, MAX_HEIGHT);
}

BOOST_AUTO_TEST_CASE(row_round_trip)
{
    for (const size_t size : {size_t{1}, size_t{2}, size_t{83}, size_t{84}, size_t{4096}, size_t{4097}, size_t{65536}, size_t{65537}, size_t{998000}}) {
        const std::vector<unsigned char> script = MakeScript(size, -1, uint32_t(size));
        for (const bool first : {true, false}) {
            const Row row = MakeRow(script, first);
            BOOST_CHECK_EQUAL(row.head.size(), std::min<size_t>(size, SCRIPT_HEAD_SIZE));
            BOOST_CHECK_EQUAL(row.HeadIsWhole(), size <= SCRIPT_HEAD_SIZE);
            CheckRoundTrip(row);
            const Row decoded = Decode<Row>(Encode(row));
            BOOST_CHECK(decoded.head == row.head);
            BOOST_CHECK_EQUAL(decoded.script_size, size);
            BOOST_CHECK_EQUAL(decoded.flags, row.flags);
            BOOST_CHECK_EQUAL(decoded.IsFirst(), first);
            if (first) BOOST_CHECK(Encode(decoded.brief) == Encode(row.brief));
            // A row is at most its head and about a hundred bytes of facts, whatever the script's size.
            BOOST_CHECK(Encode(row).size() <= std::min<size_t>(size, SCRIPT_HEAD_SIZE) + 120);
        }
    }
    // Every kind of payer, and the fee and the count of other outputs at their edges.
    for (const PaidByKind kind : {PaidByKind::PENDING, PaidByKind::SCRIPT, PaidByKind::UNAVAILABLE, PaidByKind::COINBASE, PaidByKind::HOGEX}) {
        TxBrief brief = MakeBrief();
        brief.paid_by_kind = kind;
        if (kind != PaidByKind::SCRIPT) brief.paid_by.clear();
        for (const CAmount fee : {CAmount{-1}, CAmount{0}, MAX_MONEY}) {
            for (const uint8_t n_other : {uint8_t{0}, uint8_t{254}, N_OTHER_UNKNOWN}) {
                brief.fee = fee;
                brief.n_other = n_other;
                brief.other_value = MAX_MONEY;
                CheckRoundTrip(brief);
                const TxBrief decoded = Decode<TxBrief>(Encode(brief));
                BOOST_CHECK_EQUAL(decoded.fee, fee);
                BOOST_CHECK_EQUAL(decoded.n_other, n_other);
                BOOST_CHECK(decoded.paid_by_kind == kind);
            }
        }
    }
    TxBrief long_payer = MakeBrief();
    long_payer.paid_by = ScriptOf(MAX_SCRIPT_SIZE, 0x51);
    CheckRoundTrip(long_payer);

    // A row that is not its transaction's first holds no brief, also when it is read into a row that held one.
    const std::vector<unsigned char> small = MakeScript(83, -1, 83);
    const Row reused = Decode<Row>(Encode(MakeRow(small, false)), MakeRow(small, true));
    BOOST_CHECK(reused.brief.txid.IsNull());
    BOOST_CHECK(reused.brief.paid_by.empty());
    BOOST_CHECK_EQUAL(reused.brief.n_rows, 0U);
    // A value is read only whole: a byte after its end is refused.
    std::vector<unsigned char> padded = Encode(MakeRow(small, true));
    padded.push_back(0);
    BOOST_CHECK(DecodeFails<Row>(padded));
}

BOOST_AUTO_TEST_CASE(script_round_trip)
{
    // No script has no bytes: a row of no script is never written, and it has no pieces.
    BOOST_CHECK_EQUAL(PieceCount(0), 0U);
    Row no_script;
    no_script.first_delta = 1;
    BOOST_CHECK(EncodeFails(no_script));

    const std::map<size_t, uint32_t> pieces{{4096, 0}, {4097, 1}, {65536, 1}, {65537, 2}, {998000, 16}};
    for (const auto& entry : pieces) {
        const size_t size = entry.first;
        const std::vector<unsigned char> script = MakeScript(size, -1, uint32_t(size));
        BOOST_REQUIRE_EQUAL(PieceCount(size), entry.second);

        // Write the row and its pieces as the record stores them, keyed by their encoded keys.
        const RowKey key{123456, 7};
        std::map<std::vector<unsigned char>, std::vector<unsigned char>> db;
        db[Encode(RowDbKey{key})] = Encode(MakeRow(script, true));
        uint64_t piece_bytes = 0;
        for (uint32_t k = 0; k < PieceCount(size); ++k) {
            const Span<const unsigned char> bytes = PieceOf(script, k);
            BOOST_CHECK_EQUAL(bytes.size(), PieceSize(size, k));
            BOOST_CHECK_EQUAL(bytes.size(), k + 1 < PieceCount(size) ? SCRIPT_PIECE_SIZE : size - uint64_t{k} * SCRIPT_PIECE_SIZE);
            piece_bytes += bytes.size();
            ScriptPiece piece;
            piece.bytes.assign(bytes.begin(), bytes.end());
            db[Encode(PieceDbKey{key, k})] = Encode(piece);
            BOOST_CHECK(Encode(piece).size() <= SCRIPT_PIECE_SIZE + 5);
        }
        BOOST_CHECK_EQUAL(piece_bytes, PieceCount(size) == 0 ? 0 : size);
        BOOST_CHECK(PieceOf(script, PieceCount(size)).empty());
        BOOST_CHECK_EQUAL(PieceSize(size, PieceCount(size)), 0U);

        const Row row = Decode<Row>(db.at(Encode(RowDbKey{key})));
        std::set<uint32_t> read;
        const PieceReader reader = [&](uint32_t k, std::vector<unsigned char>& out) {
            read.insert(k);
            const auto it = db.find(Encode(PieceDbKey{key, k}));
            if (it == db.end()) return false;
            out = Decode<ScriptPiece>(it->second).bytes;
            return true;
        };

        // The whole script, and every range around the edges of the head and the pieces.
        std::vector<unsigned char> out;
        BOOST_CHECK(ReadScriptBytes(row, 0, size, reader, out));
        BOOST_CHECK(out == script);
        const std::vector<uint64_t> offsets{0, 1, 4095, 4096, 4097, 65535, 65536, 65537, size - 1, size, size + 1};
        const std::vector<uint64_t> lengths{0, 1, 2, 4095, 4096, 65536, 65537, size, std::numeric_limits<uint64_t>::max()};
        for (const uint64_t offset : offsets) {
            for (const uint64_t length : lengths) {
                read.clear();
                BOOST_REQUIRE(ReadScriptBytes(row, offset, length, reader, out));
                const uint64_t begin = std::min<uint64_t>(offset, size);
                const uint64_t end = begin + std::min<uint64_t>(length, size - begin);
                BOOST_CHECK(out == std::vector<unsigned char>(script.begin() + begin, script.begin() + end));
                // Only the pieces that hold the bytes are read, and none for bytes the row holds.
                std::set<uint32_t> needed;
                if (end > begin && end > row.head.size()) {
                    for (uint64_t k = begin / SCRIPT_PIECE_SIZE; k * SCRIPT_PIECE_SIZE < end; ++k) needed.insert(uint32_t(k));
                }
                BOOST_CHECK(read == needed);
            }
        }
        // What a search looks at: the first 65,536 bytes, from the row or the first piece alone.
        read.clear();
        BOOST_CHECK(ReadScriptBytes(row, 0, SCRIPT_PIECE_SIZE, reader, out));
        BOOST_CHECK(out == std::vector<unsigned char>(script.begin(), script.begin() + std::min<size_t>(size, SCRIPT_PIECE_SIZE)));
        BOOST_CHECK(read == (size <= SCRIPT_HEAD_SIZE ? std::set<uint32_t>{} : std::set<uint32_t>{0}));
    }
}

BOOST_AUTO_TEST_CASE(piece_errors)
{
    const std::vector<unsigned char> script = MakeScript(70000, -1, 5);
    const Row row = MakeRow(script, true);
    std::vector<unsigned char> out;
    const auto pieces_with = [&](uint32_t broken, std::vector<unsigned char> replacement) {
        return [&script, broken, replacement](uint32_t k, std::vector<unsigned char>& piece) {
            if (k == broken) {
                piece = replacement;
                return !replacement.empty();
            }
            const Span<const unsigned char> bytes = PieceOf(script, k);
            piece.assign(bytes.begin(), bytes.end());
            return true;
        };
    };
    BOOST_CHECK(ReadScriptBytes(row, 0, 70000, pieces_with(9, {}), out));
    BOOST_CHECK(out == script);
    // A missing piece, a short piece, a long piece, a first piece that does not begin with the row's head.
    BOOST_CHECK(!ReadScriptBytes(row, 0, 70000, pieces_with(1, {}), out));
    BOOST_CHECK(out.empty());
    BOOST_CHECK(!ReadScriptBytes(row, 65536, 10, pieces_with(1, std::vector<unsigned char>(4463, 0)), out));
    BOOST_CHECK(!ReadScriptBytes(row, 65536, 10, pieces_with(1, std::vector<unsigned char>(4465, 0)), out));
    std::vector<unsigned char> first(script.begin(), script.begin() + SCRIPT_PIECE_SIZE);
    first[100] ^= 1;
    BOOST_CHECK(!ReadScriptBytes(row, 4000, 200, pieces_with(0, first), out));
    // Bytes in the head need no piece at all.
    BOOST_CHECK(ReadScriptBytes(row, 0, SCRIPT_HEAD_SIZE, pieces_with(0, {}), out));
    BOOST_CHECK(out == std::vector<unsigned char>(script.begin(), script.begin() + SCRIPT_HEAD_SIZE));
}

BOOST_AUTO_TEST_CASE(row_bounds)
{
    BOOST_CHECK(!DecodeFails<Row>(RawRow{}.Encode()));
    RawRow r;
    r.head.assign(SCRIPT_HEAD_SIZE + 1, 'a');
    r.head[0] = OP_RETURN;
    r.script_size = SCRIPT_HEAD_SIZE + 1;
    r.flags = 0;
    r.data_size = SCRIPT_HEAD_SIZE;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // a head above 4,096 bytes
    r = RawRow{};
    r.script_size = 4;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // the head is not the whole of a short script
    r = RawRow{};
    r.script_size = 0;
    r.head.clear();
    r.data_size = 0;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // no script
    r = RawRow{};
    r.script_size = uint64_t{MAX_OUTPUT_SCRIPT_SIZE} + 1;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // larger than a block can hold
    r = RawRow{};
    r.head[0] = 0x6b;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // not an OP_RETURN script
    r = RawRow{};
    r.flags |= 0x0100;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // an unknown flag
    r = RawRow{};
    r.data_size = 2;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // more data than the flags allow
    r = RawRow{};
    r.flags = ROW_PUSH_ONLY | ROW_PARSE_ERROR | ROW_UTF8;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // flags that no script has
    r = RawRow{};
    r.value = uint64_t(MAX_MONEY) + 1;
    BOOST_CHECK(DecodeFails<Row>(r.Encode())); // more than all coins
    r = RawRow{};
    r.value = uint64_t(MAX_MONEY);
    BOOST_CHECK(!DecodeFails<Row>(r.Encode()));
    // A row of 4,097 bytes holds exactly 4,096.
    r = RawRow{};
    r.script_size = SCRIPT_HEAD_SIZE + 1;
    r.flags = ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83;
    r.data_size = SCRIPT_HEAD_SIZE + 1 - 6;
    r.head.assign(SCRIPT_HEAD_SIZE, 'a');
    r.head[0] = OP_RETURN;
    BOOST_CHECK(!DecodeFails<Row>(r.Encode()));
    r.head.pop_back();
    BOOST_CHECK(DecodeFails<Row>(r.Encode()));

    // The brief of a first row.
    r = RawRow{};
    r.first_delta = 0;
    r.brief = RawBrief{}.Encode();
    BOOST_CHECK(!DecodeFails<Row>(r.Encode()));
    const auto brief_fails = [](const RawBrief& brief) {
        RawRow row;
        row.first_delta = 0;
        row.brief = brief.Encode();
        return DecodeFails<Row>(row.Encode()) && DecodeFails<TxBrief>(brief.Encode());
    };
    RawBrief b;
    b.kind = 5;
    BOOST_CHECK(brief_fails(b)); // an unknown kind of payer
    b = RawBrief{};
    b.kind = uint8_t(PaidByKind::PENDING);
    BOOST_CHECK(brief_fails(b)); // a script without a known coin
    b.paid_by.clear();
    BOOST_CHECK(!brief_fails(b));
    b = RawBrief{};
    b.paid_by.assign(MAX_SCRIPT_SIZE + 1, 0x51);
    BOOST_CHECK(brief_fails(b)); // a paying script above the size a coin can have
    b = RawBrief{};
    b.fee_plus_one = uint64_t(MAX_MONEY) + 2;
    BOOST_CHECK(brief_fails(b));
    b.fee_plus_one = uint64_t(MAX_MONEY) + 1;
    BOOST_CHECK(!brief_fails(b));
    b = RawBrief{};
    b.n_other_plus_one = 256;
    BOOST_CHECK(brief_fails(b));
    b.n_other_plus_one = 255;
    BOOST_CHECK(!brief_fails(b));
    // A fact this program does not know is kept, not refused.
    b = RawBrief{};
    b.tx_flags = 0x80 | (uint32_t{1} << 31);
    BOOST_CHECK(!brief_fails(b));
    b = RawBrief{};
    b.n_rows = 0;
    BOOST_CHECK(brief_fails(b));
    b = RawBrief{};
    b.n_rows = 2;
    BOOST_CHECK(brief_fails(b)); // more rows than outputs
    b = RawBrief{};
    b.other_value = uint64_t(MAX_MONEY) + 1;
    BOOST_CHECK(brief_fails(b));

    // Writing such values fails too.
    Row bad = MakeRow(MakeScript(10, 'a'), true);
    bad.flags |= ROW_PARSE_ERROR;
    BOOST_CHECK(EncodeFails(bad));
    bad = MakeRow(MakeScript(10, 'a'), true);
    bad.value = -1;
    BOOST_CHECK(EncodeFails(bad));
    bad = MakeRow(MakeScript(10, 'a'), true);
    bad.brief.n_rows = 0;
    BOOST_CHECK(EncodeFails(bad));
}

BOOST_AUTO_TEST_CASE(piece_and_small_values)
{
    ScriptPiece piece;
    BOOST_CHECK(EncodeFails(piece)); // a piece holds at least one byte
    piece.bytes.assign(SCRIPT_PIECE_SIZE, 1);
    CheckRoundTrip(piece);
    piece.bytes.push_back(1);
    BOOST_CHECK(EncodeFails(piece));
    CDataStream big(SER_DISK, CLIENT_VERSION);
    WriteCompactSize(big, SCRIPT_PIECE_SIZE + 1);
    big.write(std::string(SCRIPT_PIECE_SIZE + 1, 'x').data(), SCRIPT_PIECE_SIZE + 1);
    BOOST_CHECK(DecodeFails<ScriptPiece>({big.begin(), big.end()}));
    BOOST_CHECK(DecodeFails<ScriptPiece>(ParseHex("00")));

    BOOST_CHECK(Encode(NoValue{}).empty());
    BOOST_CHECK(!DecodeFails<NoValue>({}));

    Stats stats;
    stats.rows = 1;
    stats.txs = 2;
    stats.blocks = 3;
    stats.script_bytes = 4;
    stats.logical_bytes = std::numeric_limits<uint64_t>::max();
    CheckRoundTrip(stats);

    TxLocation location;
    location.height = MAX_HEIGHT;
    location.txpos = std::numeric_limits<uint32_t>::max();
    CheckRoundTrip(location);
    location.height = -1;
    BOOST_CHECK(EncodeFails(location));
    BOOST_CHECK(DecodeFails<TxLocation>(ParseHex("ffffffff00000000")));

    BlockMeta meta;
    meta.hash = uint256::ONE;
    meta.time = 1700000000;
    meta.ntx = 2;
    meta.nrows = 1;
    meta.logical_bytes = 500;
    CheckRoundTrip(meta);
    meta.nrows = 0;
    BOOST_CHECK(EncodeFails(meta)); // a block entry exists only for a block with rows
    meta.nrows = 1;
    meta.ntx = 0;
    BOOST_CHECK(EncodeFails(meta));

    // The proof of a block's transactions with rows, under its own key, is the tree's own type.
    const std::vector<uint256> txids{uint256S("01"), uint256S("02"), uint256S("03")};
    CPartialMerkleTree proof(txids, {false, true, false});
    std::vector<uint256> matches;
    std::vector<unsigned int> positions;
    const uint256 root = proof.ExtractMatches(matches, positions);
    CPartialMerkleTree decoded = Decode<CPartialMerkleTree>(Encode(proof));
    matches.clear();
    positions.clear();
    BOOST_CHECK(decoded.ExtractMatches(matches, positions) == root);
    BOOST_CHECK(matches == std::vector<uint256>{txids[1]});
}

BOOST_AUTO_TEST_CASE(record_meta)
{
    RecordMeta meta;
    meta.genesis_hash = uint256::ONE;
    meta.created_time = 1700000000;
    meta.created_by = "v4.0.0";
    CheckRoundTrip(meta);
    meta.created_by.assign(MAX_CREATED_BY_SIZE, 'v');
    CheckRoundTrip(meta);
    meta.created_by.push_back('v');
    BOOST_CHECK(EncodeFails(meta));
    // Flags this program does not know are kept as they are, not refused.
    meta.created_by = "v4.0.0";
    meta.flags = 0xffffffff;
    CheckRoundTrip(meta);
    BOOST_CHECK_EQUAL(Decode<RecordMeta>(Encode(meta)).flags, 0xffffffffU);

    // Another format number: the rest of the value is that format's, and is skipped.
    CDataStream other(SER_DISK, CLIENT_VERSION);
    other << uint32_t{2} << std::string("anything at all");
    const RecordMeta newer = Decode<RecordMeta>({other.begin(), other.end()});
    BOOST_CHECK_EQUAL(newer.format, 2U);
    BOOST_CHECK(newer.created_by.empty());
    BOOST_CHECK(EncodeFails(newer)); // this program writes only its own format
    BOOST_CHECK_EQUAL(FORMAT_VERSION, 1U);
}

BOOST_AUTO_TEST_CASE(tx_record)
{
    const TxRecord record = MakeTxRecord();
    CheckRoundTrip(record);
    const TxRecord decoded = Decode<TxRecord>(Encode(record));
    BOOST_REQUIRE_EQUAL(decoded.vin.size(), 4U);
    BOOST_CHECK(decoded.vin[1].coin.out == record.vin[1].coin.out);
    BOOST_CHECK_EQUAL(uint32_t{decoded.vin[1].coin.nHeight}, 77U);
    BOOST_CHECK(decoded.vin[1].coin.fCoinBase);
    BOOST_CHECK(decoded.vin[0].coin.IsSpent());
    BOOST_CHECK(decoded.vin[2].script_sig == record.vin[2].script_sig);
    BOOST_REQUIRE_EQUAL(decoded.vout.size(), 3U);
    BOOST_CHECK(decoded.vout[1].kind == TxOutRecord::Kind::ROW);
    BOOST_CHECK_EQUAL(decoded.vout[1].row_n, 4U);
    BOOST_CHECK_EQUAL(decoded.n_kernel, 3U);

    // The coins database's compression keeps scripts of up to 10,000 bytes; a longer output script is kept whole.
    BOOST_CHECK(TxOutRecord::FromOutput(CTxOut(0, ScriptOf(MAX_SCRIPT_SIZE, 0x51)), false, 0).kind == TxOutRecord::Kind::SCRIPT);
    BOOST_CHECK(decoded.vout[2].kind == TxOutRecord::Kind::LONG_SCRIPT);
    BOOST_CHECK(decoded.vout[2].out == record.vout[2].out);
    BOOST_CHECK_EQUAL(decoded.vout[2].out.scriptPubKey.size(), size_t{MAX_SCRIPT_SIZE + 1});
    CDataStream compressed(SER_DISK, CLIENT_VERSION);
    const CTxOut long_out(5, ScriptOf(MAX_SCRIPT_SIZE + 1, 0x51));
    compressed << Using<TxOutCompression>(long_out);
    CTxOut back;
    compressed >> Using<TxOutCompression>(back);
    BOOST_CHECK(back.scriptPubKey == CScript() << OP_RETURN);

    // Without an MWEB part the counts are not stored, so they must be 0.
    TxRecord no_mweb = record;
    no_mweb.tx_flags &= ~TX_HAS_MWEB;
    BOOST_CHECK(EncodeFails(no_mweb));
    no_mweb.n_pegin = no_mweb.n_pegout = no_mweb.n_kernel = 0;
    CheckRoundTrip(no_mweb);
    // A record has an input and a row.
    TxRecord bad = record;
    bad.vin.clear();
    BOOST_CHECK(EncodeFails(bad));
    bad = record;
    bad.vout.erase(bad.vout.begin() + 1);
    BOOST_CHECK(EncodeFails(bad));
    bad = record;
    bad.vout[0].out.nValue = MAX_MONEY + 1;
    BOOST_CHECK(EncodeFails(bad));
    bad = record;
    bad.vout[0].kind = TxOutRecord::Kind::LONG_SCRIPT;
    BOOST_CHECK(EncodeFails(bad));
    bad = record;
    bad.vin[1].coin.out.nValue = -1;
    BOOST_CHECK(EncodeFails(bad));

    // A scriptSig has at most MAX_SCRIPT_SIZE bytes; one more is refused on write, and on read before its bytes
    // are read.
    TxInRecord in = record.vin[0];
    in.script_sig = ScriptOf(MAX_SCRIPT_SIZE, 0x51);
    CheckRoundTrip(in);
    in.script_sig.push_back(0x51);
    BOOST_CHECK(EncodeFails(in));
    CDataStream long_sig(SER_DISK, CLIENT_VERSION);
    long_sig << in.prevout << in.script_sig << in.sequence << uint8_t(in.spent_state);
    BOOST_CHECK(TooLarge<TxInRecord>({long_sig.begin(), long_sig.end()}));

    // Counts of inputs and of outputs above what a block can hold are refused before any entry is read.
    const auto counts = [&](uint64_t n_in, uint64_t n_out) {
        CDataStream s(SER_DISK, CLIENT_VERSION);
        s << uint256::ONE << int32_t{1} << uint32_t{0} << VARINT(uint32_t{1}) << VARINT(uint32_t{1}) << VARINT(uint32_t{1}) << uint8_t{0};
        s << VARINT(n_in);
        if (n_in == 1) s << record.vin[0] << VARINT(n_out);
        return std::vector<unsigned char>(s.begin(), s.end());
    };
    BOOST_CHECK(TooLarge<TxRecord>(counts(MAX_TX_ENTRIES + 1, 0)));
    BOOST_CHECK(TooLarge<TxRecord>(counts(1, MAX_TX_ENTRIES + 1)));
    BOOST_CHECK(DecodeFails<TxRecord>(counts(MAX_TX_ENTRIES, 0)) && !TooLarge<TxRecord>(counts(MAX_TX_ENTRIES, 0)));
    BOOST_CHECK(DecodeFails<TxRecord>(counts(1, MAX_TX_ENTRIES)) && !TooLarge<TxRecord>(counts(1, MAX_TX_ENTRIES)));
    // An unknown kind of output, an unknown state of a spent coin.
    std::vector<unsigned char> in_bytes = Encode(record.vin[0]);
    in_bytes[in_bytes.size() - 1] = TxInRecord::KNOWN_APART + 1;
    BOOST_CHECK(DecodeFails<TxInRecord>(in_bytes));
    std::vector<unsigned char> out_bytes = Encode(record.vout[1]);
    out_bytes[0] = 3;
    BOOST_CHECK(DecodeFails<TxOutRecord>(out_bytes));
}

BOOST_AUTO_TEST_CASE(spent_scripts)
{
    // An input whose coin's script is at most SPENT_SCRIPT_INLINE_SIZE bytes holds it; a longer script has its own
    // key, and the input holds only its size. A script of that size that the coins compression does not shorten is
    // the largest an input holds.
    const auto known = [](const CScript& script) {
        TxInRecord in;
        in.prevout = COutPoint(uint256::ONE, 0xffffffff);
        in.sequence = 0xffffffff;
        in.spent_state = SpentState::KNOWN;
        in.coin = Coin(CTxOut(MAX_MONEY - 1, script), /*nHeightIn=*/0x3fffffff, /*fCoinBaseIn=*/true, /*fPegoutIn=*/true);
        return in;
    };
    TxInRecord in = known(ScriptOf(SPENT_SCRIPT_INLINE_SIZE, 0x51));
    CheckRoundTrip(in);
    // The input's own bytes in a transaction (outpoint, an empty scriptSig, sequence) are 41; the record adds at most 83.
    BOOST_CHECK_EQUAL(Encode(in).size(), 41U + 83U);
    const TxInRecord decoded = Decode<TxInRecord>(Encode(in));
    BOOST_CHECK(decoded.coin.out == in.coin.out);
    BOOST_CHECK(decoded.coin.fPegout && decoded.coin.fCoinBase);
    BOOST_CHECK_EQUAL(decoded.script_apart, 0U);
    in = known(ScriptOf(SPENT_SCRIPT_INLINE_SIZE + 1, 0x51));
    BOOST_CHECK(EncodeFails(in));
    // The same coin with its script kept apart.
    for (const uint32_t size : {SPENT_SCRIPT_INLINE_SIZE + 1, uint32_t{MAX_SCRIPT_SIZE}}) {
        in = known(CScript());
        in.script_apart = size;
        CheckRoundTrip(in);
        BOOST_CHECK(Encode(in).size() <= 41U + 20U);
        const TxInRecord apart = Decode<TxInRecord>(Encode(in));
        BOOST_CHECK(apart.spent_state == SpentState::KNOWN);
        BOOST_CHECK_EQUAL(apart.script_apart, size);
        BOOST_CHECK(apart.coin.out.scriptPubKey.empty());
        BOOST_CHECK_EQUAL(apart.coin.out.nValue, MAX_MONEY - 1);
        BOOST_CHECK_EQUAL(uint32_t{apart.coin.nHeight}, 0x3fffffffU);
    }
    // A size that the input could hold, one above the largest script, a script beside its size, a size for a coin that
    // is not known, and a size of 0 written after the state byte of a coin kept apart are refused.
    in = known(CScript());
    in.script_apart = SPENT_SCRIPT_INLINE_SIZE;
    BOOST_CHECK(EncodeFails(in));
    in.script_apart = MAX_SCRIPT_SIZE + 1;
    BOOST_CHECK(EncodeFails(in));
    in = known(ScriptOf(20, 0x51));
    in.script_apart = 100;
    BOOST_CHECK(EncodeFails(in));
    in = TxInRecord();
    in.script_apart = 100;
    BOOST_CHECK(EncodeFails(in));
    for (const uint32_t size : {0U, SPENT_SCRIPT_INLINE_SIZE, uint32_t{MAX_SCRIPT_SIZE + 1}}) {
        in = known(CScript());
        CDataStream raw(SER_DISK, CLIENT_VERSION);
        raw << in.prevout;
        WriteCompactSize(raw, 0);
        raw << in.sequence << TxInRecord::KNOWN_APART << Using<TxInUndoFormatter>(in.coin) << VARINT(size);
        BOOST_CHECK(DecodeFails<TxInRecord>({raw.begin(), raw.end()}));
    }
    // Under the state byte of a coin kept apart, the coin itself must come without a script.
    {
        in = known(ScriptOf(20, 0x51));
        CDataStream raw(SER_DISK, CLIENT_VERSION);
        raw << in.prevout;
        WriteCompactSize(raw, 0);
        raw << in.sequence << TxInRecord::KNOWN_APART << Using<TxInUndoFormatter>(in.coin) << VARINT(uint32_t{100});
        BOOST_CHECK(DecodeFails<TxInRecord>({raw.begin(), raw.end()}));
    }

    // The value of the key 'i': a script longer than an input holds, and at most MAX_SCRIPT_SIZE bytes.
    SpentScript spent;
    for (const size_t size : {size_t{SPENT_SCRIPT_INLINE_SIZE + 1}, size_t{MAX_SCRIPT_SIZE}}) {
        spent.script = ScriptOf(size, 0x6a);
        CheckRoundTrip(spent);
    }
    spent.script = ScriptOf(SPENT_SCRIPT_INLINE_SIZE, 0x6a);
    BOOST_CHECK(EncodeFails(spent));
    BOOST_CHECK(DecodeFails<SpentScript>(Encode(ScriptPiece{std::vector<unsigned char>(SPENT_SCRIPT_INLINE_SIZE, 0x6a)})));
    spent.script = ScriptOf(MAX_SCRIPT_SIZE + 1, 0x6a);
    BOOST_CHECK(EncodeFails(spent));
    CDataStream big(SER_DISK, CLIENT_VERSION);
    WriteCompactSize(big, MAX_SCRIPT_SIZE + 1);
    big.write(std::string(MAX_SCRIPT_SIZE + 1, 'x').data(), MAX_SCRIPT_SIZE + 1);
    BOOST_CHECK(TooLarge<SpentScript>({big.begin(), big.end()}));

    // The bound of a transaction record: the transaction without witnesses, at most 83 bytes more for each input, 2
    // for each output and 80 in all. Inputs that hold their coins' longest scripts, rows with large values and
    // numbers, and a header with its largest numbers come close.
    CMutableTransaction tx;
    TxRecord record;
    record.txid = uint256::ONE;
    record.version = -1;
    record.locktime = 0xffffffff;
    record.size = record.weight = 4000000;
    record.vsize = 1000000;
    record.tx_flags = TX_FLAGS_ALL & ~TX_SPENT_PENDING;
    record.n_pegin = record.n_pegout = record.n_kernel = 0xffffffff;
    for (uint32_t i = 0; i < 300; ++i) {
        tx.vin.emplace_back(COutPoint(uint256::ONE, i));
        record.vin.push_back(known(ScriptOf(SPENT_SCRIPT_INLINE_SIZE, 0x51)));
        record.vin.back().prevout = COutPoint(uint256::ONE, i);
        record.vin.back().sequence = 0;
    }
    for (uint32_t i = 0; i < 300; ++i) {
        tx.vout.emplace_back(MAX_MONEY - 1, CScript() << OP_RETURN);
        record.vout.push_back(TxOutRecord::FromOutput(tx.vout.back(), true, 0xfffff + i));
    }
    const size_t stripped = ::GetSerializeSize(tx, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_WITNESS);
    const size_t stored = Encode(record).size();
    BOOST_CHECK_MESSAGE(stored <= stripped + 83 * tx.vin.size() + 2 * tx.vout.size() + 80, "stored " << stored << ", transaction " << stripped);
    BOOST_CHECK(stored > stripped + 82 * tx.vin.size());
}

BOOST_AUTO_TEST_CASE(tx_flags_kept)
{
    // The facts of a transaction are written as a VARINT: every fact this program knows takes one byte, the same as
    // before, and a fact it does not know (a later version's) is read and written again as it is.
    TxBrief brief = MakeBrief();
    brief.tx_flags = TX_FLAGS_ALL;
    std::vector<unsigned char> bytes = Encode(brief);
    BOOST_CHECK_EQUAL(bytes.at(32 + 4), TX_FLAGS_ALL);
    BOOST_CHECK_EQUAL(bytes.at(32 + 4 + 1), uint8_t(PaidByKind::SCRIPT));
    for (const uint32_t unknown : {uint32_t{0x80}, uint32_t{1} << 20, uint32_t{0xffffffff}}) {
        brief.tx_flags = TX_HAS_WITNESS | unknown;
        CheckRoundTrip(brief);
        BOOST_CHECK_EQUAL(Decode<TxBrief>(Encode(brief)).tx_flags, TX_HAS_WITNESS | unknown);
        TxRecord record = MakeTxRecord();
        record.tx_flags |= unknown;
        CheckRoundTrip(record);
        BOOST_CHECK_EQUAL(Decode<TxRecord>(Encode(record)).tx_flags, record.tx_flags);
    }
}

BOOST_AUTO_TEST_CASE(logical_bytes)
{
    // The block entry counts itself, although its own size depends on the number it holds.
    std::vector<uint64_t> others;
    for (uint64_t n = 0; n < 200; ++n) others.push_back(n);
    for (const uint64_t edge : {uint64_t{16511}, uint64_t{2113663}, uint64_t{270549119}}) {
        for (uint64_t n = edge - 80; n < edge + 80; ++n) others.push_back(n);
    }
    for (const uint64_t other : others) {
        BlockMeta meta;
        meta.hash = uint256::ONE;
        meta.ntx = 3;
        meta.nrows = 2;
        SetLogicalBytes(meta, other);
        BOOST_CHECK_EQUAL(meta.logical_bytes, other + StoredSize(HeightKey{prefix::BLOCK, 7}, meta));
    }
}

BOOST_AUTO_TEST_CASE(coverage_set)
{
    HeightSet set;
    BOOST_CHECK(set.Empty());
    BOOST_CHECK(set.Missing({0, 9}) == std::vector<HeightRange>({{0, 9}}));
    set.Add({0, 0});
    set.Add({2, 2});
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 0}, {2, 2}}));
    set.Add({1, 1}); // joins the two
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 2}}));
    set.Add({3, 5}); // adjacent
    set.Add({4, 9}); // overlapping
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 9}}));
    set.Add({5, 4});  // lo above hi: nothing
    set.Add({-3, 1}); // below 0: nothing
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 9}}));
    set.Remove({3, 4});
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 2}, {5, 9}}));
    BOOST_CHECK(set.Contains(0) && set.Contains(2) && !set.Contains(3) && !set.Contains(4) && set.Contains(5) && !set.Contains(10) && !set.Contains(-1));
    BOOST_CHECK_EQUAL(set.HeightCount(), 8U);
    BOOST_CHECK(set.Missing({-5, 12}) == std::vector<HeightRange>({{3, 4}, {10, 12}}));
    BOOST_CHECK(set.Missing({5, 9}).empty());
    BOOST_CHECK(set.Missing({6, 5}).empty());
    BOOST_CHECK(*set.HighestMissing({0, 12}) == 12);
    BOOST_CHECK(*set.HighestMissing({0, 9}) == 4);
    BOOST_CHECK(!set.HighestMissing({0, 2}));
    BOOST_CHECK(!set.HighestMissing({5, 9}));
    BOOST_CHECK(*set.HighestMissing({4, 9}) == 4);
    set.RemoveAbove(6);
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{0, 2}, {5, 6}}));
    set.RemoveBelow(1);
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{1, 2}, {5, 6}}));
    set.RemoveBelow(0);
    set.RemoveAbove(MAX_HEIGHT);
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{1, 2}, {5, 6}}));
    set.RemoveAbove(-1);
    BOOST_CHECK(set.Empty());

    // The highest heights.
    set.Add({MAX_HEIGHT, MAX_HEIGHT});
    set.Add({MAX_HEIGHT - 1, MAX_HEIGHT - 1});
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{MAX_HEIGHT - 1, MAX_HEIGHT}}));
    BOOST_CHECK(set.Contains(MAX_HEIGHT));
    BOOST_CHECK_EQUAL(set.HeightCount(), 2U);
    BOOST_CHECK(*set.HighestMissing({0, MAX_HEIGHT}) == MAX_HEIGHT - 2);
    BOOST_CHECK(set.Missing({MAX_HEIGHT - 3, MAX_HEIGHT}) == std::vector<HeightRange>({{MAX_HEIGHT - 3, MAX_HEIGHT - 2}}));
    set.Remove({MAX_HEIGHT, MAX_HEIGHT});
    BOOST_CHECK(set.Ranges() == std::vector<HeightRange>({{MAX_HEIGHT - 1, MAX_HEIGHT - 1}}));
    set.Add({0, MAX_HEIGHT});
    BOOST_CHECK_EQUAL(set.HeightCount(), uint64_t{MAX_HEIGHT} + 1);
    BOOST_CHECK(!set.HighestMissing({0, MAX_HEIGHT}));

    // Canonical lists only.
    HeightSet from;
    BOOST_CHECK(HeightSet::FromRanges({{0, 1}, {3, 3}}, from));
    BOOST_CHECK(!HeightSet::FromRanges({{0, 1}, {2, 3}}, from)); // touching
    BOOST_CHECK(!HeightSet::FromRanges({{0, 2}, {2, 3}}, from)); // overlapping
    BOOST_CHECK(!HeightSet::FromRanges({{3, 3}, {0, 1}}, from)); // out of order
    BOOST_CHECK(!HeightSet::FromRanges({{2, 1}}, from));         // lo above hi
    BOOST_CHECK(!HeightSet::FromRanges({{-1, 1}}, from));        // below 0
    BOOST_CHECK(from.Ranges() == std::vector<HeightRange>({{0, 1}, {3, 3}}));
}

BOOST_AUTO_TEST_CASE(coverage_set_model)
{
    // Random changes, compared with a plain set of heights; near 0 and near the highest height.
    FastRandomContext rng(/*fDeterministic=*/true);
    for (const int32_t base : {0, MAX_HEIGHT - 200}) {
        HeightSet set;
        std::set<int32_t> model;
        for (int i = 0; i < 3000; ++i) {
            const int32_t lo = base + int32_t(rng.randrange(201));
            const int32_t hi = std::min<int64_t>(MAX_HEIGHT, int64_t{lo} + int64_t(rng.randrange(12)));
            switch (rng.randrange(4)) {
            case 0:
            case 1:
                set.Add({lo, hi});
                for (int64_t h = lo; h <= hi; ++h) model.insert(int32_t(h));
                break;
            case 2:
                set.Remove({lo, hi});
                for (int64_t h = lo; h <= hi; ++h) model.erase(int32_t(h));
                break;
            case 3:
                if (rng.randrange(20) == 0) {
                    set.RemoveAbove(lo);
                    model.erase(model.upper_bound(lo), model.end());
                }
                break;
            }
            BOOST_REQUIRE(IsCanonical(set.Ranges()));
            BOOST_CHECK_EQUAL(set.HeightCount(), model.size());
            const int32_t probe = base + int32_t(rng.randrange(201));
            BOOST_CHECK_EQUAL(set.Contains(probe), model.count(probe) == 1);
            std::vector<HeightRange> missing;
            for (int64_t h = base; h <= base + 200; ++h) {
                if (model.count(int32_t(h))) continue;
                if (!missing.empty() && missing.back().hi + 1 == h) {
                    missing.back().hi = int32_t(h);
                } else {
                    missing.push_back({int32_t(h), int32_t(h)});
                }
            }
            BOOST_CHECK(set.Missing({base, base + 200}) == missing);
            const Optional<int32_t> highest = set.HighestMissing({base, base + 200});
            BOOST_CHECK(missing.empty() ? !highest : highest && *highest == missing.back().hi);
        }
    }
}

BOOST_AUTO_TEST_CASE(stored_coverage)
{
    StoredCoverage coverage;
    CheckRoundTrip(coverage);
    for (int32_t i = 0; i < int32_t(MAX_COVERAGE_RANGES); ++i) coverage.ranges.push_back({3 * i, 3 * i + 1});
    coverage.floor = 0;
    for (int32_t i = 0; i < int32_t(MAX_UNREADABLE_RANGES); ++i) coverage.unreadable.push_back({3 * i + 2, 3 * i + 2});
    CheckRoundTrip(coverage);
    // A stretch of heights that could not be read, as one damaged block file gives, is one range.
    StoredCoverage stretch;
    stretch.unreadable = {{0, 99999}, {200000, MAX_HEIGHT}};
    CheckRoundTrip(stretch);
    BOOST_CHECK_EQUAL(Encode(stretch).size(), 1U + 4U + 1U + 16U);
    coverage.ranges.push_back({3 * int32_t(MAX_COVERAGE_RANGES), 3 * int32_t(MAX_COVERAGE_RANGES)});
    BOOST_CHECK(EncodeFails(coverage));

    BOOST_CHECK(!DecodeFails<StoredCoverage>(RawCoverage({{0, 5}, {7, MAX_HEIGHT}}, MAX_HEIGHT, {{6, 6}})));
    BOOST_CHECK(!DecodeFails<StoredCoverage>(RawCoverage({}, -1, {})));
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({{0, 5}, {6, 9}}, -1, {})));  // touching
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({{0, 5}, {5, 9}}, -1, {})));  // overlapping
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({{7, 9}, {0, 5}}, -1, {})));  // out of order
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({{5, 0}}, -1, {})));          // lo above hi
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({{-1, 0}}, -1, {})));         // below 0
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -2, {})));                // floor
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -1, {{5, 5}, {3, 3}})));  // unreadable out of order
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -1, {{3, 4}, {4, 6}})));  // overlapping
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -1, {{3, 4}, {5, 6}})));  // touching
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -1, {{6, 5}})));          // lo above hi
    BOOST_CHECK(DecodeFails<StoredCoverage>(RawCoverage({}, -1, {{-1, 2}})));
    // Counts above the limits are refused before anything they count is read, also a count far above them.
    std::vector<std::pair<int32_t, int32_t>> too_many;
    for (int32_t i = 0; i <= int32_t(MAX_COVERAGE_RANGES); ++i) too_many.push_back({3 * i, 3 * i});
    BOOST_CHECK(TooLarge<StoredCoverage>(RawCoverage(too_many, -1, {})));
    std::vector<std::pair<int32_t, int32_t>> unreadable;
    for (int32_t i = 0; i <= int32_t(MAX_UNREADABLE_RANGES); ++i) unreadable.push_back({3 * i, 3 * i});
    BOOST_CHECK(TooLarge<StoredCoverage>(RawCoverage({}, -1, unreadable)));
    CDataStream claim(SER_DISK, CLIENT_VERSION);
    WriteCompactSize(claim, 0x01000000);
    BOOST_CHECK(TooLarge<StoredCoverage>({claim.begin(), claim.end()}));
}

BOOST_AUTO_TEST_SUITE_END()
