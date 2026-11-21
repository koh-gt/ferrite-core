// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/script.h>

#include <consensus/validation.h>
#include <primitives/transaction.h>
#include <script/script.h>

namespace opreturn {
namespace {

//! Checks UTF-8 a piece at a time, so that data in several parts needs no copy.
class Utf8Checker
{
    uint32_t m_code{0}; //!< the code point read so far
    uint32_t m_min{0};  //!< the smallest code point the current sequence may encode
    int m_needed{0};    //!< continuation bytes still to come
    bool m_bad{false};

public:
    void Feed(Span<const unsigned char> bytes)
    {
        for (const unsigned char c : bytes) {
            if (m_bad) return;
            if (m_needed == 0) {
                if (c < 0x80) continue;
                if (c >= 0xc2 && c <= 0xdf) {
                    m_needed = 1;
                    m_code = c & 0x1f;
                    m_min = 0x80;
                } else if (c >= 0xe0 && c <= 0xef) {
                    m_needed = 2;
                    m_code = c & 0x0f;
                    m_min = 0x800;
                } else if (c >= 0xf0 && c <= 0xf4) {
                    m_needed = 3;
                    m_code = c & 0x07;
                    m_min = 0x10000;
                } else {
                    // A continuation byte without a lead byte, or a byte that never occurs in UTF-8: 0xc0, 0xc1
                    // and 0xf5 to 0xff.
                    m_bad = true;
                }
                continue;
            }
            if ((c & 0xc0) != 0x80) {
                m_bad = true;
                return;
            }
            m_code = (m_code << 6) | (c & 0x3f);
            if (--m_needed == 0) {
                const bool overlong = m_code < m_min;
                const bool surrogate = m_code >= 0xd800 && m_code <= 0xdfff;
                if (overlong || surrogate || m_code > 0x10ffff) m_bad = true;
            }
        }
    }

    bool Valid() const { return !m_bad && m_needed == 0; }
};

//! What a walk over the opcodes of a script found.
struct Walk {
    bool non_push{false}; //!< an opcode above OP_16 appears
    bool cut{false};      //!< a push runs past the end
    bool number{false};   //!< OP_1NEGATE or one of OP_1 to OP_16 appears
};

/**
 * Walks the opcodes after the first byte as the script interpreter reads them (an opcode above OP_16 has no
 * operand, a push is followed by its length and its bytes) and calls on_push(offset, size) for each data push
 * before the first opcode above OP_16. A push that runs past the end ends the walk; in prefix mode the script is
 * the beginning of a longer one, and such a push is reported with what the prefix holds of it when its header
 * lies in the prefix.
 */
template <typename OnPush>
Walk WalkOpcodes(Span<const unsigned char> script, bool prefix, OnPush&& on_push)
{
    Walk walk;
    const size_t size = script.size();
    size_t pc = 1;
    while (pc < size) {
        const unsigned char op = script[pc++];
        if (op <= OP_PUSHDATA4) {
            size_t len = op;
            if (op >= OP_PUSHDATA1) {
                const size_t width = op == OP_PUSHDATA1 ? 1 : op == OP_PUSHDATA2 ? 2 : 4;
                if (size - pc < width) {
                    walk.cut = true;
                    break;
                }
                len = 0;
                for (size_t i = 0; i < width; ++i) len |= size_t{script[pc + i]} << (8 * i);
                pc += width;
            }
            if (size - pc < len) {
                if (prefix && !walk.non_push) on_push(pc, size - pc);
                walk.cut = true;
                break;
            }
            if (!walk.non_push && op != OP_0) on_push(pc, len);
            pc += len;
        } else if (op <= OP_16) {
            if (op != OP_RESERVED) walk.number = true;
        } else {
            walk.non_push = true;
        }
    }
    return walk;
}

//! Fills `out` with the data of a script that is not read as pushes: every byte after the first.
void SetUnparsed(Span<const unsigned char> script, uint8_t output, ParsedScript& out)
{
    const Span<const unsigned char> rest = script.subspan(1);
    out.data_size = rest.size();
    out.pushes = 0;
    out.data.clear();
    out.parts.clear();
    if (output & PARSE_DATA) out.data.assign(rest.begin(), rest.end());
    if ((output & PARSE_PARTS) && !rest.empty()) out.parts.push_back({1, rest.size()});
}

} // namespace

bool IsOpReturn(Span<const unsigned char> script)
{
    return !script.empty() && script[0] == OP_RETURN;
}

bool IsUtf8(Span<const unsigned char> bytes)
{
    Utf8Checker checker;
    checker.Feed(bytes);
    return checker.Valid();
}

ParsedScript ParseOpReturn(Span<const unsigned char> script, uint8_t output)
{
    ParsedScript out;
    if (!IsOpReturn(script)) return out;
    Utf8Checker utf8;
    const Walk walk = WalkOpcodes(script, /*prefix=*/false, [&](size_t offset, size_t size) {
        const Span<const unsigned char> bytes = script.subspan(offset, size);
        utf8.Feed(bytes);
        out.data_size += size;
        ++out.pushes;
        if (output & PARSE_DATA) out.data.insert(out.data.end(), bytes.begin(), bytes.end());
        if (output & PARSE_PARTS) out.parts.push_back({offset, size});
    });
    if (!walk.non_push && !walk.cut) {
        out.flags |= ROW_PUSH_ONLY;
        if (out.pushes > 1) out.flags |= ROW_MULTI_PUSH;
        if (walk.number) out.flags |= ROW_NUMBER_PUSH;
        if (utf8.Valid()) out.flags |= ROW_UTF8;
    } else {
        SetUnparsed(script, output, out);
        if (IsUtf8(script.subspan(1))) out.flags |= ROW_UTF8;
    }
    if (walk.cut) out.flags |= ROW_PARSE_ERROR;
    if (out.data_size == 0) out.flags |= ROW_EMPTY;
    if (script.size() > OVER_83_SIZE) out.flags |= ROW_OVER_83;
    return out;
}

ParsedScript ParseOpReturnPrefix(Span<const unsigned char> prefix, uint16_t whole_flags, uint8_t output)
{
    ParsedScript out;
    if (!prefix.empty() && prefix[0] != OP_RETURN) return out;
    out.flags = whole_flags;
    if (prefix.empty()) return out;
    if (whole_flags & ROW_PUSH_ONLY) {
        const Walk walk = WalkOpcodes(prefix, /*prefix=*/true, [&](size_t offset, size_t size) {
            const Span<const unsigned char> bytes = prefix.subspan(offset, size);
            out.data_size += size;
            ++out.pushes;
            if (output & PARSE_DATA) out.data.insert(out.data.end(), bytes.begin(), bytes.end());
            if (output & PARSE_PARTS) out.parts.push_back({offset, size});
        });
        if (!walk.non_push) return out;
    }
    SetUnparsed(prefix, output, out);
    return out;
}

bool FlagsFit(uint16_t flags, size_t script_size, size_t data_size)
{
    if (script_size == 0 || (flags & ~ROW_FLAGS_ALL) != 0) return false;
    if (((flags & ROW_OVER_83) != 0) != (script_size > OVER_83_SIZE)) return false;
    if (((flags & ROW_EMPTY) != 0) != (data_size == 0)) return false;
    if ((flags & ROW_EMPTY) && !(flags & ROW_UTF8)) return false;
    if (flags & ROW_PUSH_ONLY) {
        if (flags & ROW_PARSE_ERROR) return false;
        // Every data byte comes after OP_RETURN and a push header.
        return data_size == 0 || data_size + 2 <= script_size;
    }
    // Not push-only: an opcode or a push header follows OP_RETURN, and the data is every byte after OP_RETURN.
    if (flags & (ROW_MULTI_PUSH | ROW_NUMBER_PUSH)) return false;
    return script_size >= 2 && data_size + 1 == script_size;
}

int WitnessCommitmentIndex(const CTransaction& tx)
{
    if (!tx.IsCoinBase()) return NO_WITNESS_COMMITMENT;
    int found = NO_WITNESS_COMMITMENT;
    for (size_t o = 0; o < tx.vout.size(); ++o) {
        const CScript& script = tx.vout[o].scriptPubKey;
        if (script.size() >= MINIMUM_WITNESS_COMMITMENT && script[0] == OP_RETURN && script[1] == 0x24 &&
            script[2] == 0xaa && script[3] == 0x21 && script[4] == 0xa9 && script[5] == 0xed) {
            found = int(o);
        }
    }
    return found;
}

} // namespace opreturn
