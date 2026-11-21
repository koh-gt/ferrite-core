// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_OPRETURN_SCRIPT_H
#define BITCOIN_OPRETURN_SCRIPT_H

#include <span.h>

#include <cstddef>
#include <cstdint>
#include <vector>

class CTransaction;

/**
 * The one parser of OP_RETURN scripts. An OP_RETURN script is an output script that is not empty and whose first
 * byte is OP_RETURN. Its data is the bytes of every data push after that byte, joined: the pushes of 1 to 75
 * bytes (opcodes 0x01 to 0x4b) and OP_PUSHDATA1, OP_PUSHDATA2 and OP_PUSHDATA4. OP_0 adds nothing, and neither do
 * OP_1NEGATE, OP_RESERVED and OP_1 to OP_16. A script that is not push-only (an opcode above OP_16 appears, or a
 * push runs past the end) is not read as pushes: its data is every byte after OP_RETURN.
 */
namespace opreturn {

/**
 * Structural facts about an OP_RETURN script, stored with every row. They describe how the script is built and
 * never depend on how its data would be shown.
 */
enum RowFlag : uint16_t {
    //! Every opcode after OP_RETURN is a push (an opcode up to OP_16) and every push is complete. This is what
    //! CScript::IsPushOnly says of the script after its first byte.
    ROW_PUSH_ONLY = 0x0001,
    //! A push runs past the end of the script.
    ROW_PARSE_ERROR = 0x0002,
    //! The data has no bytes.
    ROW_EMPTY = 0x0004,
    //! The data is well-formed UTF-8 (RFC 3629): no overlong forms, no surrogates, nothing above U+10FFFF. Data
    //! without bytes is well-formed.
    ROW_UTF8 = 0x0008,
    //! A push-only script with more than one data push. Only push-only scripts have it, because only their data
    //! is read as pushes.
    ROW_MULTI_PUSH = 0x0010,
    //! A push-only script in which OP_1NEGATE or one of OP_1 to OP_16 appears; those values are not in the data.
    ROW_NUMBER_PUSH = 0x0020,
    //! The script is longer than 83 bytes, the relay limit of earlier releases.
    ROW_OVER_83 = 0x0040,
};

//! Every row flag.
static constexpr uint16_t ROW_FLAGS_ALL{0x007f};

//! Scripts longer than this have ROW_OVER_83.
static constexpr size_t OVER_83_SIZE{83};

//! Whether a script is an OP_RETURN script: not empty, and its first byte is OP_RETURN.
bool IsOpReturn(Span<const unsigned char> script);

//! A part of a script that belongs to the data: `size` bytes from `offset`.
struct DataPart {
    size_t offset{0};
    size_t size{0};

    friend bool operator==(const DataPart& a, const DataPart& b) { return a.offset == b.offset && a.size == b.size; }
    friend bool operator!=(const DataPart& a, const DataPart& b) { return !(a == b); }
};

//! What the parser finds in a script.
struct ParsedScript {
    //! RowFlag bits; none for a script that is not an OP_RETURN script.
    uint16_t flags{0};
    //! The size of the data.
    size_t data_size{0};
    //! The number of data pushes of a push-only script (pushes of no bytes included); 0 for any other script.
    size_t pushes{0};
    //! The data, when asked for (PARSE_DATA).
    std::vector<unsigned char> data;
    //! Where the data lies in the script, when asked for (PARSE_PARTS): one part per data push of a push-only
    //! script, in order, parts of no bytes included; otherwise one part holding every byte after OP_RETURN.
    std::vector<DataPart> parts;
};

//! What the parser returns besides the flags, the data size and the number of pushes.
enum ParseOutput : uint8_t {
    PARSE_FACTS = 0, //!< nothing more: what a row stores
    PARSE_DATA = 1,  //!< the data
    PARSE_PARTS = 2, //!< where the data lies
};

/** Parses a whole OP_RETURN script. `output` is a combination of ParseOutput values. */
ParsedScript ParseOpReturn(Span<const unsigned char> script, uint8_t output = PARSE_DATA);

/**
 * Parses the beginning of a longer OP_RETURN script, given the flags ParseOpReturn gave for the whole script (every
 * row and every unconfirmed output keeps them). The result is the whole script's result cut at the end of the
 * prefix: its flags are `whole_flags`, its data is the beginning of the whole script's data, and its parts are the
 * whole script's parts whose push header lies in the prefix, shortened to the prefix. For a script that is not
 * push-only that is every byte of the prefix after OP_RETURN. A prefix that contradicts the flags (an opcode above
 * OP_16 in a script they call push-only) is read as a script that is not push-only; one whose first byte is not
 * OP_RETURN gives nothing and no flags.
 */
ParsedScript ParseOpReturnPrefix(Span<const unsigned char> prefix, uint16_t whole_flags, uint8_t output = PARSE_DATA);

//! Whether bytes are well-formed UTF-8 (RFC 3629).
bool IsUtf8(Span<const unsigned char> bytes);

/**
 * Whether flags, a script size and a data size pass these checks, which ParseOpReturn's result for every OP_RETURN
 * script passes: no unknown flag, ROW_OVER_83 and ROW_EMPTY as the sizes say, never ROW_PUSH_ONLY with
 * ROW_PARSE_ERROR, ROW_MULTI_PUSH and ROW_NUMBER_PUSH only with ROW_PUSH_ONLY, ROW_UTF8 with ROW_EMPTY, the data of
 * a script that is not push-only every byte after OP_RETURN, and data of a push-only script at least a push header
 * shorter. Values that pass need not come from any script: the flags of OP_RETURN alone with ROW_MULTI_PUSH added
 * pass.
 */
bool FlagsFit(uint16_t flags, size_t script_size, size_t data_size);

/**
 * The output that consensus reads as the witness commitment of a block whose coinbase is `tx`: the last output of
 * at least 38 bytes that begins with OP_RETURN and the bytes 24 aa 21 a9 ed, as GetWitnessCommitmentIndex finds
 * it. It is consensus data, not a row. -1 when `tx` is not a coinbase or has no such output.
 */
int WitnessCommitmentIndex(const CTransaction& tx);

} // namespace opreturn

#endif // BITCOIN_OPRETURN_SCRIPT_H
