// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <clientversion.h>
#include <merkleblock.h>
#include <opreturn/format.h>
#include <opreturn/script.h>
#include <primitives/block.h>
#include <streams.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <version.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <ios>
#include <string>
#include <vector>

using namespace opreturn;

namespace {

/**
 * Decodes the bytes as a value of type T, whole, as the record reads one. Decoding either succeeds or throws
 * std::ios_base::failure, which the record treats as a failed read; any other exception ends the program. A
 * decoded value writes again, and what it writes decodes to a value that writes the same bytes.
 */
template <typename T>
bool Decode(const std::vector<uint8_t>& bytes, T& value)
{
    CDataStream stream(bytes, SER_DISK, CLIENT_VERSION);
    try {
        stream >> Whole<T>(value);
    } catch (const std::ios_base::failure&) {
        return false;
    }
    return true;
}

template <typename T>
void CheckRewrite(const T& value, T again)
{
    CDataStream first(SER_DISK, CLIENT_VERSION);
    first << value;
    const std::string written = first.str();
    first >> Whole<T>(again);
    CDataStream second(SER_DISK, CLIENT_VERSION);
    second << again;
    assert(second.str() == written);
}

template <typename T>
void Check(const std::vector<uint8_t>& bytes, T value = T())
{
    T decoded = value;
    if (Decode(bytes, decoded)) CheckRewrite(decoded, value);
}

void CheckRow(const std::vector<uint8_t>& bytes, FuzzedDataProvider& provider)
{
    Row row;
    if (!Decode(bytes, row)) return;
    CheckRewrite(row, Row());
    // The bounds every reader relies on.
    assert(row.script_size >= 1 && row.script_size <= MAX_OUTPUT_SCRIPT_SIZE);
    assert(row.head.size() == std::min(row.script_size, SCRIPT_HEAD_SIZE) && IsOpReturn(row.head));
    assert(FlagsFit(row.flags, row.script_size, row.data_size));
    // Reading any range from pieces of the right sizes gives that range, cut at the end of the script: the pieces
    // hold a script that is the row's head followed by fill bytes.
    const uint64_t offset = provider.ConsumeIntegralInRange<uint64_t>(0, uint64_t{row.script_size} + 1);
    const uint64_t length = provider.ConsumeIntegral<uint64_t>();
    const uint8_t fill = provider.ConsumeIntegral<uint8_t>();
    const PieceReader reader = [&](uint32_t k, std::vector<unsigned char>& piece) {
        piece.assign(PieceSize(row.script_size, k), fill);
        if (piece.empty()) return false;
        if (k == 0) std::copy(row.head.begin(), row.head.end(), piece.begin());
        return true;
    };
    std::vector<unsigned char> out;
    assert(ReadScriptBytes(row, offset, length, reader, out));
    const uint64_t begin = std::min<uint64_t>(offset, row.script_size);
    assert(out.size() == std::min<uint64_t>(length, row.script_size - begin));
    for (uint64_t i = 0; i < out.size(); ++i) {
        assert(out[i] == (begin + i < row.head.size() ? row.head[begin + i] : fill));
    }
}

//! Script sizes where the way a script is stored changes, which sizes drawn at random would seldom be.
constexpr uint32_t EDGE_SIZES[]{1, 2, 83, 84, SCRIPT_HEAD_SIZE - 1, SCRIPT_HEAD_SIZE, SCRIPT_HEAD_SIZE + 1,
                                SCRIPT_PIECE_SIZE - 1, SCRIPT_PIECE_SIZE, SCRIPT_PIECE_SIZE + 1, 2 * SCRIPT_PIECE_SIZE,
                                2 * SCRIPT_PIECE_SIZE + 1, MAX_OUTPUT_SCRIPT_SIZE};

/**
 * Stores a script made from the input as the record does, as a row and pieces, and reads it back: a script the row
 * holds whole has no pieces, a longer one is its pieces joined, and any range reads as the script's bytes. Then the
 * row's encoding with one byte changed goes through CheckRow, so that decoding starts from a valid encoding.
 */
void CheckStoredScript(FuzzedDataProvider& provider)
{
    const uint32_t size = provider.ConsumeBool() ? provider.PickValueInArray(EDGE_SIZES) :
                                                   provider.ConsumeIntegralInRange<uint32_t>(1, MAX_OUTPUT_SCRIPT_SIZE);
    std::vector<uint8_t> pattern = provider.ConsumeBytes<uint8_t>(provider.ConsumeIntegralInRange<size_t>(1, 256));
    if (pattern.empty()) pattern.push_back(0);
    std::vector<unsigned char> script(size);
    script[0] = OP_RETURN;
    for (size_t i = 1; i < script.size(); ++i) script[i] = pattern[(i - 1) % pattern.size()];

    Row row;
    row.first_delta = 1;
    const ParsedScript parsed = ParseOpReturn(script, PARSE_FACTS);
    row.flags = parsed.flags;
    row.script_size = size;
    row.data_size = parsed.data_size;
    const Span<const unsigned char> head = ScriptHead(script);
    row.head.assign(head.begin(), head.end());
    CDataStream stream(SER_DISK, CLIENT_VERSION);
    stream << row;
    std::vector<uint8_t> encoded(stream.begin(), stream.end());
    Row stored;
    assert(Decode(encoded, stored));

    std::vector<std::vector<unsigned char>> pieces;
    for (uint32_t k = 0; k < PieceCount(size); ++k) {
        const Span<const unsigned char> piece = PieceOf(script, k);
        pieces.emplace_back(piece.begin(), piece.end());
    }
    if (size <= SCRIPT_HEAD_SIZE) {
        assert(pieces.empty());
    } else {
        std::vector<unsigned char> joined;
        for (size_t k = 0; k < pieces.size(); ++k) {
            assert(k + 1 < pieces.size() ? pieces[k].size() == SCRIPT_PIECE_SIZE : !pieces[k].empty() && pieces[k].size() <= SCRIPT_PIECE_SIZE);
            joined.insert(joined.end(), pieces[k].begin(), pieces[k].end());
        }
        assert(joined == script);
    }

    const PieceReader reader = [&](uint32_t k, std::vector<unsigned char>& piece) {
        if (k >= pieces.size()) return false;
        piece = pieces[k];
        return true;
    };
    std::vector<unsigned char> out;
    assert(ReadScriptBytes(stored, 0, size, reader, out) && out == script);
    const uint64_t offset = provider.ConsumeIntegralInRange<uint64_t>(0, uint64_t{size} + 1);
    const uint64_t length = provider.ConsumeIntegralInRange<uint64_t>(0, uint64_t{size} + 1);
    assert(ReadScriptBytes(stored, offset, length, reader, out));
    const uint64_t begin = std::min<uint64_t>(offset, size);
    const uint64_t end = begin + std::min<uint64_t>(length, size - begin);
    assert(out == std::vector<unsigned char>(script.begin() + begin, script.begin() + end));

    encoded[provider.ConsumeIntegralInRange<size_t>(0, encoded.size() - 1)] ^= provider.ConsumeIntegralInRange<uint8_t>(1, 255);
    CheckRow(encoded, provider);
}

} // namespace

void test_one_input(const std::vector<uint8_t>& buffer)
{
    FuzzedDataProvider provider(buffer.data(), buffer.size());
    const uint8_t kind = provider.ConsumeIntegral<uint8_t>() % 20;
    if (kind == 19) {
        CheckStoredScript(provider);
        return;
    }
    const std::vector<uint8_t> bytes = provider.ConsumeBytes<uint8_t>(provider.ConsumeIntegralInRange<size_t>(0, provider.remaining_bytes()));
    switch (kind) {
    case 0: {
        // A record of another format number is not read further, and this program does not write one.
        RecordMeta meta;
        if (Decode(bytes, meta) && meta.format == FORMAT_VERSION) CheckRewrite(meta, RecordMeta());
        break;
    }
    case 1: Check<CBlockLocator>(bytes); break;
    case 2: Check<StoredCoverage>(bytes); break;
    case 3: Check<Stats>(bytes); break;
    case 4: Check<BlockMeta>(bytes); break;
    case 5: Check<CPartialMerkleTree>(bytes); break;
    case 6: CheckRow(bytes, provider); break;
    case 7: Check<ScriptPiece>(bytes); break;
    case 8: Check<TxRecord>(bytes); break;
    case 9: Check<TxLocation>(bytes); break;
    case 10: Check<NoValue>(bytes); break;
    case 11: Check<TxBrief>(bytes); break;
    case 12: Check<HeightKey>(bytes, HeightKey{provider.PickValueInArray({prefix::BLOCK, prefix::PROOF, prefix::PENDING}), 0}); break;
    case 13: Check<RowDbKey>(bytes); break;
    case 14: Check<PieceDbKey>(bytes); break;
    case 15: Check<TxDbKey>(bytes); break;
    case 16: Check<TxidDbKey>(bytes); break;
    case 17: Check<SpentScript>(bytes); break;
    case 18: Check<SpentDbKey>(bytes); break;
    }
}
