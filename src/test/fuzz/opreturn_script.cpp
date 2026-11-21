// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <opreturn/script.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/fuzz/fuzz.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <vector>

using namespace opreturn;

namespace {

//! The whole script's parts cut at `size` bytes, as the prefix parser must give them.
std::vector<DataPart> CutParts(const ParsedScript& whole, size_t size)
{
    std::vector<DataPart> cut;
    if (whole.flags & ROW_PUSH_ONLY) {
        for (const DataPart& part : whole.parts) {
            if (part.offset > size) break;
            cut.push_back({part.offset, std::min(part.size, size - part.offset)});
        }
    } else if (size > 1) {
        cut.push_back({1, size - 1});
    }
    return cut;
}

//! Whether bytes are well-formed UTF-8, read by the table of well-formed byte sequences of the Unicode standard,
//! apart from the parser's own reading, so that the two can be compared.
bool WellFormedUtf8(const std::vector<unsigned char>& bytes)
{
    // The range of a sequence's first byte, the range of its second byte, and how many bytes follow the first;
    // every later byte is 80 to bf.
    struct Form {
        unsigned char first_lo, first_hi, second_lo, second_hi;
        size_t more;
    };
    static constexpr Form FORMS[]{{0x00, 0x7f, 0x00, 0x00, 0}, {0xc2, 0xdf, 0x80, 0xbf, 1}, {0xe0, 0xe0, 0xa0, 0xbf, 2},
                                  {0xe1, 0xec, 0x80, 0xbf, 2}, {0xed, 0xed, 0x80, 0x9f, 2}, {0xee, 0xef, 0x80, 0xbf, 2},
                                  {0xf0, 0xf0, 0x90, 0xbf, 3}, {0xf1, 0xf3, 0x80, 0xbf, 3}, {0xf4, 0xf4, 0x80, 0x8f, 3}};
    for (size_t i = 0; i < bytes.size();) {
        const Form* form = std::find_if(std::begin(FORMS), std::end(FORMS), [&](const Form& f) { return bytes[i] >= f.first_lo && bytes[i] <= f.first_hi; });
        if (form == std::end(FORMS) || bytes.size() - i <= form->more) return false;
        for (size_t j = 1; j <= form->more; ++j) {
            const unsigned char lo = j == 1 ? form->second_lo : 0x80;
            const unsigned char hi = j == 1 ? form->second_hi : 0xbf;
            if (bytes[i + j] < lo || bytes[i + j] > hi) return false;
        }
        i += form->more + 1;
    }
    return true;
}

} // namespace

void test_one_input(const std::vector<uint8_t>& buffer)
{
    // The input is an output script.
    const std::vector<unsigned char>& script = buffer;
    assert(IsUtf8(script) == WellFormedUtf8(script));
    const ParsedScript parsed = ParseOpReturn(script, PARSE_DATA | PARSE_PARTS);
    if (!IsOpReturn(script)) {
        assert(parsed.flags == 0 && parsed.data_size == 0 && parsed.pushes == 0);
        assert(parsed.data.empty() && parsed.parts.empty());
        return;
    }

    // The data and the parts lie in the script after OP_RETURN, in order, and agree.
    assert(FlagsFit(parsed.flags, script.size(), parsed.data_size));
    assert(parsed.data.size() == parsed.data_size);
    std::vector<unsigned char> joined;
    size_t end = 1;
    for (const DataPart& part : parsed.parts) {
        assert(part.offset >= end && part.offset <= script.size() && part.size <= script.size() - part.offset);
        joined.insert(joined.end(), script.begin() + part.offset, script.begin() + part.offset + part.size);
        end = part.offset + part.size;
    }
    assert(joined == parsed.data);
    assert(((parsed.flags & ROW_UTF8) != 0) == WellFormedUtf8(parsed.data));
    if (parsed.flags & ROW_PUSH_ONLY) {
        assert(parsed.parts.size() == parsed.pushes);
        assert(((parsed.flags & ROW_MULTI_PUSH) != 0) == (parsed.pushes > 1));
    } else {
        assert(parsed.pushes == 0);
        assert(parsed.data_size + 1 == script.size());
    }

    // The flags agree with the tree's own reading of the script.
    const CScript cscript(script.begin(), script.end());
    assert(((parsed.flags & ROW_PUSH_ONLY) != 0) == cscript.IsPushOnly(cscript.begin() + 1));
    bool complete = true;
    bool number = false;
    std::vector<unsigned char> pushed;
    for (CScript::const_iterator pc = cscript.begin() + 1; pc < cscript.end();) {
        opcodetype op;
        std::vector<unsigned char> item;
        if (!cscript.GetOp(pc, op, item)) {
            complete = false;
            break;
        }
        pushed.insert(pushed.end(), item.begin(), item.end());
        if (op == OP_1NEGATE || (op >= OP_1 && op <= OP_16)) number = true;
    }
    assert(((parsed.flags & ROW_PARSE_ERROR) != 0) == !complete);
    if (parsed.flags & ROW_PUSH_ONLY) {
        assert(parsed.data == pushed);
        assert(((parsed.flags & ROW_NUMBER_PUSH) != 0) == number);
    }

    // Asking for less changes no fact.
    const ParsedScript facts = ParseOpReturn(script, PARSE_FACTS);
    assert(facts.flags == parsed.flags && facts.data_size == parsed.data_size && facts.pushes == parsed.pushes);
    assert(facts.data.empty() && facts.parts.empty());

    // A prefix read with the whole script's flags gives the whole result cut at the prefix, never more.
    const size_t n = script.size();
    for (const size_t size : {size_t{0}, size_t{1}, size_t{2}, n / 3, n / 2, n - 1, n, size_t{4096}, size_t{65536}}) {
        if (size > n) continue;
        const Span<const unsigned char> prefix = Span<const unsigned char>{script}.first(size);
        const ParsedScript cut = ParseOpReturnPrefix(prefix, parsed.flags, PARSE_DATA | PARSE_PARTS);
        assert(cut.flags == parsed.flags);
        assert(cut.parts == CutParts(parsed, size));
        assert(cut.data_size <= parsed.data_size && cut.data.size() == cut.data_size);
        assert(std::equal(cut.data.begin(), cut.data.end(), parsed.data.begin()));
    }

    // As an output of a coinbase, around another output, the witness commitment is the one consensus reads.
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vout.emplace_back(0, cscript);
    coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
    coinbase.vout.emplace_back(0, cscript);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    assert(WitnessCommitmentIndex(*block.vtx[0]) == GetWitnessCommitmentIndex(block));
}
