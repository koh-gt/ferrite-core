// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/opreturn.h>

#include <logging.h>
#include <opreturn/names.h>
#include <script/script.h>
#include <uint256.h>

#include <random>
#include <stdexcept>

namespace opreturn {
namespace test {

const std::string HOSTILE_MARKER{"zqhostilemarkerzq"};

std::vector<unsigned char> OneDataPush(Span<const unsigned char> data)
{
    std::vector<unsigned char> script{OP_RETURN};
    const size_t size = data.size();
    if (size < OP_PUSHDATA1) {
        script.push_back(uint8_t(size));
    } else if (size <= 0xff) {
        script.push_back(OP_PUSHDATA1);
        script.push_back(uint8_t(size));
    } else if (size <= 0xffff) {
        script.push_back(OP_PUSHDATA2);
        for (int i = 0; i < 2; ++i) script.push_back(uint8_t(size >> (8 * i)));
    } else {
        script.push_back(OP_PUSHDATA4);
        for (int i = 0; i < 4; ++i) script.push_back(uint8_t(size >> (8 * i)));
    }
    script.insert(script.end(), data.begin(), data.end());
    return script;
}

std::vector<unsigned char> MakeScript(size_t size, int fill, uint32_t seed)
{
    if (size < 1) throw std::invalid_argument("an OP_RETURN script has at least one byte");
    std::vector<unsigned char> script{OP_RETURN};
    if (size == 1) return script;
    // The smallest push header that leaves exactly the rest for the data: 1 byte for up to 75 data bytes, then 2,
    // 3 and 5 bytes for OP_PUSHDATA1, OP_PUSHDATA2 and OP_PUSHDATA4.
    size_t header = 0;
    size_t data = 0;
    for (const size_t h : {size_t{1}, size_t{2}, size_t{3}, size_t{5}}) {
        if (size < 1 + h) break;
        const size_t limit = h == 1 ? 75 : h == 2 ? 0xff : h == 3 ? 0xffff : SIZE_MAX;
        if (size - 1 - h <= limit) {
            header = h;
            data = size - 1 - h;
            break;
        }
    }
    if (header == 1) {
        script.push_back(uint8_t(data)); // OP_0 when there is no data
    } else if (header == 2) {
        script.push_back(OP_PUSHDATA1);
        script.push_back(uint8_t(data));
    } else if (header == 3) {
        script.push_back(OP_PUSHDATA2);
        for (int i = 0; i < 2; ++i) script.push_back(uint8_t(data >> (8 * i)));
    } else {
        script.push_back(OP_PUSHDATA4);
        for (int i = 0; i < 4; ++i) script.push_back(uint8_t(data >> (8 * i)));
    }
    if (fill >= 0) {
        script.resize(script.size() + data, uint8_t(fill));
    } else {
        std::mt19937 rng(seed);
        for (size_t i = 0; i < data; ++i) script.push_back(uint8_t(rng()));
    }
    if (script.size() != size) throw std::logic_error("MakeScript");
    return script;
}

CMutableTransaction MakeOpReturnTx(const std::vector<std::vector<unsigned char>>& scripts, bool coinbase)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    if (coinbase) {
        tx.vin[0].prevout.SetNull();
        tx.vin[0].scriptSig = CScript() << OP_0 << OP_0;
    } else {
        tx.vin[0].prevout = COutPoint(uint256::ONE, 0);
    }
    for (const std::vector<unsigned char>& script : scripts) {
        tx.vout.emplace_back(0, CScript(script.begin(), script.end()));
    }
    return tx;
}

std::vector<HostileSample> HostileCorpus()
{
    const std::string m = HOSTILE_MARKER;
    const auto sample = [](std::string name, const std::string& text, bool utf8) {
        return HostileSample{std::move(name), std::vector<unsigned char>(text.begin(), text.end()), utf8};
    };
    return {
        sample("markup", "<script>alert(1)</script>" + m, true),
        sample("link", "<a href=\"javascript:void(0)\">" + m + "</a>", true),
        sample("terminal escapes", "\x1b[2J\x1b[H" + m + "\x07", true),
        sample("terminal link", "\x1b]8;;ferrite:example\x1b\\" + m + "\x1b]8;;\x1b\\", true),
        sample("direction override", "\xe2\x80\xae" + m + "\xe2\x80\xac", true),
        sample("direction isolate", "\xe2\x81\xa6" + m + "\xe2\x81\xa9", true),
        sample("invisible", "\xe2\x80\x8b" + m + "\xe2\x80\x8d\xef\xbb\xbf", true),
        sample("nul", m + std::string(1, '\0') + m, true),
        sample("forged log line", m + "\r\n" + std::string(LOG_PREFIX) + " state FAILED\n", true),
        sample("format directives", "%s%n%x%d" + m, true),
        sample("overlong", m + "\xc0\xaf", false),
        sample("surrogate", m + "\xed\xa0\x80", false),
        sample("above U+10FFFF", m + "\xf4\x90\x80\x80", false),
        sample("lone continuation byte", m + "\x80", false),
        sample("cut sequence", m + "\xe2\x82", false),
    };
}

LogLines::LogLines()
{
    m_callback = LogInstance().PushBackCallback([this](const std::string& line) {
        LOCK(m_mutex);
        m_lines.push_back(line);
    });
}

LogLines::~LogLines()
{
    LogInstance().DeleteCallback(m_callback);
}

std::vector<std::string> LogLines::Lines() const
{
    LOCK(m_mutex);
    return m_lines;
}

std::vector<std::string> LogLines::Containing(const std::string& text) const
{
    std::vector<std::string> found;
    for (const std::string& line : Lines()) {
        if (line.find(text) != std::string::npos) found.push_back(line);
    }
    return found;
}

} // namespace test
} // namespace opreturn
