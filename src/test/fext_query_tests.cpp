// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fext/query.h>

#include <amount.h>
#include <fext/text.h>
#include <interfaces/opreturn.h>
#include <opreturn/format.h>
#include <opreturn/options.h>
#include <opreturn/record.h>
#include <opreturn/script.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <util/system.h>
#include <validation.h>
#include <validationinterface.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fext;
using opreturn::HeightRange;
using opreturn::ReadResult;
using opreturn::RowKey;
using opreturn::RowView;
using opreturn::TxBrief;
using opreturn::VisibilityRef;
using opreturn::test::FakeRecord;
using opreturn::test::OneDataPush;

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::chrono::milliseconds LONG_SLICE{60000};

std::vector<unsigned char> Bytes(const std::string& text)
{
    return std::vector<unsigned char>(text.begin(), text.end());
}

//! An OP_RETURN script whose data is the text.
std::vector<unsigned char> TextScript(const std::string& text)
{
    return OneDataPush(Bytes(text));
}

//! The folded text of a message with this text.
std::vector<uint32_t> Folded(const std::string& text)
{
    const std::vector<unsigned char> script = TextScript(text);
    return SearchText(script, opreturn::ParseOpReturn(script, opreturn::PARSE_FACTS).flags);
}

MatchResult Find(const std::string& message, const std::string& typed)
{
    return FindFolded(Folded(message), TextNeedle(typed));
}

//! A brief of a transaction that a coin with `script` paid for.
TxBrief PaidWith(const CScript& script, CAmount fee = -1, uint32_t vsize = 0)
{
    TxBrief tx;
    tx.paid_by_kind = opreturn::PaidByKind::SCRIPT;
    tx.paid_by = script;
    tx.fee = fee;
    tx.vsize = vsize;
    tx.n_rows = 1;
    return tx;
}

RowView RowOf(const std::vector<unsigned char>& script, const TxBrief& tx = {})
{
    RowView row;
    const opreturn::ParsedScript parsed = opreturn::ParseOpReturn(script, opreturn::PARSE_FACTS);
    row.flags = parsed.flags;
    row.script_size = uint32_t(script.size());
    row.data_size = uint32_t(parsed.data_size);
    row.script_prefix = script;
    row.tx = tx;
    return row;
}

std::shared_ptr<const TextNeedle> Needle(const std::string& typed)
{
    return std::make_shared<const TextNeedle>(typed);
}

//! Every slice of a scan from `request`, joined, until it ends; each slice must neither fail nor be stale.
std::vector<RowKey> ScanAll(interfaces::OpReturnRecord& record, ScanRequest request, const RowFilter& filter,
                            size_t* slices = nullptr)
{
    std::vector<RowKey> found;
    for (size_t i = 0; i < 100000; ++i) {
        const ScanResult result = Scan(record, request, filter);
        BOOST_REQUIRE(!result.failed && !result.stale && !result.cancelled);
        found.insert(found.end(), result.found.begin(), result.found.end());
        if (slices) ++*slices;
        if (result.end) return found;
        BOOST_REQUIRE(result.rows > 0);
        request.from = result.next;
    }
    BOOST_FAIL("the scan did not end");
    return found;
}

//! A record of rows at heights 10 to 14, three in each; at each height row 0 is a text, row 1 binary data and row 2
//! the text "news <height>".
struct Rows {
    FakeRecord record;
    std::vector<RowKey> all; //!< in chain order

    Rows()
    {
        for (int32_t h = 10; h <= 14; ++h) {
            record.AddRow({h, 0}, TextScript(strprintf("hello from %d", h)));
            record.AddRow({h, 1}, std::vector<unsigned char>{OP_RETURN, 0x02, 0xff, 0xfe});
            record.AddRow({h, 2}, TextScript(strprintf("news %d", h)));
            for (uint32_t n = 0; n < 3; ++n) all.push_back({h, n});
        }
        record.Show({{10, 14}});
    }
};

std::vector<RowKey> Reversed(std::vector<RowKey> keys)
{
    std::reverse(keys.begin(), keys.end());
    return keys;
}

std::string Keys(const std::vector<RowKey>& keys)
{
    std::string text;
    for (const RowKey& key : keys) text += strprintf("(%d,%u)", key.height, key.n);
    return text;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(fext_query_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(needle_folds_as_messages)
{
    // The typed text is folded as the messages are: letter case, invisible characters and white space characters do
    // not keep a word from being found.
    BOOST_CHECK(TextNeedle("Scam Alert").Folded() == Folded("scam alert"));
    BOOST_CHECK(Find("SC\xe2\x80\x8b" "AM\xc2\xa0" "alert", "scam alert") == MatchResult::MATCH);
    BOOST_CHECK(Find("s\xe2\x80\x8e" "cam", "scam") == MatchResult::MATCH);
    BOOST_CHECK(Find("scam\talert", "scam alert") == MatchResult::MATCH);
    BOOST_CHECK(Find("sc am", "scam") == MatchResult::NO_MATCH);
    // Text hidden in tag characters is never found: the folding drops them.
    BOOST_CHECK(Find("hi \xf0\x9f\x8f\xb4\xf3\xa0\x81\xb3\xf3\xa0\x81\xa3\xf3\xa0\x81\xa1\xf3\xa0\x81\xad\xf3\xa0\x81\xbf", "scam") ==
                MatchResult::NO_MATCH);
    // An ill-formed sequence is U+FFFD in the typed text as in a message.
    BOOST_CHECK(TextNeedle("\xff").Folded() == std::vector<uint32_t>{REPLACEMENT_CHARACTER});
    BOOST_CHECK(Find("a\xff" "b", "\xfe") == MatchResult::MATCH);
    // A needle that folds to nothing occurs in every text.
    BOOST_CHECK(TextNeedle("").Empty());
    BOOST_CHECK(TextNeedle("\xe2\x80\x8b\xe2\x80\x8c").Empty());
    BOOST_CHECK(FindFolded({}, TextNeedle("")) == MatchResult::MATCH);
    BOOST_CHECK(FindFolded({}, TextNeedle("a")) == MatchResult::NO_MATCH);
}

BOOST_AUTO_TEST_CASE(find_folded_linear)
{
    // The same answer as a search at every position, and at most two steps per code point of the text.
    for (int i = 0; i < 4000; ++i) {
        std::string text(InsecureRandRange(80), 'a');
        for (char& c : text) c = char('a' + InsecureRandRange(3));
        std::string typed(1 + InsecureRandRange(6), 'a');
        for (char& c : typed) c = char('a' + InsecureRandRange(i % 2 ? 2 : 3));
        const std::vector<uint32_t> folded(text.begin(), text.end());
        uint64_t steps = 0;
        const MatchResult result = FindFolded(folded, TextNeedle(typed), nullptr, &steps);
        const bool expected = std::search(text.begin(), text.end(), typed.begin(), typed.end()) != text.end();
        BOOST_REQUIRE_MESSAGE((result == MatchResult::MATCH) == expected, text + " / " + typed);
        BOOST_REQUIRE(steps <= 2 * text.size());
    }
    // A needle that nearly matches everywhere: a search from every position would take about a thousand steps per
    // code point.
    const std::vector<uint32_t> text(TEXT_SEARCH_SCRIPT_BYTES, 'a');
    uint64_t steps = 0;
    BOOST_CHECK(FindFolded(text, TextNeedle(std::string(1000, 'a') + "b"), nullptr, &steps) == MatchResult::NO_MATCH);
    BOOST_CHECK_LE(steps, 2 * text.size());
    steps = 0;
    BOOST_CHECK(FindFolded(text, TextNeedle(std::string(1000, 'a')), nullptr, &steps) == MatchResult::MATCH);
    BOOST_CHECK_EQUAL(steps, 1000U);
}

BOOST_AUTO_TEST_CASE(find_folded_cancel)
{
    // A search that is cancelled before it decides says so.
    const std::vector<uint32_t> text(3 * CANCEL_INTERVAL, 'a');
    std::atomic<bool> cancel{true};
    uint64_t steps = 0;
    BOOST_CHECK(FindFolded(text, TextNeedle("b"), &cancel, &steps) == MatchResult::CANCELLED);
    BOOST_CHECK_EQUAL(steps, 0U);
    cancel = false;
    BOOST_CHECK(FindFolded(text, TextNeedle("b"), &cancel, &steps) == MatchResult::NO_MATCH);
    BOOST_CHECK_EQUAL(steps, text.size());

    // A flag set while the search runs stops it at its next check, CANCEL_INTERVAL code points apart: another thread
    // sets it as soon as the search begins. The flag may also come before the search's first check, or after its end;
    // the search is tried until a flag set during it has stopped it.
    const std::vector<uint32_t> long_text(64 * CANCEL_INTERVAL, 'a');
    const TextNeedle needle("b");
    bool stopped_inside{false};
    for (int attempt = 0; attempt < 200 && !stopped_inside; ++attempt) {
        std::atomic<bool> flag{false};
        std::atomic<bool> begun{false};
        std::thread setter([&] {
            while (!begun.load()) {
            }
            flag = true;
        });
        uint64_t taken = 0;
        begun = true;
        const MatchResult result = FindFolded(long_text, needle, &flag, &taken);
        setter.join();
        if (result == MatchResult::CANCELLED) {
            BOOST_CHECK_EQUAL(taken % CANCEL_INTERVAL, 0U);
            stopped_inside = taken > 0 && taken < long_text.size();
        } else {
            BOOST_CHECK(result == MatchResult::NO_MATCH);
        }
    }
    BOOST_CHECK(stopped_inside);
}

BOOST_AUTO_TEST_CASE(rules)
{
    // Binary: data that is not well-formed UTF-8, and empty data.
    BOOST_CHECK(!IsBinary(RowOf(TextScript("hello"))));
    BOOST_CHECK(!IsBinary(RowOf(TextScript("caf\xc3\xa9"))));
    BOOST_CHECK(IsBinary(RowOf(TextScript("caf\xe9"))));
    BOOST_CHECK(IsBinary(RowOf({OP_RETURN})));
    BOOST_CHECK(IsBinary(RowOf({OP_RETURN, OP_0})));
    BOOST_CHECK(IsBinary(RowOf({OP_RETURN, 0x01, 0x80})));
    // A push that runs past the end: the data is every byte after OP_RETURN, here a control character, which is text.
    BOOST_CHECK(!IsBinary(RowOf({OP_RETURN, 0x02, 0x41})));
    RowFilter filter;
    BOOST_CHECK(filter.hide_binary);
    BOOST_CHECK(!PassesRules(RowOf({OP_RETURN, OP_0}), filter));
    filter.hide_binary = false;
    BOOST_CHECK(PassesRules(RowOf({OP_RETURN, OP_0}), filter));

    // The fee rate, in atoms per 1,000 virtual bytes, compared without rounding: 1,500 atoms for 1,000 vbytes is
    // exactly 1,500; a fee that is not known never hides a message.
    const CScript payer = CScript() << OP_TRUE;
    BOOST_CHECK(!PaidBelow(PaidWith(payer, 1500, 1000), 1500));
    BOOST_CHECK(PaidBelow(PaidWith(payer, 1499, 1000), 1500));
    BOOST_CHECK(!PaidBelow(PaidWith(payer, 3, 2), 1500));
    BOOST_CHECK(PaidBelow(PaidWith(payer, 2, 2), 1500));
    BOOST_CHECK(!PaidBelow(PaidWith(payer, -1, 1000), 1500));
    BOOST_CHECK(!PaidBelow(PaidWith(payer, 0, 0), 1500));
    BOOST_CHECK(!PaidBelow(PaidWith(payer, 0, 1000), 0));
    BOOST_CHECK(PaidBelow(PaidWith(payer, 0, 1000), 1));
    // The largest values do not overflow; a rate above the highest counts as the highest.
    BOOST_CHECK(!PaidBelow(PaidWith(payer, MAX_MONEY, 1), MAX_MIN_FEE_RATE));
    BOOST_CHECK(PaidBelow(PaidWith(payer, MAX_MIN_FEE_RATE - 1, 1000), std::numeric_limits<int64_t>::max()));
    BOOST_CHECK(!PaidBelow(PaidWith(payer, MAX_MIN_FEE_RATE, 1000), std::numeric_limits<int64_t>::max()));
    BOOST_CHECK(PaidBelow(PaidWith(payer, 99, std::numeric_limits<uint32_t>::max()), MAX_MIN_FEE_RATE));
    filter.min_fee_rate = 1500;
    BOOST_CHECK(!PassesRules(RowOf(TextScript("cheap"), PaidWith(payer, 1499, 1000)), filter));
    BOOST_CHECK(PassesRules(RowOf(TextScript("paid"), PaidWith(payer, 1500, 1000)), filter));
    BOOST_CHECK(PassesRules(RowOf(TextScript("unknown"), PaidWith(payer)), filter));

    // Paid by: only a known coin with that very script.
    const CScript other = CScript() << OP_FALSE;
    filter = RowFilter{};
    filter.paid_by = payer;
    BOOST_CHECK(PassesRules(RowOf(TextScript("a"), PaidWith(payer)), filter));
    BOOST_CHECK(!PassesRules(RowOf(TextScript("a"), PaidWith(other)), filter));
    for (const opreturn::PaidByKind kind : {opreturn::PaidByKind::PENDING, opreturn::PaidByKind::UNAVAILABLE,
                                            opreturn::PaidByKind::COINBASE, opreturn::PaidByKind::HOGEX}) {
        TxBrief tx = PaidWith(payer);
        tx.paid_by_kind = kind;
        BOOST_CHECK(!PaidBy(RowOf(TextScript("a"), tx), payer));
    }
}

BOOST_AUTO_TEST_CASE(scan_order_and_slices)
{
    Rows rows;
    RowFilter all;
    all.hide_binary = false;
    ScanRequest newest;
    // Newest first and oldest first, over every visible row, in slices of four rows found.
    newest.max_found = 4;
    size_t slices = 0;
    BOOST_CHECK_EQUAL(Keys(ScanAll(rows.record, newest, all, &slices)), Keys(Reversed(rows.all)));
    BOOST_CHECK_EQUAL(slices, 4U);
    ScanRequest oldest;
    oldest.from = opreturn::ROW_KEY_BEFORE_ALL;
    oldest.older = false;
    BOOST_CHECK_EQUAL(Keys(ScanAll(rows.record, oldest, all)), Keys(rows.all));

    // The first slice of four: the newest four rows, and where to go on.
    ScanResult first = Scan(rows.record, newest, all);
    BOOST_CHECK_EQUAL(Keys(first.found), "(14,2)(14,1)(14,0)(13,2)");
    BOOST_CHECK(first.next == (RowKey{13, 2}));
    BOOST_CHECK(!first.end);
    BOOST_CHECK_EQUAL(first.rows, 4U);

    // The rules decide which rows are found; every row is decided.
    RowFilter text;
    const ScanResult texts = Scan(rows.record, newest, text);
    BOOST_CHECK_EQUAL(Keys(texts.found), "(14,2)(14,0)(13,2)(13,0)");
    BOOST_CHECK_EQUAL(texts.rows, 6U);

    // A slice of no time decides one row.
    ScanRequest instant = newest;
    instant.slice = std::chrono::milliseconds{0};
    const ScanResult one = Scan(rows.record, instant, all);
    BOOST_CHECK_EQUAL(one.rows, 1U);
    BOOST_CHECK_EQUAL(Keys(one.found), "(14,2)");
    BOOST_CHECK(!one.end);
    slices = 0;
    BOOST_CHECK_EQUAL(Keys(ScanAll(rows.record, instant, all, &slices)), Keys(Reversed(rows.all)));
    BOOST_CHECK_EQUAL(slices, rows.all.size() + 1);

    // Heights: only those asked for, and only those the snapshot shows.
    ScanRequest some = oldest;
    some.heights = {11, 12};
    BOOST_CHECK_EQUAL(Keys(ScanAll(rows.record, some, all)), "(11,0)(11,1)(11,2)(12,0)(12,1)(12,2)");
    rows.record.Show({{10, 10}, {13, 14}}, /*floor=*/11, /*live_tip=*/13);
    BOOST_CHECK_EQUAL(Keys(ScanAll(rows.record, oldest, all)), "(13,0)(13,1)(13,2)");
    rows.record.Show({});
    const ScanResult nothing = Scan(rows.record, newest, all);
    BOOST_CHECK(nothing.found.empty() && nothing.end && nothing.rows == 0);
}

BOOST_AUTO_TEST_CASE(scan_text_search)
{
    FakeRecord record;
    const CScript payer = CScript() << OP_TRUE;
    record.AddRow({1, 0}, TextScript("The NEWS of the day"), PaidWith(payer));
    record.AddRow({1, 1}, TextScript("no message here"));
    record.AddRow({2, 0}, TextScript("ne\xe2\x80\x8bws, hidden"));
    record.AddRow({2, 1}, std::vector<unsigned char>{OP_RETURN, 0x04, 'n', 'e', 'w', 0xff});
    // A script of 70,000 bytes of data in one push: its data begins 6 bytes into the script. The search looks at the
    // data in the script's first 65,536 bytes: a word that ends there is found, one a byte later is not.
    const auto long_script = [](size_t at) {
        std::string data(70000, 'x');
        data.replace(at, 6, "NEEDLE");
        return TextScript(data);
    };
    record.AddRow({3, 0}, long_script(65536 - 6 - 6));
    record.AddRow({3, 1}, long_script(65536 - 6 - 5));
    // ... and one past the script's head of 4,096 bytes, which only a further read gives.
    record.AddRow({3, 2}, long_script(5000));
    // A long script of many pushes, whose data joins them: a word split between two of them is found, though the
    // part of the script searched ends inside a push.
    std::vector<unsigned char> pushes{OP_RETURN};
    for (int i = 0; i < 900; ++i) {
        std::string data(75, 'x');
        if (i == 10) data.replace(72, 3, "NEE");
        if (i == 11) data.replace(0, 3, "DLE");
        pushes.push_back(75);
        pushes.insert(pushes.end(), data.begin(), data.end());
    }
    BOOST_REQUIRE_GT(pushes.size(), TEXT_SEARCH_SCRIPT_BYTES);
    record.AddRow({3, 3}, pushes);
    record.Show({{1, 3}});

    ScanRequest request;
    request.from = opreturn::ROW_KEY_BEFORE_ALL;
    request.older = false;
    request.slice = LONG_SLICE;
    RowFilter filter;
    filter.text = Needle("news");
    ScanResult result = Scan(record, request, filter);
    BOOST_CHECK_EQUAL(Keys(result.found), "(1,0)(2,0)");
    BOOST_CHECK(result.end);
    BOOST_CHECK_EQUAL(result.searched_in_part, 4U);
    BOOST_CHECK_EQUAL(result.unreadable, 0U);

    filter.text = Needle("needle");
    result = Scan(record, request, filter);
    BOOST_CHECK_EQUAL(Keys(result.found), "(3,0)(3,2)(3,3)");
    BOOST_CHECK_EQUAL(result.searched_in_part, 4U);
    filter.whole_messages = true;
    result = Scan(record, request, filter);
    BOOST_CHECK_EQUAL(Keys(result.found), "(3,0)(3,1)(3,2)(3,3)");
    BOOST_CHECK_EQUAL(result.searched_in_part, 0U);

    // Binary rows are not searched while they are hidden.
    filter = RowFilter{};
    filter.text = Needle("new");
    BOOST_CHECK_EQUAL(Keys(Scan(record, request, filter).found), "(1,0)(2,0)");
    filter.hide_binary = false;
    BOOST_CHECK_EQUAL(Keys(Scan(record, request, filter).found), "(1,0)(2,0)(2,1)");

    // Paid by, and paid by with a text.
    filter = RowFilter{};
    filter.paid_by = payer;
    BOOST_CHECK_EQUAL(Keys(Scan(record, request, filter).found), "(1,0)");
    filter.text = Needle("message");
    BOOST_CHECK(Scan(record, request, filter).found.empty());

    // A needle that folds to nothing finds every row the rules show, and reads no script.
    filter = RowFilter{};
    filter.text = Needle("\xe2\x80\x8b");
    const size_t reads = record.ReadSnapshots().size();
    BOOST_CHECK_EQUAL(Scan(record, request, filter).found.size(), 7U);
    BOOST_CHECK_EQUAL(record.ReadSnapshots().size(), reads + 1);
}

BOOST_AUTO_TEST_CASE(scan_one_snapshot)
{
    // Every read of a slice uses one snapshot: the one asked for, or the record's current one when it begins.
    Rows rows;
    for (int32_t h = 10; h <= 14; ++h) rows.record.AddRow({h, 3}, TextScript(std::string(10000, 'y') + "news"));
    rows.record.Show({{10, 14}});
    RowFilter filter;
    filter.text = Needle("news");
    ScanRequest request;
    request.slice = LONG_SLICE;
    // Each read publishes a new snapshot, as a record that writes blocks meanwhile does.
    rows.record.BeforeRead([&](const VisibilityRef&) { rows.record.Show({{10, 14}}); });
    const VisibilityRef first = rows.record.getVisibility();
    const ScanResult result = Scan(rows.record, request, filter);
    BOOST_CHECK_EQUAL(result.found.size(), 10U);
    BOOST_CHECK(result.visibility == first);
    const std::vector<VisibilityRef> used = rows.record.ReadSnapshots();
    // One read of rows and five reads of long scripts.
    BOOST_CHECK_EQUAL(used.size(), 6U);
    for (const VisibilityRef& v : used) BOOST_CHECK(v == first);

    request.visibility = used.front();
    rows.record.Show({{10, 14}});
    BOOST_CHECK(Scan(rows.record, request, filter).visibility == used.front());
    for (const VisibilityRef& v : rows.record.ReadSnapshots()) BOOST_CHECK(v == first);
}

BOOST_AUTO_TEST_CASE(scan_removed_rows)
{
    // When rows are removed while a slice reads, nothing it found counts, and the next slice reads again from the same
    // place with the new snapshot.
    Rows rows;
    RowFilter all;
    all.hide_binary = false;
    ScanRequest request;
    request.max_found = 5;
    int reads = 0;
    rows.record.BeforeRead([&](const VisibilityRef&) {
        if (++reads == 1) rows.record.RaiseRemovals();
    });
    ScanResult result = Scan(rows.record, request, all);
    BOOST_CHECK(result.stale);
    BOOST_CHECK(result.found.empty());
    BOOST_CHECK(result.next == request.from);
    BOOST_CHECK(!result.end && !result.failed);
    BOOST_CHECK_EQUAL(result.rows, 0U);
    // The old snapshot stays stale; the record's new one, published after the removal, reads.
    request.visibility = result.visibility;
    BOOST_CHECK(Scan(rows.record, request, all).stale);
    rows.record.Show({{10, 13}});
    request.visibility = nullptr;
    result = Scan(rows.record, request, all);
    BOOST_CHECK(!result.stale);
    BOOST_CHECK_EQUAL(Keys(result.found), "(13,2)(13,1)(13,0)(12,2)(12,1)");

    // A removal during a long script's read, after the row was read, also makes the slice stale.
    FakeRecord record;
    record.AddRow({5, 0}, TextScript(std::string(9000, 'z') + "news"));
    record.Show({{5, 5}});
    RowFilter filter;
    filter.text = Needle("news");
    reads = 0;
    record.BeforeRead([&](const VisibilityRef&) {
        if (++reads == 2) record.RaiseRemovals();
    });
    ScanRequest from_start;
    BOOST_CHECK(Scan(record, from_start, filter).stale);
    record.BeforeRead(nullptr);
    record.Show({{5, 5}});
    BOOST_CHECK_EQUAL(Keys(Scan(record, from_start, filter).found), "(5,0)");

    // A failed read while rows were removed is read again, not reported.
    rows.record.FailRow({12, 0});
    reads = 0;
    rows.record.BeforeRead([&](const VisibilityRef&) {
        if (++reads == 1) rows.record.RaiseRemovals();
    });
    result = Scan(rows.record, ScanRequest{}, all);
    BOOST_CHECK(result.stale && !result.failed);

    // As the record does, the counter is raised, the rows are removed and the snapshot after the removal is
    // published while the slice reads: the slice read with the snapshot before it, so it is stale, though the record's
    // current snapshot then has the counter's value.
    {
        Rows raced_rows;
        ScanRequest from_top;
        from_top.slice = LONG_SLICE;
        int n = 0;
        raced_rows.record.BeforeRead([&](const VisibilityRef&) {
            if (++n == 1) {
                raced_rows.record.RaiseRemovals();
                raced_rows.record.Show({{10, 13}});
            }
        });
        const ScanResult raced = Scan(raced_rows.record, from_top, all);
        BOOST_CHECK(raced.stale);
        BOOST_CHECK_MESSAGE(raced.found.empty(), Keys(raced.found));
        // ... and a slice given the old snapshot after the new one was published is stale too, and does not say it
        // came to the end although it read every row.
        raced_rows.record.BeforeRead(nullptr);
        from_top.visibility = raced.visibility;
        const ScanResult old = Scan(raced_rows.record, from_top, all);
        BOOST_CHECK(old.stale);
        BOOST_CHECK_MESSAGE(old.found.empty(), Keys(old.found));
        BOOST_CHECK(!old.end);
    }

    // A stale slice counts nothing: not the rows it decided, nor the long scripts it searched in part or could not read.
    {
        FakeRecord counted;
        counted.AddRow({1, 0}, TextScript(std::string(70000, 'z')));
        counted.AddRow({1, 1}, TextScript(std::string(70000, 'z')));
        counted.FailScript({1, 1});
        counted.Show({{1, 1}});
        RowFilter search;
        search.text = Needle("news");
        ScanResult fresh = Scan(counted, ScanRequest{}, search);
        BOOST_CHECK(fresh.end && !fresh.stale);
        BOOST_CHECK_EQUAL(fresh.rows, 2U);
        BOOST_CHECK_EQUAL(fresh.searched_in_part, 1U);
        BOOST_CHECK_EQUAL(fresh.unreadable, 1U);
        counted.RaiseRemovals();
        ScanRequest again;
        again.visibility = fresh.visibility;
        const ScanResult stale = Scan(counted, again, search);
        BOOST_CHECK(stale.stale);
        BOOST_CHECK_EQUAL(stale.rows, 0U);
        BOOST_CHECK_EQUAL(stale.searched_in_part, 0U);
        BOOST_CHECK_EQUAL(stale.unreadable, 0U);
    }
}

BOOST_AUTO_TEST_CASE(scan_removed_between_slices)
{
    // A scan of several slices passes the first slice's snapshot to the next: a removal between two slices then makes
    // the second stale, which tells the caller that the first slice's rows may be gone too. A slice that took the
    // record's current snapshot instead would not see the removal.
    Rows rows;
    RowFilter all;
    all.hide_binary = false;
    ScanRequest first;
    first.max_found = 3;
    const ScanResult one = Scan(rows.record, first, all);
    BOOST_CHECK_EQUAL(Keys(one.found), "(14,2)(14,1)(14,0)");
    BOOST_CHECK(!one.stale && !one.end);
    rows.record.RaiseRemovals();
    rows.record.Show({{10, 13}});
    ScanRequest second;
    second.from = one.next;
    second.visibility = one.visibility;
    const ScanResult two = Scan(rows.record, second, all);
    BOOST_CHECK(two.stale);
    BOOST_CHECK(two.found.empty());
    BOOST_CHECK(two.next == one.next);
    second.visibility = nullptr;
    const ScanResult unaware = Scan(rows.record, second, all);
    BOOST_CHECK(!unaware.stale);
    BOOST_CHECK_EQUAL(unaware.found.size(), 12U);
}

BOOST_AUTO_TEST_CASE(scan_without_coverage)
{
    // A snapshot that covers nothing, as a record shows before it opens, ends the scan without a read.
    Rows rows;
    ScanRequest request;
    request.visibility = std::make_shared<const opreturn::Visibility>();
    const ScanResult result = Scan(rows.record, request, RowFilter{});
    BOOST_CHECK(result.end && result.found.empty() && !result.stale && !result.failed);
    BOOST_CHECK(rows.record.ReadSnapshots().empty());
}

BOOST_AUTO_TEST_CASE(scan_read_failures)
{
    // A failed read of rows ends the slice after the last row decided: what was found before holds, the failure is
    // the record's to count, and the next slice may try again. Nothing throws.
    Rows rows;
    rows.record.FailRow({12, 1});
    RowFilter all;
    all.hide_binary = false;
    ScanRequest request;
    request.slice = LONG_SLICE;
    ScanResult result = Scan(rows.record, request, all);
    BOOST_CHECK(result.failed);
    BOOST_CHECK(!result.stale && !result.end);
    BOOST_CHECK_EQUAL(Keys(result.found), "(14,2)(14,1)(14,0)(13,2)(13,1)(13,0)(12,2)");
    BOOST_CHECK(result.next == (RowKey{12, 2}));
    BOOST_CHECK_EQUAL(rows.record.ReadErrors(), 1U);
    request.from = result.next;
    result = Scan(rows.record, request, all);
    BOOST_CHECK(result.failed && result.found.empty() && result.next == request.from);
    BOOST_CHECK_EQUAL(rows.record.ReadErrors(), 2U);

    // A long script that cannot be read does not match the search, and the scan goes on.
    FakeRecord record;
    record.AddRow({1, 0}, TextScript(std::string(9000, 'z') + "news"));
    record.AddRow({1, 1}, TextScript(std::string(9000, 'z') + "news"));
    record.AddRow({1, 2}, TextScript("news"));
    record.FailScript({1, 1});
    record.Show({{1, 1}});
    RowFilter filter;
    filter.text = Needle("news");
    result = Scan(record, ScanRequest{}, filter);
    BOOST_CHECK_EQUAL(Keys(result.found), "(1,2)(1,0)");
    BOOST_CHECK(result.end && !result.failed);
    BOOST_CHECK_EQUAL(result.unreadable, 1U);
    BOOST_CHECK_EQUAL(record.ReadErrors(), 1U);
}

BOOST_AUTO_TEST_CASE(scan_cancel)
{
    Rows rows;
    RowFilter all;
    all.hide_binary = false;
    std::atomic<bool> cancel{true};
    // Cancelled before it begins: nothing is read.
    ScanResult result = Scan(rows.record, ScanRequest{}, all, &cancel);
    BOOST_CHECK(result.cancelled && result.found.empty() && result.rows == 0);
    BOOST_CHECK(rows.record.ReadSnapshots().empty());

    // Cancelled while a long script is read: the slice stops before that row.
    FakeRecord record;
    record.AddRow({1, 0}, TextScript("news"));
    record.AddRow({1, 1}, TextScript(std::string(9000, 'z') + "news"));
    record.AddRow({1, 2}, TextScript("news"));
    record.Show({{1, 1}});
    cancel = false;
    record.BeforeRead([&](const VisibilityRef&) {
        if (record.ReadSnapshots().size() == 1) cancel = true;
    });
    RowFilter filter;
    filter.text = Needle("news");
    ScanRequest request;
    request.from = opreturn::ROW_KEY_BEFORE_ALL;
    request.older = false;
    result = Scan(record, request, filter, &cancel);
    BOOST_CHECK(result.cancelled);
    BOOST_CHECK_EQUAL(Keys(result.found), "(1,0)");
    BOOST_CHECK(result.next == (RowKey{1, 0}));

    // Cancelled while rows are read, without a text search: the slice stops after the first row.
    FakeRecord short_rows;
    for (uint32_t n = 0; n < 3; ++n) short_rows.AddRow({1, n}, TextScript("news"));
    short_rows.Show({{1, 1}});
    cancel = false;
    short_rows.BeforeRead([&](const VisibilityRef&) { cancel = true; });
    result = Scan(short_rows, request, all, &cancel);
    BOOST_CHECK(result.cancelled);
    BOOST_CHECK_EQUAL(Keys(result.found), "(1,0)");
    BOOST_CHECK(result.next == (RowKey{1, 0}));
    // ... and with one, a text search that begins after the flag is set does not decide its row.
    cancel = false;
    result = Scan(short_rows, request, filter, &cancel);
    BOOST_CHECK(result.cancelled && result.found.empty() && result.next == request.from);

    // A long message searched in part whose search was cancelled is not counted as searched.
    FakeRecord long_rows;
    long_rows.AddRow({1, 0}, TextScript(std::string(70000, 'z') + "news"));
    long_rows.Show({{1, 1}});
    cancel = false;
    long_rows.BeforeRead([&](const VisibilityRef&) {
        if (long_rows.ReadSnapshots().size() == 1) cancel = true;
    });
    result = Scan(long_rows, request, filter, &cancel);
    BOOST_CHECK(result.cancelled);
    BOOST_CHECK_EQUAL(result.rows, 0U);
    BOOST_CHECK_EQUAL(result.searched_in_part, 0U);

    // A cancelled slice during which rows were removed is stale as well: what it found does not count.
    cancel = false;
    short_rows.BeforeRead([&](const VisibilityRef&) {
        short_rows.RaiseRemovals();
        cancel = true;
    });
    result = Scan(short_rows, request, all, &cancel);
    BOOST_CHECK(result.cancelled && result.stale);
    BOOST_CHECK(result.found.empty());
}

BOOST_FIXTURE_TEST_CASE(real_record, TestChain100Setup)
{
    // The scan over a record the node keeps: the rows it finds are those the fake record finds when it holds the same
    // rows, and it takes no node lock.
    opreturn::Options options;
    options.record = true;
    auto record = std::make_shared<opreturn::Record>(GetDataDir() / "query", options);
    record->load();
    record->start();
    const auto op_return = [](const std::string& data) {
        const std::vector<unsigned char> script = TextScript(data);
        return CScript(script.begin(), script.end());
    };
    std::string long_text(70000, 'x');
    long_text.replace(60000, 6, "needle");
    const CScript plain = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    for (const std::string& text : {std::string("first news"), std::string("caf\xe9 news"), std::string("weather"),
                                    long_text, std::string("NEWS again")}) {
        CreateAndProcessBlock({}, op_return(text));
        // A sparse chain: blocks without rows between some of those with rows, more of them than the record reads
        // block entries at a time and than its first span of heights.
        if (text == "weather") {
            for (int i = 0; i < 300; ++i) CreateAndProcessBlock({}, plain);
        }
    }
    const int tip = WITH_LOCK(cs_main, return ::ChainActive().Height());
    SyncWithValidationInterfaceQueue();
    const Clock::time_point until = Clock::now() + std::chrono::seconds{20};
    while (record->getVisibility()->live_tip < tip && Clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds{1});
    BOOST_REQUIRE_EQUAL(record->getVisibility()->live_tip, tip);

    // The same rows in a fake record.
    FakeRecord fake;
    opreturn::RowRequest every;
    every.max_script = 0;
    every.max_rows = 1000;
    const opreturn::RowBatch batch = record->readRows(every);
    BOOST_REQUIRE(batch.end && !batch.failed);
    BOOST_REQUIRE_EQUAL(batch.rows.size(), 5U);
    for (const RowView& row : batch.rows) {
        std::vector<unsigned char> script;
        BOOST_REQUIRE(record->readScript(row.key, 0, row.script_size, script, nullptr) == ReadResult::FOUND);
        fake.AddView(row, script);
    }
    fake.Show(*record->getVisibility()->coverage);

    // Held by another thread while the scans run, cs_main would stop any scan that waited for it.
    std::promise<void> held;
    std::promise<void> done;
    std::future<void> scanned = done.get_future();
    bool done_while_held{false};
    std::thread holder([&] {
        LOCK(cs_main);
        held.set_value();
        done_while_held = scanned.wait_for(std::chrono::seconds{20}) == std::future_status::ready;
    });
    held.get_future().wait();
    for (const bool older : {true, false}) {
        ScanRequest request;
        request.older = older;
        request.from = older ? opreturn::ROW_KEY_AFTER_ALL : opreturn::ROW_KEY_BEFORE_ALL;
        for (const bool hide_binary : {true, false}) {
            for (const std::string& typed : {std::string(), std::string("news"), std::string("needle")}) {
                RowFilter filter;
                filter.hide_binary = hide_binary;
                if (!typed.empty()) filter.text = Needle(typed);
                const std::vector<RowKey> found = ScanAll(*record, request, filter);
                BOOST_CHECK_EQUAL(Keys(found), Keys(ScanAll(fake, request, filter)));
                if (typed == "news") BOOST_CHECK_EQUAL(found.size(), hide_binary ? 2U : 3U);
                if (typed == "needle") BOOST_CHECK_EQUAL(found.size(), 1U);
            }
        }
    }
    done.set_value();
    holder.join();
    BOOST_CHECK(done_while_held);
    record->stop();
}

BOOST_AUTO_TEST_SUITE_END()
