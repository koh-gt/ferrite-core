// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <opreturn/log.h>
#include <opreturn/names.h>
#include <opreturn/options.h>
#include <opreturn/script.h>
#include <test/util/opreturn.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/system.h>

#include <boost/test/unit_test.hpp>

#include <limits>
#include <string>
#include <vector>

using namespace opreturn;
using opreturn::test::HOSTILE_MARKER;
using opreturn::test::HostileCorpus;
using opreturn::test::HostileSample;
using opreturn::test::LogLines;

namespace {

bool EndsWith(const std::string& text, const std::string& end)
{
    return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(opreturn_log_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(formats_fixed)
{
    for (size_t i = 0; i < LOG_EVENT_COUNT; ++i) {
        const LogEvent event = LogEvent(uint8_t(i));
        const LogEventFormat& format = GetLogEventFormat(event);
        BOOST_CHECK(format.event == event);
        // One line of printable ASCII, whose only directives are "%s", one for each argument, the first first.
        const std::string text = format.text;
        BOOST_CHECK(!text.empty());
        for (const char c : text) BOOST_CHECK(c >= 0x20 && c < 0x7f);
        size_t directives = 0;
        for (size_t at = text.find('%'); at != std::string::npos; at = text.find('%', at + 2)) {
            BOOST_CHECK(at + 1 < text.size() && text[at + 1] == 's');
            ++directives;
        }
        const size_t arguments = (format.a != LogArg::NONE ? 1 : 0) + (format.b != LogArg::NONE ? 1 : 0);
        BOOST_CHECK_EQUAL(directives, arguments);
        BOOST_CHECK(format.a != LogArg::NONE || format.b == LogArg::NONE);

        // The line is the record's prefix, a space and the text, and Log writes it once with its newline.
        const std::string line = FormatLogLine(event, 2, 3);
        BOOST_CHECK(line.compare(0, std::string(LOG_PREFIX).size() + 1, std::string(LOG_PREFIX) + " ") == 0);
        BOOST_CHECK(line.find('\n') == std::string::npos);
        BOOST_CHECK(line.find('%') == std::string::npos);
        const LogLines lines;
        Log(event, 2, 3);
        const std::vector<std::string> written = lines.Containing(LOG_PREFIX);
        if (format.bench && !LogAcceptCategory(BCLog::BENCH)) {
            BOOST_CHECK(written.empty());
        } else {
            BOOST_REQUIRE_EQUAL(written.size(), 1U);
            BOOST_CHECK(EndsWith(written[0], line + "\n"));
        }
    }
    // A value that is no event reads the table's first entry, never past its end.
    BOOST_CHECK(GetLogEventFormat(LogEvent(uint8_t(LOG_EVENT_COUNT))).event == LogEvent::OPTION_IGNORED);
    BOOST_CHECK(GetLogEventFormat(LogEvent(uint8_t{255})).event == LogEvent::OPTION_IGNORED);
}

BOOST_AUTO_TEST_CASE(arguments_from_tables)
{
    for (const Option option : {Option::INDEX, Option::MAX_SIZE, Option::REINDEX}) {
        BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::OPTION, int64_t(option)), OptionName(option));
    }
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::OPTION, int64_t(Option::REINDEX) + 1), "?");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::OPTION, -1), "?");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::INTEGER, 0), "0");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::INTEGER, -5), "-5");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::INTEGER, std::numeric_limits<int64_t>::min()), "-9223372036854775808");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::INTEGER, std::numeric_limits<int64_t>::max()), "9223372036854775807");
    BOOST_CHECK_EQUAL(FormatLogArgument(LogArg::NONE, 7), "");
    BOOST_CHECK_EQUAL(FormatLogLine(LogEvent::OPTION_IGNORED, int64_t(Option::REINDEX)),
                      strprintf("%s off, so %s is ignored", LOG_PREFIX, OptionName(Option::REINDEX)));
    BOOST_CHECK_EQUAL(FormatLogLine(LogEvent::STORED_CHOICE_IGNORED, int64_t(Option::MAX_SIZE)),
                      strprintf("%s the stored choice for %s is out of range and is ignored", LOG_PREFIX, OptionName(Option::MAX_SIZE)));
    BOOST_CHECK_EQUAL(FormatLogLine(LogEvent::OPTION_IGNORED, 99), strprintf("%s off, so ? is ignored", LOG_PREFIX));
}

BOOST_AUTO_TEST_CASE(no_input_in_log)
{
    const std::vector<HostileSample> corpus = HostileCorpus();
    BOOST_REQUIRE(!corpus.empty());
    for (const HostileSample& sample : corpus) {
        const std::string value(sample.bytes.begin(), sample.bytes.end());
        BOOST_REQUIRE(value.find(HOSTILE_MARKER) != std::string::npos);

        // As the size limit, under either name: refused with the fixed message, which repeats nothing of the value.
        for (const char* name : {OptionName(Option::MAX_SIZE), OptionAlias(Option::MAX_SIZE)}) {
            ArgsManager args;
            args.ForceSetArg(name, value);
            const LogLines lines;
            Options options;
            std::string error;
            BOOST_CHECK(!ResolveOptions(args, {}, {}, /*gui_host=*/false, options, error));
            BOOST_CHECK_EQUAL(error, strprintf("%s must be 0 (no limit) or from 100 to 4194304 MiB", name));
            for (const std::string& line : lines.Lines()) BOOST_CHECK(line.find(HOSTILE_MARKER) == std::string::npos);
        }
        // As the values of an option that is logged when it has no effect, under both names: the lines name the
        // options only.
        {
            ArgsManager args;
            args.ForceSetArg(OptionName(Option::REINDEX), value);
            args.ForceSetArg(OptionAlias(Option::REINDEX), value);
            args.ForceSetArg(OptionAlias(Option::MAX_SIZE), "200");
            const LogLines lines;
            Options options;
            std::string error;
            BOOST_CHECK(ResolveOptions(args, {}, {}, /*gui_host=*/false, options, error));
            BOOST_CHECK_EQUAL(lines.Containing(LOG_PREFIX).size(), 2U);
            for (const std::string& line : lines.Lines()) BOOST_CHECK(line.find(HOSTILE_MARKER) == std::string::npos);
        }
        // As one of two names of an option set to different values: the message names the two names only.
        {
            ArgsManager args;
            args.ForceSetArg(OptionName(Option::INDEX), value);
            args.ForceSetArg(OptionAlias(Option::INDEX), args.GetBoolArg(OptionName(Option::INDEX), false) ? "0" : "1");
            const LogLines lines;
            Options options;
            std::string error;
            BOOST_CHECK(!ResolveOptions(args, {}, {}, /*gui_host=*/false, options, error));
            BOOST_CHECK_EQUAL(error, strprintf("%s is another name for %s, and the two are set to different values: remove %s",
                                               OptionAlias(Option::INDEX), OptionName(Option::INDEX), OptionAlias(Option::INDEX)));
            for (const std::string& line : lines.Lines()) BOOST_CHECK(line.find(HOSTILE_MARKER) == std::string::npos);
        }
        // Parsing it as data, whole or as a prefix, logs nothing at all.
        {
            const LogLines lines;
            const std::vector<unsigned char> script = opreturn::test::OneDataPush(sample.bytes);
            const ParsedScript parsed = ParseOpReturn(script);
            ParseOpReturnPrefix(Span<const unsigned char>{script}.first(script.size() / 2), parsed.flags);
            BOOST_CHECK(lines.Lines().empty());
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
