// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/validation.h>
#include <opreturn/script.h>
#include <primitives/block.h>
#include <random.h>
#include <script/script.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace opreturn;
using opreturn::test::MakeScript;
using opreturn::test::OneDataPush;

namespace {

std::vector<unsigned char> Bytes(const std::string& hex) { return ParseHex(hex); }
std::vector<unsigned char> Text(const std::string& text) { return {text.begin(), text.end()}; }

ParsedScript Parse(const std::vector<unsigned char>& script)
{
    return ParseOpReturn(script, PARSE_DATA | PARSE_PARTS);
}

//! OP_RETURN, OP_PUSHDATA4 with a length of `length`, then `data` bytes of `fill`, then `tail`.
std::vector<unsigned char> Pushdata4(uint32_t length, size_t data, unsigned char fill, const std::vector<unsigned char>& tail = {})
{
    std::vector<unsigned char> script{OP_RETURN, OP_PUSHDATA4};
    for (int i = 0; i < 4; ++i) script.push_back(uint8_t(length >> (8 * i)));
    script.resize(script.size() + data, fill);
    script.insert(script.end(), tail.begin(), tail.end());
    return script;
}

//! What the tree's own script reader says of a script after its first byte.
struct TreeView {
    bool push_only{false};
    bool walk_ok{true};              //!< every push is complete
    std::vector<unsigned char> data; //!< the pushed bytes, joined
    size_t data_pushes{0};           //!< opcodes 0x01 to OP_PUSHDATA4
    bool number{false};              //!< OP_1NEGATE or OP_1 to OP_16
};

TreeView ReadWithTree(const std::vector<unsigned char>& bytes)
{
    TreeView view;
    const CScript script(bytes.begin(), bytes.end());
    view.push_only = script.IsPushOnly(script.begin() + 1);
    CScript::const_iterator pc = script.begin() + 1;
    while (pc < script.end()) {
        opcodetype op;
        std::vector<unsigned char> pushed;
        if (!script.GetOp(pc, op, pushed)) {
            view.walk_ok = false;
            break;
        }
        view.data.insert(view.data.end(), pushed.begin(), pushed.end());
        if (op >= 0x01 && op <= OP_PUSHDATA4) ++view.data_pushes;
        if (op == OP_1NEGATE || (op >= OP_1 && op <= OP_16)) view.number = true;
    }
    return view;
}

//! Checks a whole-script result against the parser's definitions and against the tree's script reader.
void CheckAgainstTree(const std::vector<unsigned char>& script)
{
    BOOST_REQUIRE(IsOpReturn(script));
    const ParsedScript parsed = Parse(script);
    const TreeView tree = ReadWithTree(script);
    BOOST_CHECK_EQUAL((parsed.flags & ROW_PUSH_ONLY) != 0, tree.push_only);
    BOOST_CHECK_EQUAL((parsed.flags & ROW_PARSE_ERROR) != 0, !tree.walk_ok);
    BOOST_CHECK(FlagsFit(parsed.flags, script.size(), parsed.data_size));
    BOOST_CHECK_EQUAL(parsed.data.size(), parsed.data_size);
    // The parts lie in the script after OP_RETURN, in order, and joined they are the data.
    std::vector<unsigned char> joined;
    size_t end = 1;
    for (const DataPart& part : parsed.parts) {
        BOOST_CHECK(part.offset >= end && part.offset + part.size <= script.size());
        joined.insert(joined.end(), script.begin() + part.offset, script.begin() + part.offset + part.size);
        end = part.offset + part.size;
    }
    BOOST_CHECK(joined == parsed.data);
    BOOST_CHECK_EQUAL((parsed.flags & ROW_UTF8) != 0, IsUtf8(parsed.data));
    if (tree.push_only) {
        BOOST_CHECK(parsed.data == tree.data);
        BOOST_CHECK_EQUAL(parsed.pushes, tree.data_pushes);
        BOOST_CHECK_EQUAL(parsed.parts.size(), tree.data_pushes);
        BOOST_CHECK_EQUAL((parsed.flags & ROW_MULTI_PUSH) != 0, tree.data_pushes > 1);
        BOOST_CHECK_EQUAL((parsed.flags & ROW_NUMBER_PUSH) != 0, tree.number);
    } else {
        BOOST_CHECK(parsed.data == std::vector<unsigned char>(script.begin() + 1, script.end()));
        BOOST_CHECK_EQUAL(parsed.pushes, 0U);
    }
    // Asking for less gives the same facts.
    const ParsedScript facts = ParseOpReturn(script, PARSE_FACTS);
    BOOST_CHECK_EQUAL(facts.flags, parsed.flags);
    BOOST_CHECK_EQUAL(facts.data_size, parsed.data_size);
    BOOST_CHECK_EQUAL(facts.pushes, parsed.pushes);
    BOOST_CHECK(facts.data.empty() && facts.parts.empty());
}

//! The whole script's result cut at `prefix_size` bytes, built from the whole result.
ParsedScript CutAt(const ParsedScript& whole, size_t prefix_size)
{
    ParsedScript cut;
    cut.flags = whole.flags;
    if (whole.flags & ROW_PUSH_ONLY) {
        for (const DataPart& part : whole.parts) {
            if (part.offset > prefix_size) break; // its push header does not lie wholly in the prefix
            cut.parts.push_back({part.offset, std::min(part.size, prefix_size - part.offset)});
        }
        cut.pushes = cut.parts.size();
    } else if (prefix_size > 1) {
        cut.parts.push_back({1, prefix_size - 1});
    }
    for (const DataPart& part : cut.parts) cut.data_size += part.size;
    cut.data.assign(whole.data.begin(), whole.data.begin() + cut.data_size);
    return cut;
}

void CheckPrefixes(const std::vector<unsigned char>& script, const std::vector<size_t>& sizes)
{
    const ParsedScript whole = Parse(script);
    for (const size_t size : sizes) {
        if (size > script.size()) continue;
        const Span<const unsigned char> prefix = Span<const unsigned char>{script}.first(size);
        const ParsedScript got = ParseOpReturnPrefix(prefix, whole.flags, PARSE_DATA | PARSE_PARTS);
        const ParsedScript want = CutAt(whole, size);
        BOOST_CHECK_EQUAL(got.flags, want.flags);
        BOOST_CHECK_EQUAL(got.data_size, want.data_size);
        BOOST_CHECK_EQUAL(got.pushes, want.pushes);
        BOOST_CHECK(got.data == want.data);
        BOOST_CHECK(got.parts == want.parts);
        BOOST_CHECK(got.data_size <= whole.data_size);
        const ParsedScript facts = ParseOpReturnPrefix(prefix, whole.flags, PARSE_FACTS);
        BOOST_CHECK_EQUAL(facts.data_size, want.data_size);
        BOOST_CHECK(facts.data.empty() && facts.parts.empty());
    }
}

std::vector<size_t> AllSizes(size_t n)
{
    std::vector<size_t> sizes;
    for (size_t i = 0; i <= n; ++i) sizes.push_back(i);
    return sizes;
}

//! A random script after OP_RETURN, mostly pushes, sometimes cut short or with another opcode.
std::vector<unsigned char> RandomScript(FastRandomContext& rng)
{
    std::vector<unsigned char> script{OP_RETURN};
    const int ops = rng.randrange(12);
    for (int i = 0; i < ops; ++i) {
        switch (rng.randrange(8)) {
        case 0: script.push_back(uint8_t(rng.randrange(256))); break;                       // any opcode
        case 1: script.push_back(uint8_t(OP_1NEGATE + rng.randrange(OP_16 - OP_1NEGATE + 1))); break;
        case 2: script.push_back(OP_0); break;
        default: {
            const std::vector<unsigned char> push = OneDataPush(rng.randbytes(rng.randrange(300)));
            script.insert(script.end(), push.begin() + 1, push.end());
        }
        }
    }
    if (rng.randrange(4) == 0 && script.size() > 1) script.resize(1 + rng.randrange(script.size() - 1));
    return script;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_script_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(not_op_return)
{
    BOOST_CHECK(!IsOpReturn(std::vector<unsigned char>{}));
    BOOST_CHECK(!IsOpReturn(Bytes("00")));
    BOOST_CHECK(!IsOpReturn(Bytes("516a")));
    BOOST_CHECK(IsOpReturn(Bytes("6a")));
    for (const std::string hex : {"", "00", "516a", "76a914"}) {
        const ParsedScript parsed = Parse(Bytes(hex));
        BOOST_CHECK_EQUAL(parsed.flags, 0);
        BOOST_CHECK_EQUAL(parsed.data_size, 0U);
        BOOST_CHECK_EQUAL(parsed.pushes, 0U);
        BOOST_CHECK(parsed.data.empty() && parsed.parts.empty());
        // A prefix that cannot begin an OP_RETURN script gives nothing either.
        if (!hex.empty()) BOOST_CHECK_EQUAL(ParseOpReturnPrefix(Bytes(hex), ROW_PUSH_ONLY).flags, 0);
    }
}

BOOST_AUTO_TEST_CASE(push_forms)
{
    // The example of the earlier command-line tools: the data is exactly the pushed bytes.
    const ParsedScript example = Parse(Bytes("6a04deadbeef"));
    BOOST_CHECK(example.data == Bytes("deadbeef"));
    BOOST_CHECK_EQUAL(example.flags, ROW_PUSH_ONLY);
    BOOST_CHECK(example.parts == std::vector<DataPart>({{2, 4}}));

    // Direct pushes of 1 to 75 bytes; a push of no bytes is OP_0, which adds nothing.
    for (size_t n = 0; n <= 75; ++n) {
        std::vector<unsigned char> script{OP_RETURN, uint8_t(n)};
        script.resize(script.size() + n, 'x');
        const ParsedScript parsed = Parse(script);
        BOOST_CHECK(parsed.data == std::vector<unsigned char>(n, 'x'));
        BOOST_CHECK_EQUAL(parsed.pushes, n == 0 ? 0U : 1U);
        BOOST_CHECK(parsed.flags & ROW_PUSH_ONLY);
        BOOST_CHECK(!(parsed.flags & ROW_MULTI_PUSH));
        CheckAgainstTree(script);
    }
    // OP_PUSHDATA1, 2 and 4 at the edges of their lengths, minimal or not.
    struct Case {
        uint8_t opcode;
        size_t width;
        size_t n;
    };
    for (const Case& c : std::vector<Case>{{OP_PUSHDATA1, 1, 0}, {OP_PUSHDATA1, 1, 1}, {OP_PUSHDATA1, 1, 75},
                                           {OP_PUSHDATA1, 1, 76}, {OP_PUSHDATA1, 1, 255}, {OP_PUSHDATA2, 2, 0},
                                           {OP_PUSHDATA2, 2, 255}, {OP_PUSHDATA2, 2, 256}, {OP_PUSHDATA2, 2, 65535},
                                           {OP_PUSHDATA4, 4, 0}, {OP_PUSHDATA4, 4, 65536}, {OP_PUSHDATA4, 4, 100000}}) {
        std::vector<unsigned char> script{OP_RETURN, c.opcode};
        for (size_t i = 0; i < c.width; ++i) script.push_back(uint8_t(c.n >> (8 * i)));
        const size_t offset = script.size();
        script.resize(script.size() + c.n, 'y');
        const ParsedScript parsed = Parse(script);
        BOOST_CHECK(parsed.data == std::vector<unsigned char>(c.n, 'y'));
        BOOST_CHECK_EQUAL(parsed.pushes, 1U);
        BOOST_CHECK(parsed.parts == std::vector<DataPart>({{offset, c.n}}));
        BOOST_CHECK(parsed.flags & ROW_PUSH_ONLY);
        BOOST_CHECK_EQUAL((parsed.flags & ROW_EMPTY) != 0, c.n == 0);
        CheckAgainstTree(script);
    }
    // A push longer than the interpreter's limit for stack elements is still a complete push.
    const std::vector<unsigned char> long_push = OneDataPush(std::vector<unsigned char>(MAX_SCRIPT_ELEMENT_SIZE + 1, 'z'));
    BOOST_CHECK_EQUAL(Parse(long_push).flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83);
}

BOOST_AUTO_TEST_CASE(multi_push)
{
    const ParsedScript two = Parse(Bytes("6a03616263026465"));
    BOOST_CHECK(two.data == Text("abcde"));
    BOOST_CHECK_EQUAL(two.flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_MULTI_PUSH);
    BOOST_CHECK_EQUAL(two.pushes, 2U);
    BOOST_CHECK(two.parts == std::vector<DataPart>({{2, 3}, {6, 2}}));

    // A character split over two pushes: the data is checked as UTF-8 as a whole.
    const ParsedScript split = Parse(Bytes("6a01e20282ac"));
    BOOST_CHECK(split.data == Bytes("e282ac"));
    BOOST_CHECK_EQUAL(split.flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_MULTI_PUSH);

    // Pushes of no bytes are data pushes; OP_0 is not one.
    const ParsedScript empty_pushes = Parse(Bytes("6a4c004c00"));
    BOOST_CHECK_EQUAL(empty_pushes.flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8 | ROW_MULTI_PUSH);
    BOOST_CHECK(empty_pushes.parts == std::vector<DataPart>({{3, 0}, {5, 0}}));
    const ParsedScript zeros = Parse(Bytes("6a0000"));
    BOOST_CHECK_EQUAL(zeros.flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8);
    BOOST_CHECK_EQUAL(zeros.pushes, 0U);
    const ParsedScript zero_then_data = Parse(Bytes("6a0003616263"));
    BOOST_CHECK_EQUAL(zero_then_data.flags, ROW_PUSH_ONLY | ROW_UTF8);
    BOOST_CHECK(zero_then_data.data == Text("abc"));

    // Many pushes of every kind of length.
    std::vector<unsigned char> many{OP_RETURN};
    for (int i = 0; i < 300; ++i) {
        const std::vector<unsigned char> push = OneDataPush(std::vector<unsigned char>(i % 80, uint8_t('a' + i % 26)));
        many.insert(many.end(), push.begin() + 1, push.end());
    }
    BOOST_CHECK(Parse(many).flags & ROW_MULTI_PUSH);
    CheckAgainstTree(many);
    for (const std::string hex : {"6a03616263026465", "6a01e20282ac", "6a4c004c00", "6a0000", "6a0003616263"}) {
        CheckAgainstTree(Bytes(hex));
    }
}

BOOST_AUTO_TEST_CASE(number_and_zero)
{
    BOOST_CHECK_EQUAL(Parse(Bytes("6a4f")).flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8 | ROW_NUMBER_PUSH);
    for (int op = OP_1; op <= OP_16; ++op) {
        const std::vector<unsigned char> script{OP_RETURN, uint8_t(op)};
        BOOST_CHECK_EQUAL(Parse(script).flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8 | ROW_NUMBER_PUSH);
        CheckAgainstTree(script);
    }
    const ParsedScript number_and_data = Parse(Bytes("6a5d03616263"));
    BOOST_CHECK(number_and_data.data == Text("abc"));
    BOOST_CHECK_EQUAL(number_and_data.flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_NUMBER_PUSH);
    // OP_RESERVED counts as a push for the interpreter's push-only rule; it adds nothing and is no number.
    BOOST_CHECK_EQUAL(Parse(Bytes("6a50")).flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8);
    const ParsedScript reserved = Parse(Bytes("6a50026869"));
    BOOST_CHECK(reserved.data == Text("hi"));
    BOOST_CHECK_EQUAL(reserved.flags, ROW_PUSH_ONLY | ROW_UTF8);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a0051")).flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8 | ROW_NUMBER_PUSH);
    for (const std::string hex : {"6a4f", "6a50", "6a5d03616263", "6a50026869", "6a00", "6a0051"}) {
        CheckAgainstTree(Bytes(hex));
    }
}

BOOST_AUTO_TEST_CASE(non_push_and_cutoff)
{
    // An opcode above OP_16: the data is every byte after OP_RETURN, unparsed.
    const ParsedScript checksig = Parse(Bytes("6aac"));
    BOOST_CHECK(checksig.data == Bytes("ac"));
    BOOST_CHECK_EQUAL(checksig.flags, 0); // the byte ac alone is not UTF-8
    BOOST_CHECK(checksig.parts == std::vector<DataPart>({{1, 1}}));
    const ParsedScript after_push = Parse(Bytes("6a03616263ac"));
    BOOST_CHECK(after_push.data == Bytes("03616263ac"));
    BOOST_CHECK_EQUAL(after_push.flags, 0);
    // OP_NOP is the byte of "a", so this unparsed data is text.
    BOOST_CHECK_EQUAL(Parse(Bytes("6a61")).flags, ROW_UTF8);
    // In a script that is not push-only, number pushes and several pushes set no flag.
    BOOST_CHECK_EQUAL(Parse(Bytes("6a5151ac")).flags, 0);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a01610162ac")).flags, 0);

    // A push that runs past the end.
    for (const std::string hex : {"6a05616263",      // a direct push of 5 bytes with 3
                                  "6a4c",            // OP_PUSHDATA1 without its length
                                  "6a4c05616263",    // OP_PUSHDATA1 of 5 bytes with 3
                                  "6a4d01",          // OP_PUSHDATA2 with half its length
                                  "6a4d0001",        // OP_PUSHDATA2 of 256 bytes with none
                                  "6a4e010000",      // OP_PUSHDATA4 with three bytes of its length
                                  "6a4effffffff00",  // OP_PUSHDATA4 of 4294967295 bytes with one
                                  "6a0161056263",    // a complete push, then a cut one
                                  "6aac0562"}) {     // an opcode above OP_16, then a cut push
        const std::vector<unsigned char> script = Bytes(hex);
        const ParsedScript parsed = Parse(script);
        BOOST_CHECK(parsed.flags & ROW_PARSE_ERROR);
        BOOST_CHECK(!(parsed.flags & (ROW_PUSH_ONLY | ROW_MULTI_PUSH | ROW_NUMBER_PUSH | ROW_EMPTY)));
        BOOST_CHECK(parsed.data == std::vector<unsigned char>(script.begin() + 1, script.end()));
        CheckAgainstTree(script);
    }
    for (const std::string hex : {"6aac", "6a03616263ac", "6a61", "6a5151ac", "6a01610162ac", "6a6a", "6aff"}) {
        CheckAgainstTree(Bytes(hex));
    }
}

BOOST_AUTO_TEST_CASE(empty_and_flags)
{
    // OP_RETURN alone.
    const ParsedScript alone = Parse(Bytes("6a"));
    BOOST_CHECK_EQUAL(alone.flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8);
    BOOST_CHECK_EQUAL(alone.data_size, 0U);
    BOOST_CHECK_EQUAL(alone.pushes, 0U);
    BOOST_CHECK(alone.parts.empty());
    CheckAgainstTree(Bytes("6a"));
    BOOST_CHECK_EQUAL(Parse(Bytes("6a00")).flags, ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8);

    // ROW_OVER_83 from 84 bytes on, push-only or not.
    BOOST_CHECK_EQUAL(Parse(MakeScript(83, 'a')).flags, ROW_PUSH_ONLY | ROW_UTF8);
    BOOST_CHECK_EQUAL(Parse(MakeScript(84, 'a')).flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83);
    std::vector<unsigned char> non_push(84, 0xac);
    non_push[0] = OP_RETURN;
    BOOST_CHECK_EQUAL(Parse(non_push).flags, ROW_OVER_83);
    non_push.pop_back();
    BOOST_CHECK_EQUAL(Parse(non_push).flags, 0);

    // Each flag on its own where it can be.
    BOOST_CHECK_EQUAL(Parse(Bytes("6a0161")).flags, ROW_PUSH_ONLY | ROW_UTF8);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a01ff")).flags, ROW_PUSH_ONLY);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a02ff")).flags, ROW_PARSE_ERROR);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a0261")).flags, ROW_PARSE_ERROR | ROW_UTF8);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a510161")).flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_NUMBER_PUSH);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a01610162")).flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_MULTI_PUSH);
    BOOST_CHECK_EQUAL(Parse(Bytes("6a01ff01ff")).flags, ROW_PUSH_ONLY | ROW_MULTI_PUSH);
}

BOOST_AUTO_TEST_CASE(sizes)
{
    // Scripts of the sizes at the edges of the record's layout: what a row holds, and its pieces. A script of no
    // bytes is not an OP_RETURN script.
    BOOST_CHECK(!IsOpReturn(std::vector<unsigned char>{}));
    BOOST_CHECK_EQUAL(Parse({}).flags, 0);
    BOOST_CHECK_THROW(MakeScript(0, 'a'), std::invalid_argument);
    for (const size_t size : {size_t{1}, size_t{2}, size_t{4096}, size_t{4097}, size_t{65536}, size_t{65537}, size_t{998000}}) {
        const std::vector<unsigned char> script = MakeScript(size, 'a');
        BOOST_REQUIRE_EQUAL(script.size(), size);
        const ParsedScript parsed = Parse(script);
        const size_t header = size <= 77 ? 1 : size <= 1 + 2 + 255 ? 2 : size <= 1 + 3 + 65535 ? 3 : 5;
        BOOST_CHECK_EQUAL(parsed.data_size, size == 1 ? 0 : size - 1 - header);
        BOOST_CHECK(parsed.data == std::vector<unsigned char>(parsed.data_size, 'a'));
        uint16_t flags = ROW_PUSH_ONLY | ROW_UTF8;
        if (parsed.data_size == 0) flags |= ROW_EMPTY;
        if (size > 83) flags |= ROW_OVER_83;
        BOOST_CHECK_EQUAL(parsed.flags, flags);
        CheckAgainstTree(script);
        CheckPrefixes(script, {0, 1, 2, 4095, 4096, 4097, 65535, 65536, 65537, size - 1, size});
    }
    // The same sizes, not push-only.
    for (const size_t size : {size_t{2}, size_t{4096}, size_t{4097}, size_t{65536}, size_t{65537}, size_t{998000}}) {
        std::vector<unsigned char> script(size, 'b');
        script[0] = OP_RETURN;
        script[1] = 0xac;
        const ParsedScript parsed = Parse(script);
        BOOST_CHECK_EQUAL(parsed.data_size, size - 1);
        BOOST_CHECK(!(parsed.flags & ROW_PUSH_ONLY));
        CheckPrefixes(script, {0, 1, 2, 4096, 65536, size});
    }
}

BOOST_AUTO_TEST_CASE(utf8_rules)
{
    for (const std::string hex : {"", "61", "7f", "00", "c280", "dfbf", "e0a080", "efbfbf", "ed9fbf", "ee8080",
                                  "f0908080", "f48fbfbf", "e282ac", "f09f9a80"}) {
        BOOST_CHECK_MESSAGE(IsUtf8(Bytes(hex)), hex);
        BOOST_CHECK(Parse(OneDataPush(Bytes(hex))).flags & ROW_UTF8);
    }
    for (const std::string hex : {"c080", "c1bf", "e08080", "e09fbf", "f0808080", "f08fbfbf", // overlong forms
                                  "eda080", "edbfbf",                                        // surrogates
                                  "f4908080", "f5808080", "f8", "fc", "fe", "ff",            // above U+10FFFF
                                  "80", "bf", "c2", "e282", "f09f9a", "e28241", "c2c2"}) {   // cut or broken
        BOOST_CHECK_MESSAGE(!IsUtf8(Bytes(hex)), hex);
        BOOST_CHECK(!(Parse(OneDataPush(Bytes(hex))).flags & ROW_UTF8));
    }
}

BOOST_AUTO_TEST_CASE(prefix_mode)
{
    const std::vector<std::vector<unsigned char>> scripts{
        Bytes("6a"), Bytes("6a00"), Bytes("6a04deadbeef"), Bytes("6a03616263026465"), Bytes("6a4c004c00"),
        Bytes("6a5d03616263"), Bytes("6a50026869"), Bytes("6a03616263ac"), Bytes("6a05616263"), Bytes("6a4d01"),
        Bytes("6a4e0400000041424344"), Bytes("6aac0562"), MakeScript(300, 'q'),
    };
    for (const auto& script : scripts) CheckPrefixes(script, AllSizes(script.size()));

    // Long scripts whose push-only-ness is decided only after 4,096 and after 65,536 bytes. A prefix alone could
    // not tell; the stored flags do, so the prefix's data is still the beginning of the whole data.
    const std::vector<unsigned char> late_opcode = Pushdata4(69994, 69994, 'r', {0xac});
    const std::vector<unsigned char> late_cut = Pushdata4(70000, 69994, 's');
    std::vector<unsigned char> pushes{OP_RETURN};
    while (pushes.size() < 70000) {
        pushes.push_back(0x4b);
        pushes.resize(pushes.size() + 0x4b, 't');
    }
    BOOST_CHECK_EQUAL(Parse(late_opcode).flags, ROW_OVER_83); // its last byte, ac, is not UTF-8
    BOOST_CHECK_EQUAL(Parse(late_cut).flags, ROW_PARSE_ERROR | ROW_OVER_83 | ROW_UTF8);
    const std::vector<size_t> sizes{0, 1, 2, 3, 5, 6, 7, 75, 76, 77, 4095, 4096, 4097, 65535, 65536, 65537, 69999, 70000, 70001};
    for (const auto& script : {MakeScript(70000, 'p'), late_opcode, late_cut, pushes}) {
        CheckAgainstTree(script);
        CheckPrefixes(script, sizes);
    }

    // A prefix that contradicts its flags is read as not push-only, and nothing is taken from beyond it.
    const ParsedScript contradiction = ParseOpReturnPrefix(Bytes("6a03616263ac0161"), ROW_PUSH_ONLY);
    BOOST_CHECK(contradiction.data == Bytes("03616263ac0161"));
    BOOST_CHECK_EQUAL(contradiction.flags, ROW_PUSH_ONLY);
    BOOST_CHECK_EQUAL(ParseOpReturnPrefix({}, ROW_PUSH_ONLY | ROW_UTF8).data_size, 0U);
}

BOOST_AUTO_TEST_CASE(witness_commitment)
{
    const std::vector<unsigned char> commitment = Bytes("6a24aa21a9ed" + std::string(64, '1'));
    std::vector<unsigned char> longer = commitment;
    longer.push_back(0x00);
    std::vector<unsigned char> too_short = commitment;
    too_short.pop_back();
    std::vector<unsigned char> other_byte = commitment;
    other_byte[5] = 0xee;
    const std::vector<unsigned char> payment = Bytes("0014" + std::string(40, '2'));
    const std::vector<unsigned char> message = OneDataPush(Text("hello"));

    const std::vector<std::vector<std::vector<unsigned char>>> outputs_list{
        {payment, commitment},
        {payment, commitment, message, commitment}, // an earlier look-alike that consensus does not use
        {commitment, message, longer},              // the last one counts, also when it is longer
        {payment, too_short, other_byte, message},  // no commitment
        {},
        {commitment},
    };
    for (const auto& outputs : outputs_list) {
        CBlock block;
        block.vtx.push_back(MakeTransactionRef(opreturn::test::MakeOpReturnTx(outputs, /*coinbase=*/true)));
        BOOST_CHECK_EQUAL(WitnessCommitmentIndex(*block.vtx[0]), GetWitnessCommitmentIndex(block));
    }
    BOOST_CHECK_EQUAL(WitnessCommitmentIndex(CTransaction(opreturn::test::MakeOpReturnTx({payment, commitment, message, commitment}, true))), 3);
    // In any other transaction such an output is an ordinary OP_RETURN output.
    BOOST_CHECK_EQUAL(WitnessCommitmentIndex(CTransaction(opreturn::test::MakeOpReturnTx({commitment}, /*coinbase=*/false))), NO_WITNESS_COMMITMENT);
}

BOOST_AUTO_TEST_CASE(flags_fit)
{
    BOOST_CHECK(FlagsFit(ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8, 1, 0));
    BOOST_CHECK(FlagsFit(ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83, 84, 82));
    BOOST_CHECK(FlagsFit(0, 2, 1));
    BOOST_CHECK(FlagsFit(ROW_PARSE_ERROR, 2, 1));
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8, 0, 0));             // no script
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8 | 0x0080, 1, 0));    // an unknown flag
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_UTF8, 84, 82));                       // ROW_OVER_83 missing
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83, 83, 81));         // ROW_OVER_83 wrong
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_UTF8, 1, 0));                         // ROW_EMPTY missing
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_EMPTY | ROW_UTF8, 3, 1));             // ROW_EMPTY wrong
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_EMPTY, 1, 0));                        // empty data is UTF-8
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY | ROW_PARSE_ERROR, 3, 1));                  // both
    BOOST_CHECK(!FlagsFit(ROW_MULTI_PUSH, 3, 2));                                   // only with push-only
    BOOST_CHECK(!FlagsFit(ROW_NUMBER_PUSH, 3, 2));
    BOOST_CHECK(!FlagsFit(0, 3, 1));                                                // not push-only: all after OP_RETURN
    BOOST_CHECK(!FlagsFit(ROW_PUSH_ONLY, 3, 2));                                    // no room for a push header
    BOOST_CHECK(!FlagsFit(0, 1, 0) && !FlagsFit(ROW_PARSE_ERROR, 1, 0));
    BOOST_CHECK(!FlagsFit(ROW_PARSE_ERROR | ROW_EMPTY | ROW_UTF8, 1, 0));            // OP_RETURN alone is push-only
    BOOST_CHECK(!FlagsFit(ROW_EMPTY | ROW_UTF8, 1, 0));
}

BOOST_AUTO_TEST_CASE(matches_tree)
{
    FastRandomContext rng(/*fDeterministic=*/true);
    for (int i = 0; i < 20000; ++i) {
        const std::vector<unsigned char> script = RandomScript(rng);
        CheckAgainstTree(script);
        if (i % 500 == 0) CheckPrefixes(script, AllSizes(script.size()));
    }
}

BOOST_AUTO_TEST_CASE(hostile_corpus)
{
    for (const opreturn::test::HostileSample& sample : opreturn::test::HostileCorpus()) {
        const std::vector<unsigned char> script = OneDataPush(sample.bytes);
        const ParsedScript parsed = Parse(script);
        BOOST_CHECK_MESSAGE(parsed.data == sample.bytes, sample.name);
        BOOST_CHECK_MESSAGE(((parsed.flags & ROW_UTF8) != 0) == sample.utf8, sample.name);
        BOOST_CHECK(parsed.flags & ROW_PUSH_ONLY);
        CheckAgainstTree(script);
    }
    // 64 KiB and 998,000 bytes of data in one push.
    for (const size_t size : {size_t{65536}, size_t{998000}}) {
        const std::vector<unsigned char> data(size, 0x1b);
        const ParsedScript parsed = Parse(OneDataPush(data));
        BOOST_CHECK(parsed.data == data);
        BOOST_CHECK_EQUAL(parsed.flags, ROW_PUSH_ONLY | ROW_UTF8 | ROW_OVER_83);
    }
}

BOOST_AUTO_TEST_SUITE_END()
