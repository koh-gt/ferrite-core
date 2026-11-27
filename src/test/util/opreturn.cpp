// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/opreturn.h>

#include <interfaces/handler.h>
#include <logging.h>
#include <opreturn/coverage.h>
#include <opreturn/format.h>
#include <opreturn/names.h>
#include <opreturn/script.h>
#include <script/script.h>
#include <uint256.h>

#include <algorithm>
#include <iterator>
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

FakeRecord::FakeRecord()
{
    Show({});
}

void FakeRecord::AddRow(const RowKey& key, const std::vector<unsigned char>& script, const TxBrief& tx)
{
    RowView row;
    row.key = key;
    const ParsedScript parsed = ParseOpReturn(script, PARSE_FACTS);
    row.flags = parsed.flags;
    row.data_size = uint32_t(parsed.data_size);
    row.tx = tx;
    AddView(std::move(row), script);
}

void FakeRecord::AddView(RowView row, const std::vector<unsigned char>& script)
{
    row.script_size = uint32_t(script.size());
    row.script_prefix = script;
    LOCK(m_mutex);
    const RowKey key = row.key;
    m_rows[key] = Entry{std::move(row), script};
}

void FakeRecord::Show(std::vector<HeightRange> coverage, int32_t floor, int32_t live_tip)
{
    auto v = std::make_shared<Visibility>();
    HeightSet set;
    if (!HeightSet::FromRanges(coverage, set)) throw std::invalid_argument("FakeRecord::Show: ranges not canonical");
    if (live_tip < 0 && !set.Empty()) live_tip = set.Ranges().back().hi;
    v->floor = floor;
    v->live_tip = live_tip;
    if (live_tip >= 0) v->live_tip_hash = uint256::ONE;
    auto gaps = std::make_shared<std::vector<GapRange>>();
    if (live_tip >= 0) {
        for (const HeightRange& range : set.Missing({std::max(floor, 0), live_tip})) {
            gaps->push_back({range.lo, range.hi, GapReason::PENDING});
        }
    }
    v->coverage = std::make_shared<const std::vector<HeightRange>>(set.Ranges());
    v->gaps = gaps;
    LOCK(m_mutex);
    v->removals = m_removals;
    v->generation = ++m_generation;
    m_visibility = v;
}

void FakeRecord::RaiseRemovals()
{
    LOCK(m_mutex);
    ++m_removals;
}

void FakeRecord::FailRow(const RowKey& key)
{
    LOCK(m_mutex);
    m_fail_rows.emplace(key.height, key.n);
}

void FakeRecord::FailScript(const RowKey& key)
{
    LOCK(m_mutex);
    m_fail_scripts.emplace(key.height, key.n);
}

void FakeRecord::BeforeRead(std::function<void(const VisibilityRef&)> fn)
{
    LOCK(m_mutex);
    m_before_read = std::move(fn);
}

std::vector<VisibilityRef> FakeRecord::ReadSnapshots() const
{
    LOCK(m_mutex);
    return m_read_snapshots;
}

uint64_t FakeRecord::ReadErrors() const
{
    LOCK(m_mutex);
    return m_read_errors;
}

VisibilityRef FakeRecord::Use(const VisibilityRef& given)
{
    std::function<void(const VisibilityRef&)> before;
    VisibilityRef v;
    {
        LOCK(m_mutex);
        before = m_before_read;
        v = given ? given : m_visibility;
    }
    if (before) before(v);
    LOCK(m_mutex);
    m_read_snapshots.push_back(v);
    return v;
}

RowView FakeRecord::View(const Entry& entry, size_t max_script)
{
    RowView row = entry.row;
    row.script_prefix.assign(entry.script.begin(),
                             entry.script.begin() + std::min({entry.script.size(), max_script, size_t{SCRIPT_HEAD_SIZE}}));
    return row;
}

RecordStatus FakeRecord::getStatus()
{
    LOCK(m_mutex);
    RecordStatus status;
    status.state = State::RUNNING;
    status.live_tip = m_visibility->live_tip;
    status.live_tip_hash = m_visibility->live_tip_hash;
    status.floor = m_visibility->floor;
    status.coverage = m_visibility->coverage;
    status.rows = m_rows.size();
    status.read_errors = m_read_errors;
    status.generation = m_visibility->generation;
    return status;
}

VisibilityRef FakeRecord::getVisibility()
{
    LOCK(m_mutex);
    return m_visibility;
}

uint64_t FakeRecord::getRemovals()
{
    LOCK(m_mutex);
    return m_removals;
}

RowBatch FakeRecord::readRows(const RowRequest& request)
{
    const VisibilityRef v = Use(request.visibility);
    RowBatch out;
    out.next = request.from;
    out.generation = v ? v->generation : 0;
    if (!v || !v->coverage) {
        out.end = true;
        return out;
    }
    if (request.max_rows == 0) return out;
    const int32_t lowest = std::max({request.heights.lo, v->floor, 0});
    const int32_t highest = std::min(request.heights.hi, v->live_tip);
    LOCK(m_mutex);
    size_t bytes = 0;
    // Adds a row to the batch unless the batch is full; false then, or when the row's read fails.
    const auto take = [&](const Entry& entry) {
        const RowKey& key = entry.row.key;
        if (out.rows.size() >= request.max_rows) return false;
        if (m_fail_rows.count({key.height, key.n})) {
            out.failed = true;
            ++m_read_errors;
            return false;
        }
        const size_t size = std::min(entry.script.size(), std::min(request.max_script, size_t{SCRIPT_HEAD_SIZE})) +
                            entry.row.tx.paid_by.size();
        if (!out.rows.empty() && bytes + size > request.max_bytes) return false;
        bytes += size;
        out.rows.push_back(View(entry, request.max_script));
        out.next = key;
        return true;
    };
    const auto shown = [&](const RowKey& key) {
        return key.height >= lowest && key.height <= highest && v->IsVisible(key.height);
    };
    if (!request.older) {
        for (auto it = m_rows.upper_bound(request.from); it != m_rows.end(); ++it) {
            if (it->first.height > highest) break;
            if (!shown(it->first)) continue;
            if (!take(it->second)) return out;
        }
    } else {
        for (auto it = std::make_reverse_iterator(m_rows.lower_bound(request.from)); it != m_rows.rend(); ++it) {
            if (it->first.height < lowest) break;
            if (!shown(it->first)) continue;
            if (!take(it->second)) return out;
        }
    }
    out.end = true;
    return out;
}

ReadResult FakeRecord::readRow(const RowKey& key, RowView& out, const VisibilityRef& visibility)
{
    const VisibilityRef v = Use(visibility);
    LOCK(m_mutex);
    if (m_fail_rows.count({key.height, key.n})) {
        ++m_read_errors;
        return ReadResult::FAILED;
    }
    const auto it = m_rows.find(key);
    if (!v || !v->IsVisible(key.height) || it == m_rows.end()) return ReadResult::NOT_FOUND;
    out = View(it->second, SCRIPT_HEAD_SIZE);
    return ReadResult::FOUND;
}

ReadResult FakeRecord::readScript(const RowKey& key, size_t offset, size_t max_bytes, std::vector<unsigned char>& out,
                                  const VisibilityRef& visibility)
{
    const VisibilityRef v = Use(visibility);
    out.clear();
    LOCK(m_mutex);
    if (m_fail_rows.count({key.height, key.n}) || m_fail_scripts.count({key.height, key.n})) {
        ++m_read_errors;
        return ReadResult::FAILED;
    }
    const auto it = m_rows.find(key);
    if (!v || !v->IsVisible(key.height) || it == m_rows.end()) return ReadResult::NOT_FOUND;
    const std::vector<unsigned char>& script = it->second.script;
    if (offset < script.size()) {
        out.assign(script.begin() + offset, script.begin() + offset + std::min(max_bytes, script.size() - offset));
    }
    return ReadResult::FOUND;
}

ReadResult FakeRecord::readTransaction(const TxKey&, size_t, size_t, TxView&, const VisibilityRef& visibility)
{
    Use(visibility);
    return ReadResult::NOT_FOUND;
}

ReadResult FakeRecord::readBlock(int32_t, BlockView&, const VisibilityRef& visibility)
{
    Use(visibility);
    return ReadResult::NOT_FOUND;
}

ReadResult FakeRecord::findTransaction(const uint256&, TxKey&, const VisibilityRef& visibility)
{
    Use(visibility);
    return ReadResult::NOT_FOUND;
}

ReadResult FakeRecord::findHeightForTime(int64_t, int32_t&, const VisibilityRef& visibility)
{
    Use(visibility);
    return ReadResult::NOT_FOUND;
}

bool FakeRecord::waitForTip(int)
{
    return false;
}

void FakeRecord::setRecordEnabled(bool) {}

void FakeRecord::requestRebuild() {}

std::unique_ptr<interfaces::Handler> FakeRecord::handleChanged(std::function<void(const RecordChange&)>)
{
    return interfaces::MakeHandler(std::function<void()>{[] {}});
}

} // namespace test
} // namespace opreturn
