// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/format.h>

#include <opreturn/script.h>

#include <algorithm>

namespace opreturn {
namespace detail {

void Fail(const char* what)
{
    throw std::ios_base::failure(what);
}

void CheckHeight(int32_t height)
{
    if (height < 0) Fail("OP_RETURN record value with a negative height");
}

} // namespace detail

namespace {

void Require(bool condition)
{
    if (!condition) detail::Fail("OP_RETURN record value invalid");
}

} // namespace

void StoredCoverage::Check() const
{
    Require(ranges.size() <= MAX_COVERAGE_RANGES && IsCanonical(ranges));
    Require(floor >= -1);
    Require(unreadable.size() <= MAX_UNREADABLE_RANGES && IsCanonical(unreadable));
}

void BlockMeta::Check() const
{
    Require(ntx >= 1 && nrows >= 1);
}

void TxBrief::Check() const
{
    Require(uint8_t(paid_by_kind) <= uint8_t(PaidByKind::HOGEX));
    Require(paid_by_kind == PaidByKind::SCRIPT || paid_by.empty());
    Require(paid_by.size() <= MAX_SCRIPT_SIZE);
    Require(fee == -1 || MoneyRange(fee));
    Require(n_rows >= 1 && n_rows <= n_out);
    Require(MoneyRange(other_value));
}

void Row::Check() const
{
    Require(script_size >= 1 && script_size <= MAX_OUTPUT_SCRIPT_SIZE);
    Require(FlagsFit(flags, script_size, data_size));
    Require(head.size() == std::min(script_size, SCRIPT_HEAD_SIZE) && IsOpReturn(head));
    Require(MoneyRange(value));
}

void TxInRecord::Check() const
{
    Require(uint8_t(spent_state) <= uint8_t(SpentState::NONE));
    Require(script_sig.size() <= MAX_SCRIPT_SIZE);
    if (spent_state == SpentState::KNOWN) {
        Require(MoneyRange(coin.out.nValue) && coin.out.scriptPubKey.size() <= SPENT_SCRIPT_INLINE_SIZE);
        Require(script_apart == 0 || (coin.out.scriptPubKey.empty() && script_apart > SPENT_SCRIPT_INLINE_SIZE &&
                                      script_apart <= MAX_SCRIPT_SIZE));
    } else {
        Require(script_apart == 0);
    }
}

void SpentScript::Check() const
{
    Require(script.size() > SPENT_SCRIPT_INLINE_SIZE && script.size() <= MAX_SCRIPT_SIZE);
}

TxOutRecord TxOutRecord::FromOutput(const CTxOut& txout, bool is_row, uint32_t row_n)
{
    TxOutRecord record;
    record.out.nValue = txout.nValue;
    if (is_row) {
        record.kind = Kind::ROW;
        record.row_n = row_n;
    } else {
        record.kind = txout.scriptPubKey.size() > MAX_SCRIPT_SIZE ? Kind::LONG_SCRIPT : Kind::SCRIPT;
        record.out.scriptPubKey = txout.scriptPubKey;
    }
    return record;
}

void TxOutRecord::Check() const
{
    Require(uint8_t(kind) <= uint8_t(Kind::LONG_SCRIPT));
    Require(MoneyRange(out.nValue));
    const size_t size = out.scriptPubKey.size();
    switch (kind) {
    case Kind::SCRIPT: Require(size <= MAX_SCRIPT_SIZE); break;
    case Kind::ROW: Require(size == 0); break;
    case Kind::LONG_SCRIPT: Require(size > MAX_SCRIPT_SIZE && size <= MAX_OUTPUT_SCRIPT_SIZE); break;
    }
}

void TxRecord::Check() const
{
    Require(!vin.empty() && vin.size() <= MAX_TX_ENTRIES && vout.size() <= MAX_TX_ENTRIES);
    Require(std::any_of(vout.begin(), vout.end(), [](const TxOutRecord& o) { return o.kind == TxOutRecord::Kind::ROW; }));
    if (!(tx_flags & TX_HAS_MWEB)) Require(n_pegin == 0 && n_pegout == 0 && n_kernel == 0);
}

void SetLogicalBytes(BlockMeta& meta, uint64_t others)
{
    // The entry's size depends on the number it holds only through the length of a VARINT, and grows with it, so
    // the number settles within a few rounds.
    meta.logical_bytes = others;
    for (int i = 0; i < 16; ++i) {
        const uint64_t total = others + StoredSize(HeightKey{prefix::BLOCK, 0}, meta);
        if (total == meta.logical_bytes) return;
        meta.logical_bytes = total;
    }
}

uint32_t PieceCount(uint64_t script_size)
{
    if (script_size <= SCRIPT_HEAD_SIZE) return 0;
    return uint32_t((script_size + SCRIPT_PIECE_SIZE - 1) / SCRIPT_PIECE_SIZE);
}

uint32_t PieceSize(uint64_t script_size, uint32_t k)
{
    if (k >= PieceCount(script_size)) return 0;
    return uint32_t(std::min<uint64_t>(SCRIPT_PIECE_SIZE, script_size - uint64_t{k} * SCRIPT_PIECE_SIZE));
}

Span<const unsigned char> ScriptHead(Span<const unsigned char> script)
{
    return script.first(std::min<size_t>(script.size(), SCRIPT_HEAD_SIZE));
}

Span<const unsigned char> PieceOf(Span<const unsigned char> script, uint32_t k)
{
    const uint32_t size = PieceSize(script.size(), k);
    if (size == 0) return {};
    return script.subspan(size_t{k} * SCRIPT_PIECE_SIZE, size);
}

bool ReadScriptBytes(const Row& row, uint64_t offset, uint64_t max_bytes, const PieceReader& read_piece,
                     std::vector<unsigned char>& out)
{
    out.clear();
    if (offset >= row.script_size || max_bytes == 0) return true;
    const uint64_t end = offset + std::min<uint64_t>(max_bytes, row.script_size - offset);
    if (end <= row.head.size()) {
        out.assign(row.head.begin() + offset, row.head.begin() + end);
        return true;
    }
    out.reserve(end - offset);
    std::vector<unsigned char> piece;
    for (uint64_t k = offset / SCRIPT_PIECE_SIZE; k * SCRIPT_PIECE_SIZE < end; ++k) {
        piece.clear();
        if (!read_piece(uint32_t(k), piece) || piece.size() != PieceSize(row.script_size, uint32_t(k))) {
            out.clear();
            return false;
        }
        // The first piece begins with what the row holds.
        if (k == 0 && !std::equal(row.head.begin(), row.head.end(), piece.begin())) {
            out.clear();
            return false;
        }
        const uint64_t start = k * SCRIPT_PIECE_SIZE;
        const uint64_t from = std::max(offset, start) - start;
        const uint64_t to = std::min(end, start + piece.size()) - start;
        out.insert(out.end(), piece.begin() + from, piece.begin() + to);
    }
    return true;
}

} // namespace opreturn
