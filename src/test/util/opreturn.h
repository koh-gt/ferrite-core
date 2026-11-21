// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_OPRETURN_H
#define BITCOIN_TEST_UTIL_OPRETURN_H

#include <primitives/transaction.h>
#include <span.h>
#include <sync.h>

#include <cstdint>
#include <functional>
#include <list>
#include <string>
#include <vector>

namespace opreturn {
namespace test {

//! OP_RETURN and one push of `data`, with the smallest push opcode that holds it.
std::vector<unsigned char> OneDataPush(Span<const unsigned char> data);

/**
 * An OP_RETURN script of exactly `size` bytes (at least 1): one push of the smallest kind that gives that size,
 * whose data is the byte `fill` repeated, or bytes made from `seed` when `fill` is negative. A script of one byte is
 * OP_RETURN alone, and one of two bytes OP_RETURN OP_0.
 */
std::vector<unsigned char> MakeScript(size_t size, int fill, uint32_t seed = 0);

//! A transaction whose outputs have the given scripts and the value 0: a coinbase, or a transaction with one input.
CMutableTransaction MakeOpReturnTx(const std::vector<std::vector<unsigned char>>& scripts, bool coinbase = false);

//! A sample of bytes that anyone can write into an OP_RETURN output.
struct HostileSample {
    std::string name;
    std::vector<unsigned char> bytes;
    bool utf8; //!< whether the bytes are well-formed UTF-8
};

/**
 * Bytes that must never reach a log line or an error message: markup, a link, terminal escape sequences, direction
 * controls, invisible characters, a NUL, line breaks with a forged log line, format directives, and malformed
 * UTF-8. Each sample holds HOSTILE_MARKER, so that a test can search for it.
 */
std::vector<HostileSample> HostileCorpus();

//! The text every hostile sample holds.
extern const std::string HOSTILE_MARKER;

//! Collects every line written to the debug log while it exists.
class LogLines
{
    mutable Mutex m_mutex;
    std::vector<std::string> m_lines GUARDED_BY(m_mutex);
    std::list<std::function<void(const std::string&)>>::iterator m_callback;

public:
    LogLines();
    ~LogLines();
    LogLines(const LogLines&) = delete;
    LogLines& operator=(const LogLines&) = delete;

    std::vector<std::string> Lines() const;
    //! The lines that contain `text`.
    std::vector<std::string> Containing(const std::string& text) const;
};

} // namespace test
} // namespace opreturn

#endif // BITCOIN_TEST_UTIL_OPRETURN_H
