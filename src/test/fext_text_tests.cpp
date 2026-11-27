// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fext/text.h>
#include <fext/unicode_data.h>
#include <random.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>

#include <test/data/fext_grapheme_break_test.txt.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using fext::DisplayCounts;
using fext::DisplayMap;
using fext::DisplayMode;
using fext::Mapped;
using fext::REPLACEMENT_CHARACTER;

namespace {

using Text = std::vector<uint32_t>;

constexpr uint32_t FFFD{REPLACEMENT_CHARACTER};

Text Ascii(const std::string& s) { return Text(s.begin(), s.end()); }

Text Repeat(const Text& part, size_t n)
{
    Text out;
    for (size_t i = 0; i < n; ++i) out.insert(out.end(), part.begin(), part.end());
    return out;
}

Text Cat(std::initializer_list<Text> parts)
{
    Text out;
    for (const Text& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

Text Decode(const std::vector<unsigned char>& bytes)
{
    Text out;
    fext::DecodeUtf8(bytes, out);
    return out;
}

Text Decode(const std::string& hex) { return Decode(ParseHex(hex)); }

std::vector<unsigned char> Bytes(const std::string& s) { return {s.begin(), s.end()}; }

std::string Show(const Text& text)
{
    std::string out;
    for (size_t i = 0; i < text.size() && i < 80; ++i) out += strprintf("%s%04X", i ? " " : "", text[i]);
    if (text.size() > 80) out += strprintf(" ... (%u code points)", text.size());
    return out;
}

DisplayCounts Counts(size_t control, size_t direction, size_t invisible, size_t private_use, size_t marks, size_t cluster)
{
    DisplayCounts c;
    c.control = control;
    c.direction = direction;
    c.invisible = invisible;
    c.private_use = private_use;
    c.marks = marks;
    c.cluster = cluster;
    return c;
}

bool operator==(const DisplayCounts& a, const DisplayCounts& b)
{
    return a.control == b.control && a.direction == b.direction && a.invisible == b.invisible && a.private_use == b.private_use &&
           a.marks == b.marks && a.cluster == b.cluster;
}

std::string Show(const DisplayCounts& c)
{
    return strprintf("control %u, direction %u, invisible %u, private %u, marks %u, cluster %u", c.control, c.direction, c.invisible,
                     c.private_use, c.marks, c.cluster);
}

bool IsVariationSelector(uint32_t c)
{
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180D) || c == 0x180F;
}

//! Whether a code point counts as a combining mark of a grapheme cluster for the mark limit of DisplayMap.
bool CountsAsMark(uint32_t c)
{
    const fext::unicode::Properties p = fext::unicode::GetProperties(c);
    const auto g = p.Grapheme();
    return (g == fext::unicode::GraphemeBreak::EXTEND || g == fext::unicode::GraphemeBreak::SPACING_MARK) && c != 0x200C && c != 0x200D &&
           !IsVariationSelector(c) && !p.Has(fext::unicode::EMOJI_MODIFIER) && !(c >= 0xE0020 && c <= 0xE007F);
}

bool IsRegionalIndicator(uint32_t c) { return c >= 0x1F1E6 && c <= 0x1F1FF; }

//! The text with every line break (CR LF, LF, U+2028, U+2029) replaced by the line symbol, as one line shows it.
Text LineSymbols(const Text& text)
{
    Text out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == 0x0D && i + 1 < text.size() && text[i + 1] == 0x0A) continue;
        out.push_back(text[i] == 0x0A || text[i] == 0x2028 || text[i] == 0x2029 ? fext::LINE_SYMBOL : text[i]);
    }
    return out;
}

//! The problems of text as DisplayMap shows it, in either mode, that no input may cause.
void ShownProblems(const Text& out, const std::string& mode, std::vector<std::string>& problems)
{
    // Every grapheme cluster has at most 32 code points before the U+FFFD that replaced the rest of a longer one,
    // and at most 8 marks; no run of regional indicators is longer than 64.
    const std::vector<size_t> breaks = fext::GraphemeBreaks(out);
    for (size_t k = 0; k + 1 < breaks.size(); ++k) {
        const size_t size = breaks[k + 1] - breaks[k];
        if (size > 33 || (size == 33 && out[breaks[k + 1] - 1] != FFFD)) problems.push_back(mode + strprintf(": a cluster of %u", size));
        if (std::count_if(out.begin() + breaks[k], out.begin() + breaks[k + 1], CountsAsMark) > 8) problems.push_back(mode + ": a cluster with more than 8 marks");
    }
    size_t run = 0;
    for (const uint32_t c : out) {
        run = IsRegionalIndicator(c) ? run + 1 : 0;
        if (run == 65) problems.push_back(mode + ": a run of more than 64 regional indicators");
    }
    // No replaced class survives: no control but the line feed, no direction control, no default-ignorable code
    // point but the format characters kept in context, no private use code point, noncharacter or non-scalar value.
    for (const uint32_t c : out) {
        const bool control = (c < 0x20 && c != 0x0A) || (c >= 0x7F && c <= 0x9F);
        const bool direction = (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
        const bool context = c == 0x200C || c == 0x200D || c == 0x200E || c == 0x200F || c == 0x061C || IsVariationSelector(c) || (c >= 0xE0020 && c <= 0xE007F);
        const bool ignorable = fext::unicode::GetProperties(c).Has(fext::unicode::DEFAULT_IGNORABLE) && !context;
        const bool private_use = (c >= 0xE000 && c <= 0xF8FF) || c >= 0xF0000 || (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE || (c >= 0xD800 && c <= 0xDFFF);
        if (control || direction || ignorable || private_use || c == 0x2800 || (c >= 0xFFF9 && c <= 0xFFFC)) problems.push_back(mode + strprintf(": U+%04X kept", c));
    }
    // A tag character is kept only inside one of the three flags.
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 0xE0020 && out[i] <= 0xE007F) {
            size_t start = i;
            while (start > 0 && out[start - 1] >= 0xE0020 && out[start - 1] <= 0xE007F) --start;
            if (start == 0 || out[start - 1] != 0x1F3F4) problems.push_back(mode + ": a tag kept outside a flag");
        }
    }
}

//! The problems of a result of DisplayMap in MULTI_LINE mode, and of the same text on one line, that no input may
//! cause; the fuzz target makes the same checks.
std::vector<std::string> Problems(const Text& input, const Mapped& mapped)
{
    std::vector<std::string> problems;
    const Text& out = mapped.Text();
    if (out.size() > input.size()) problems.push_back("longer than the text");
    if (mapped.Changed() != (out != input)) problems.push_back("Changed() is wrong");
    if (!mapped.Changed() && mapped.Counts().Total() != 0) problems.push_back("counts without a change");
    // Mapping again changes nothing.
    const Mapped again = DisplayMap(out, DisplayMode::MULTI_LINE);
    if (again.Text() != out || again.Changed()) problems.push_back("not idempotent: " + Show(again.Text()));
    if (again.Counts().Total() != 0) problems.push_back("mapping again counts " + Show(again.Counts()));
    ShownProblems(out, "MULTI_LINE", problems);
    // One line: the rules applied to the text with its line breaks as line symbols, so that the limits hold for the
    // text a line shows, no line break is left, and mapping that text again on one line changes nothing.
    const Mapped one = DisplayMap(input, DisplayMode::ONE_LINE);
    if (one.Mode() != DisplayMode::ONE_LINE) problems.push_back("ONE_LINE: wrong mode");
    if (one.Text() != DisplayMap(LineSymbols(input), DisplayMode::MULTI_LINE).Text()) problems.push_back("ONE_LINE is not the mapping of the text with line symbols");
    if (std::any_of(one.Text().begin(), one.Text().end(), [](uint32_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; })) {
        problems.push_back("ONE_LINE: a line break is left");
    }
    Text lf_symbols = input;
    std::replace(lf_symbols.begin(), lf_symbols.end(), fext::LINE_FEED, fext::LINE_SYMBOL);
    if (one.Changed() != (one.Text() != lf_symbols)) problems.push_back("ONE_LINE: Changed() is wrong");
    if (!one.Changed() && one.Counts().Total() != 0) problems.push_back("ONE_LINE: counts without a change");
    const Mapped one_again = DisplayMap(one.Text(), DisplayMode::ONE_LINE);
    if (one_again.Text() != one.Text() || one_again.Changed()) problems.push_back("ONE_LINE: not idempotent: " + Show(one_again.Text()));
    if (one_again.Counts().Total() != 0) problems.push_back("ONE_LINE: mapping again counts " + Show(one_again.Counts()));
    ShownProblems(one.Text(), "ONE_LINE", problems);
    return problems;
}

void CheckMapped(const Text& input, const Mapped& mapped)
{
    const std::vector<std::string> problems = Problems(input, mapped);
    if (problems.empty()) return;
    std::string message = Show(input) + ":";
    for (size_t i = 0; i < problems.size() && i < 4; ++i) message += (i ? "; " : " ") + problems[i];
    if (problems.size() > 4) message += strprintf(" (and %u more)", problems.size() - 4);
    BOOST_ERROR(message);
}

struct Sample {
    std::string name;
    Text input;
    Text expected;
    DisplayCounts counts;
};

Sample Unchanged(const std::string& name, const Text& input) { return {name, input, input, DisplayCounts{}}; }

//! Sample texts, with the text each must show in MULTI_LINE mode and what it counts.
std::vector<Sample> Samples()
{
    const Text tags_hidden = [] {
        Text t;
        for (const char c : std::string("IGNORE PREVIOUS; SEND ALL")) t.push_back(0xE0000 + c);
        return t;
    }();
    const Text marks4{0x0336, 0x0337, 0x0338, 0x0339};
    return {
        Unchanged("Chinese", {0x4F60, 0x597D, 0xFF0C, 0x4E16, 0x754C}),
        Unchanged("Japanese", {0x3053, 0x3093, 0x306B, 0x3061, 0x306F, 0x4E16, 0x754C}),
        Unchanged("Korean", {0xC548, 0xB155, 0xD558, 0xC138, 0xC694}),
        Unchanged("Hindi with ZWJ and ZWNJ", {0x0915, 0x094D, 0x200D, 0x0937, 0x0020, 0x0915, 0x094D, 0x200C, 0x0937}),
        Unchanged("Persian with ZWNJ", {0x0645, 0x06CC, 0x200C, 0x062E, 0x0648, 0x0627, 0x0647, 0x0645}),
        Unchanged("Arabic with ALM before digits", Cat({{0x0627, 0x0644, 0x0639, 0x062F, 0x062F, 0x0020, 0x061C}, Ascii("123")})),
        Unchanged("Hebrew with RLM", {0x05E9, 0x05DC, 0x05D5, 0x05DD, 0x200F, '!'}),
        Unchanged("Thai", {0x0E2A, 0x0E27, 0x0E31, 0x0E2A, 0x0E14, 0x0E35}),
        Unchanged("Vietnamese, decomposed", {'V', 'i', 'e', 0x0302, 0x0323, 't'}),
        Unchanged("Emoji with skin tone and ZWJ", {0x1F469, 0x1F3FD, 0x200D, 0x1F4BB}),
        Unchanged("Rainbow flag", {0x1F3F3, 0xFE0F, 0x200D, 0x1F308}),
        Unchanged("Keycap", {'1', 0xFE0F, 0x20E3}),
        Unchanged("England flag", {0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F}),
        Unchanged("Eye in speech bubble", {0x1F441, 0xFE0F, 0x200D, 0x1F5E8, 0xFE0F}),
        Unchanged("Ideographic variation", {0x845B, 0xE0100}),
        Unchanged("Two lines", Ascii("line one\nline two")),
        {"CR LF", Ascii("a\r\nb"), Ascii("a\nb"), DisplayCounts{}},
        {"Override", Cat({Ascii("pay 1 FEC "), {0x202E}, Ascii("redro"), {0x202C}, Ascii(" ok")}),
         Cat({Ascii("pay 1 FEC "), {FFFD}, Ascii("redro"), {FFFD}, Ascii(" ok")}), Counts(0, 2, 0, 0, 0, 0)},
        {"Isolate", Cat({Ascii("abc"), {0x2067}, Ascii("def"), {0x2069}}), Cat({Ascii("abc"), {FFFD}, Ascii("def"), {FFFD}}), Counts(0, 2, 0, 0, 0, 0)},
        {"NUL and controls", {'a', 0x0000, 'b', 0x0007, 'c', 0x001B, 'd', 0x007F}, {'a', 0x2400, 'b', 0x2407, 'c', 0x241B, 'd', 0x2421}, Counts(4, 0, 0, 0, 0, 0)},
        {"C1", {'a', 0x0085, 'b'}, {'a', FFFD, 'b'}, Counts(1, 0, 0, 0, 0, 0)},
        {"Zero-width space", {'p', 'a', 0x200B, 'y'}, {'p', 'a', FFFD, 'y'}, Counts(0, 0, 1, 0, 0, 0)},
        {"Soft hyphen", {'p', 'a', 0x00AD, 'y'}, {'p', 'a', FFFD, 'y'}, Counts(0, 0, 1, 0, 0, 0)},
        {"Tag characters after letters", {'h', 'i', 0xE0049, 0xE0047, 0xE004E}, {'h', 'i', FFFD, FFFD, FFFD}, Counts(0, 0, 3, 0, 0, 0)},
        {"Selectors after an emoji", {0x1F600, 0xFE00, 0xFE01, 0xFE02, 0xFE03}, {0x1F600, FFFD, FFFD, FFFD, FFFD}, Counts(0, 0, 4, 0, 0, 0)},
        {"Joiner run", {'a', 0x200D, 0x200D, 0x200D, 'b'}, {'a', FFFD, FFFD, FFFD, 'b'}, Counts(0, 0, 3, 0, 0, 0)},
        {"Blank name", {0x3164, 0x3164}, {FFFD, FFFD}, Counts(0, 0, 2, 0, 0, 0)},
        {"Private use", Cat({Ascii("logo "), {0xF8FF}}), Cat({Ascii("logo "), {FFFD}}), Counts(0, 0, 0, 1, 0, 0)},
        {"Object character", {'x', 0xFFFC, 'y'}, {'x', FFFD, 'y'}, Counts(0, 0, 1, 0, 0, 0)},
        {"Stacked marks", Cat({{'Z'}, Repeat(marks4, 5), {'a'}}), Cat({{'Z'}, Repeat(marks4, 2), {FFFD, 'a'}}), Counts(0, 0, 0, 0, 12, 0)},
        {"Noncharacter", {'a', 0xFDD0, 'b'}, {'a', FFFD, 'b'}, Counts(0, 0, 0, 1, 0, 0)},
        {"Selector after a replaced character", {'a', 0x200B, 0xFE0F, 'b'}, {'a', FFFD, FFFD, 'b'}, Counts(0, 0, 2, 0, 0, 0)},
        Unchanged("Scotland flag", {0x1F3F4, 0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074, 0xE007F}),
        {"Other subdivision flag (us-ca)", {0x1F3F4, 0xE0075, 0xE0073, 0xE0063, 0xE0061, 0xE007F}, Cat({{0x1F3F4}, Repeat({FFFD}, 5)}), Counts(0, 0, 5, 0, 0, 0)},
        Unchanged("Heart with VS16", {0x2764, 0xFE0F}),
        Unchanged("Heart with VS15", {0x2764, 0xFE0E}),
        {"VS16 after a letter", {'a', 0xFE0F}, {'a', FFFD}, Counts(0, 0, 1, 0, 0, 0)},
        Unchanged("Mongolian variation", {0x1820, 0x180B}),
        Unchanged("Family", {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466}),
        Unchanged("Kiss with two skin tones", {0x1F9D1, 0x1F3FB, 0x200D, 0x2764, 0xFE0F, 0x200D, 0x1F48B, 0x200D, 0x1F9D1, 0x1F3FC}),
        Unchanged("Leading RLM before digits", Cat({{0x200F}, Ascii("123 "), {0x05E9}})),
        {"Two marks around one digit", {0x05D0, 0x200F, '1', 0x200F, 0x05D1}, {0x05D0, 0x200F, '1', FFFD, 0x05D1}, Counts(0, 0, 1, 0, 0, 0)},
        {"ZWNJ after a right-joining letter", {0x062F, 0x200C, 0x0628}, {0x062F, FFFD, 0x0628}, Counts(0, 0, 1, 0, 0, 0)},
        {"ZWJ between two letters", {'a', 0x200D, 'b'}, {'a', FFFD, 'b'}, Counts(0, 0, 1, 0, 0, 0)},
        {"Nine marks", Cat({{'a'}, Repeat({0x0301}, 9)}), Cat({{'a'}, Repeat({0x0301}, 8), {FFFD}}), Counts(0, 0, 0, 0, 1, 0)},
        Unchanged("Twenty flags", Repeat({0x1F1FA, 0x1F1F8}, 20)),
        {"Forty flags", Repeat({0x1F1FA, 0x1F1F8}, 40), Cat({Repeat({0x1F1FA, 0x1F1F8}, 32), {FFFD}}), Counts(0, 0, 0, 0, 0, 16)},
        {"1,000 regional indicators", Repeat({0x1F1E6}, 1000), Cat({Repeat({0x1F1E6}, 64), {FFFD}}), Counts(0, 0, 0, 0, 0, 936)},
        Unchanged("Spaces only", Cat({Repeat({0x0020}, 50), Repeat({0x3000}, 5)})),
        {"A hidden flag", Cat({Ascii("hi "), {0x1F3F4}, tags_hidden, {0xE007F}}), Cat({Ascii("hi "), {0x1F3F4}, Repeat({FFFD}, 26)}), Counts(0, 0, 26, 0, 0, 0)},
        {"Selectors after letters", {'h', 0xE0100, 'e', 0xE0125, 'l', 0xE014A, 'l', 0xE016F, 'o', 0xE0194},
         {'h', FFFD, 'e', FFFD, 'l', FFFD, 'l', FFFD, 'o', FFFD}, Counts(0, 0, 5, 0, 0, 0)},
        {"ZWNJ in a word", {'s', 'c', 0x200C, 'a', 'm'}, {'s', 'c', FFFD, 'a', 'm'}, Counts(0, 0, 1, 0, 0, 0)},
        {"Direction marks in a word", {'p', 0x200E, 'a', 0x200F, 'y'}, {'p', FFFD, 'a', FFFD, 'y'}, Counts(0, 0, 2, 0, 0, 0)},
        {"Marks across joiners", Cat({{'Z'}, Repeat(Cat({marks4, marks4, {0x200D}}), 50), {'a'}}),
         Cat({{'Z'}, Repeat(Cat({marks4, marks4, {FFFD}}), 50), {'a'}}), Counts(0, 0, 50, 0, 0, 0)},
        {"Hangul jamo run", Repeat({0x1100}, 131072), Cat({Repeat({0x1100}, 32), {FFFD}}), Counts(0, 0, 0, 0, 0, 131040)},
        {"Spacing marks", Cat({{'a'}, Repeat({0x0903}, 131072)}), Cat({{'a'}, Repeat({0x0903}, 8), {FFFD}}), Counts(0, 0, 0, 0, 131064, 0)},
        {"Emoji chain", Cat({Repeat({0x1F469, 0x200D}, 40000), {0x1F469}}), Cat({Repeat({0x1F469, 0x200D}, 15), {0x1F469, FFFD}}), Counts(0, 0, 0, 0, 0, 79970)},
        {"Emoji modifiers", Cat({{0x1F44D}, Repeat({0x1F3FD}, 40000)}), Cat({{0x1F44D}, Repeat({0x1F3FD}, 31), {FFFD}}), Counts(0, 0, 0, 0, 0, 39969)},
        {"Braille blanks", Cat({Repeat({0x2800}, 10), {'x'}}), Cat({Repeat({FFFD}, 10), {'x'}}), Counts(0, 0, 10, 0, 0, 0)},
    };
}

//! Structured random text: code points and runs from small sets that exercise the context rules and the caps.
Text RandomText(FastRandomContext& rng)
{
    static const Text ALPHABET{
        'a', 'b', '1', ' ', '!', 0x0A, 0x0D, 0x2028, 0x00, 0x07, 0x1B, 0x7F, 0x85, 0x0628, 0x062F, 0x0627, 0x0645, 0x06CC, 0x064B, 0x0651,
        0x0640, 0x0600, 0x05D0, 0x05E9, 0x0915, 0x0937, 0x094D, 0x093F, 0x0903, 0x0901, 0x0B95, 0x0BCD, 0x1100, 0x1161, 0x11A8, 0xAC00,
        0xAC01, 0x0301, 0x0336, 0x20E3, 0x1F469, 0x1F468, 0x2764, 0x1F44D, 0x1F3FD, 0x1F3F3, 0x1F308, 0x1F600, 0x1F3F4, 0x0031, 0x2620,
        0x1F1FA, 0x1F1F8, 0x200C, 0x200D, 0x200E, 0x200F, 0x061C, 0xFE0E, 0xFE0F, 0xFE00, 0xFE01, 0xE0100, 0x180B, 0xE0067, 0xE0062,
        0xE0065, 0xE006E, 0xE007F, 0xE0041, 0x200B, 0x00AD, 0x2060, 0xFEFF, 0x3164, 0x202E, 0x2066, 0x2800, 0xFFFC, 0xE000, 0xFDD0,
        0xFFFD, 0x4E00, 0x845B, 0x1820, 0x1031, 0x1000, 0x0378, 0x0E33, 0xFF9E, 0x13430, 0x070F, 0x0890, 0x2E3A, 0x1F9B0, 0xD800, 0x110000,
    };
    static const std::vector<Text> GROUPS{
        {0x0628, 0x062F, 0x0627, 0x0645, 0x064B, 0x0651, 0x0640, 0x200C, 0x200D, 0xFE00, 0x200E, 0x070F, 0x13430, 0x0600},
        {0x0915, 0x0937, 0x094D, 0x093F, 0x0903, 0x0901, 0x200C, 0x200D, 0x0B95, 0x0BCD, 0x0301},
        {0x200E, 0x200F, 0x061C, 0x31, 0x20, 0x61, 0x05D0, 0x0627, 0x0301, 0x0903, 0x0A, 0x1F1FA, 0xFFFD, 0x0600},
        {0x1F469, 0x200D, 0xFE0F, 0x1F3FD, 0x2764, 0x0301, 0x1F3F4, 0xE0067, 0xE007F, 0x20E3, 0x31},
        {0x1100, 0x1161, 0x11A8, 0xAC00, 0xAC01, 0x0301, 0x200D, 0x200E},
        {0x0A, 0x0D, 0x2028, 0x0301, 0x1F3FD, 0x200C, 0x200E, 0x0600, 0xFE0F},
    };
    static const std::vector<Text> UNITS{
        {0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F}, {0x31, 0xFE0F, 0x20E3}, {0x1F469, 0x200D, 0x1F4BB},
        {0x1F3F3, 0xFE0F, 0x200D, 0x1F308}, {0x0915, 0x094D, 0x200D, 0x0937}, {0x0645, 0x06CC, 0x200C, 0x062E}, {0x0D, 0x0A},
    };
    static const size_t RUNS[]{2, 3, 7, 8, 9, 30, 31, 32, 33, 63, 64, 65};
    const Text* group = rng.randrange(10) < 6 ? &GROUPS[rng.randrange(GROUPS.size())] : nullptr;
    Text out;
    for (size_t n = 1 + rng.randrange(14); n > 0; --n) {
        const Text& pool = group && rng.randrange(100) < 85 ? *group : ALPHABET;
        const uint64_t r = rng.randrange(10);
        if (r < 6) {
            out.push_back(pool[rng.randrange(pool.size())]);
        } else if (r < 7) {
            const Text& unit = UNITS[rng.randrange(UNITS.size())];
            out.insert(out.end(), unit.begin(), unit.end());
        } else {
            out.insert(out.end(), RUNS[rng.randrange(sizeof(RUNS) / sizeof(RUNS[0]))], pool[rng.randrange(pool.size())]);
        }
    }
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(fext_text_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(utf8_maximal_subparts)
{
    // An overlong form, a surrogate, a cut sequence, a code point above U+10FFFF and a lone continuation byte, and the
    // example of the Unicode Standard, chapter 3 (Table 3-8).
    BOOST_CHECK(Decode("61c0af62") == (Text{'a', FFFD, FFFD, 'b'}));
    BOOST_CHECK(Decode("61eda08062") == (Text{'a', FFFD, FFFD, FFFD, 'b'}));
    BOOST_CHECK(Decode("61e4bd62") == (Text{'a', FFFD, 'b'}));
    BOOST_CHECK(Decode("61f490808062") == (Text{'a', FFFD, FFFD, FFFD, FFFD, 'b'}));
    BOOST_CHECK(Decode("618062") == (Text{'a', FFFD, 'b'}));
    BOOST_CHECK(Decode("61f18080e180c262806380bf64") == (Text{'a', FFFD, FFFD, FFFD, 'b', FFFD, 'c', FFFD, FFFD, 'd'}));

    // The widely used UTF-8 decoder capability and stress test, by its section numbers.
    BOOST_CHECK(Decode("cebae1bdb9cf83cebcceb5") == (Text{0x03BA, 0x1F79, 0x03C3, 0x03BC, 0x03B5}));    // 1
    BOOST_CHECK(Decode("00c280e0a080f0908080") == (Text{0x0000, 0x0080, 0x0800, 0x10000}));            // 2.1
    BOOST_CHECK(Decode("7fdfbfefbfbf") == (Text{0x007F, 0x07FF, 0xFFFF}));                             // 2.2
    BOOST_CHECK(Decode("f7bfbfbf") == Text(4, FFFD));                                                  // 2.2, above U+10FFFF
    BOOST_CHECK(Decode("ed9fbfee8080efbfbdf48fbfbf") == (Text{0xD7FF, 0xE000, 0xFFFD, 0x10FFFF}));    // 2.3
    BOOST_CHECK(Decode("f4908080") == Text(4, FFFD));                                                  // 2.3
    BOOST_CHECK(Decode("80") == Text(1, FFFD));                                                        // 3.1
    BOOST_CHECK(Decode("80bf80bf") == Text(4, FFFD));
    std::vector<unsigned char> continuation;
    for (int b = 0x80; b <= 0xBF; ++b) continuation.push_back(b);
    BOOST_CHECK(Decode(continuation) == Text(64, FFFD));
    for (int lead = 0xC0; lead <= 0xFF; ++lead) {                                                       // 3.2, 3.5
        BOOST_CHECK(Decode(std::vector<unsigned char>{static_cast<unsigned char>(lead), 0x20}) == (Text{FFFD, 0x20}));
    }
    BOOST_CHECK(Decode("c0") == Text(1, FFFD));                                                        // 3.3
    BOOST_CHECK(Decode("e080") == Text(2, FFFD));
    BOOST_CHECK(Decode("f08080") == Text(3, FFFD));
    BOOST_CHECK(Decode("df") == Text(1, FFFD));
    BOOST_CHECK(Decode("efbf") == Text(1, FFFD));
    BOOST_CHECK(Decode("f7bfbf") == Text(3, FFFD));
    BOOST_CHECK(Decode("f48fbf") == Text(1, FFFD));
    BOOST_CHECK(Decode("c0e080f08080dfefbff7bfbff48fbf") == Text(12, FFFD));                          // 3.4
    BOOST_CHECK(Decode("fefeffff") == Text(4, FFFD));                                                  // 3.5
    BOOST_CHECK(Decode("c0af") == Text(2, FFFD));                                                      // 4.1
    BOOST_CHECK(Decode("e080af") == Text(3, FFFD));
    BOOST_CHECK(Decode("f08080af") == Text(4, FFFD));
    BOOST_CHECK(Decode("f8808080af") == Text(5, FFFD));
    BOOST_CHECK(Decode("fc80808080af") == Text(6, FFFD));
    BOOST_CHECK(Decode("c1bf") == Text(2, FFFD));                                                      // 4.2
    BOOST_CHECK(Decode("e09fbf") == Text(3, FFFD));
    BOOST_CHECK(Decode("f08fbfbf") == Text(4, FFFD));
    BOOST_CHECK(Decode("c080") == Text(2, FFFD));                                                      // 4.3
    BOOST_CHECK(Decode("e08080") == Text(3, FFFD));
    BOOST_CHECK(Decode("f0808080") == Text(4, FFFD));
    for (const char* surrogate : {"eda080", "edadbf", "edae80", "edafbf", "edb080", "edbe80", "edbfbf"}) { // 5.1
        BOOST_CHECK(Decode(surrogate) == Text(3, FFFD));
    }
    BOOST_CHECK(Decode("eda080edb080") == Text(6, FFFD));                                             // 5.2
    BOOST_CHECK(Decode("efbfbeefbfbfefb790") == (Text{0xFFFE, 0xFFFF, 0xFDD0}));                      // 5.3

    // The first byte of each code point.
    Text out;
    std::vector<uint32_t> offsets;
    fext::DecodeUtf8(ParseHex("61f18080e180c262c3a9f09f9880"), out, &offsets);
    BOOST_CHECK(out == (Text{'a', FFFD, FFFD, FFFD, 'b', 0x00E9, 0x1F600}));
    BOOST_CHECK(offsets == (std::vector<uint32_t>{0, 1, 4, 6, 7, 8, 10}));
    fext::DecodeUtf8({}, out, &offsets);
    BOOST_CHECK(out.empty() && offsets.empty());

    // Against an independent reading of the table of well-formed byte sequences (Unicode Standard, Table 3-7), over
    // every string of two bytes and random strings of up to 12 bytes.
    struct Form {
        unsigned char first_lo, first_hi, second_lo, second_hi;
        size_t length;
    };
    static const Form FORMS[]{{0x00, 0x7F, 0x00, 0x00, 1}, {0xC2, 0xDF, 0x80, 0xBF, 2}, {0xE0, 0xE0, 0xA0, 0xBF, 3},
                              {0xE1, 0xEC, 0x80, 0xBF, 3}, {0xED, 0xED, 0x80, 0x9F, 3}, {0xEE, 0xEF, 0x80, 0xBF, 3},
                              {0xF0, 0xF0, 0x90, 0xBF, 4}, {0xF1, 0xF3, 0x80, 0xBF, 4}, {0xF4, 0xF4, 0x80, 0x8F, 4}};
    const auto reference = [&](const std::vector<unsigned char>& bytes) {
        Text text;
        for (size_t i = 0; i < bytes.size();) {
            const Form* form = std::find_if(std::begin(FORMS), std::end(FORMS), [&](const Form& f) { return bytes[i] >= f.first_lo && bytes[i] <= f.first_hi; });
            if (form == std::end(FORMS)) {
                text.push_back(FFFD);
                ++i;
                continue;
            }
            size_t matched = 1;
            while (matched < form->length && i + matched < bytes.size()) {
                const unsigned char b = bytes[i + matched];
                if (b < (matched == 1 ? form->second_lo : 0x80) || b > (matched == 1 ? form->second_hi : 0xBF)) break;
                ++matched;
            }
            if (matched < form->length) {
                text.push_back(FFFD);
            } else {
                static const unsigned char LEAD_MASK[]{0, 0x7F, 0x1F, 0x0F, 0x07};
                uint32_t cp = bytes[i] & LEAD_MASK[form->length];
                for (size_t k = 1; k < form->length; ++k) cp = (cp << 6) | (bytes[i + k] & 0x3F);
                text.push_back(cp);
            }
            i += matched;
        }
        return text;
    };
    for (int a = 0; a < 256; ++a) {
        for (int b = 0; b < 256; ++b) {
            const std::vector<unsigned char> bytes{static_cast<unsigned char>(a), static_cast<unsigned char>(b)};
            if (Decode(bytes) != reference(bytes)) BOOST_ERROR(HexStr(bytes));
        }
    }
    FastRandomContext rng{uint256S("fe")};
    for (int k = 0; k < 200000; ++k) {
        std::vector<unsigned char> bytes(1 + rng.randrange(12));
        // Mostly bytes that start or continue sequences, so that the forms are reached often.
        for (unsigned char& b : bytes) b = rng.randrange(4) == 0 ? rng.randbits(8) : static_cast<unsigned char>(0x80 + rng.randrange(0x78));
        if (Decode(bytes) != reference(bytes)) BOOST_ERROR(HexStr(bytes));
    }
}

BOOST_AUTO_TEST_CASE(display_samples)
{
    size_t count = 0;
    for (const Sample& sample : Samples()) {
        const Mapped mapped = DisplayMap(sample.input, DisplayMode::MULTI_LINE);
        BOOST_CHECK_MESSAGE(mapped.Text() == sample.expected, sample.name + ": " + Show(mapped.Text()));
        BOOST_CHECK_MESSAGE(mapped.Counts() == sample.counts, sample.name + ": " + Show(mapped.Counts()));
        BOOST_CHECK_MESSAGE(mapped.Changed() == (sample.expected != sample.input), sample.name);
        BOOST_CHECK(mapped.Mode() == DisplayMode::MULTI_LINE);
        CheckMapped(sample.input, mapped);
        // As bytes, decoded first, they give the same.
        BOOST_CHECK(Decode(Bytes(fext::EncodeUtf8(sample.input))) == sample.input);
        ++count;
    }
    BOOST_CHECK_EQUAL(count, 59U);

    const Mapped one_line = DisplayMap(Ascii("line one\nline two"), DisplayMode::ONE_LINE);
    BOOST_CHECK(one_line.Text() == Cat({Ascii("line one"), {fext::LINE_SYMBOL}, Ascii("line two")}));
    BOOST_CHECK(!one_line.Changed() && one_line.Mode() == DisplayMode::ONE_LINE);
}

BOOST_AUTO_TEST_CASE(direction_controls)
{
    for (const uint32_t c : {0x202A, 0x202B, 0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069}) {
        // Anywhere: at the start, between letters, inside an emoji sequence, after a mark, at the end.
        for (const Text& text : {Text{c, 'a'}, Text{'a', c, 'b'}, Text{0x1F469, c, 0x200D, 0x1F4BB}, Text{'a', 0x0301, c}, Text{0x05D0, c}}) {
            const Mapped mapped = DisplayMap(text, DisplayMode::MULTI_LINE);
            BOOST_CHECK(std::find(mapped.Text().begin(), mapped.Text().end(), c) == mapped.Text().end());
            BOOST_CHECK_EQUAL(mapped.Counts().direction, 1U);
            CheckMapped(text, mapped);
        }
    }
    // A run of overrides is replaced one by one.
    const Mapped run = DisplayMap(Text(1000, 0x202E), DisplayMode::ONE_LINE);
    BOOST_CHECK(run.Text() == Text(1000, FFFD));
    BOOST_CHECK_EQUAL(run.Counts().direction, 1000U);
}

BOOST_AUTO_TEST_CASE(invisibles)
{
    // Every default-ignorable code point between two Latin letters is replaced: none is needed there, the
    // joiners, direction marks and variation selectors included.
    size_t checked = 0;
    for (uint32_t c = 0; c <= fext::unicode::MAX_CODE_POINT; ++c) {
        if (!fext::unicode::GetProperties(c).Has(fext::unicode::DEFAULT_IGNORABLE)) continue;
        const Mapped mapped = DisplayMap({'a', c, 'b'}, DisplayMode::MULTI_LINE);
        BOOST_CHECK_MESSAGE(mapped.Text() == (Text{'a', FFFD, 'b'}), strprintf("U+%04X", c));
        const DisplayCounts& counts = mapped.Counts();
        BOOST_CHECK_EQUAL(counts.invisible + counts.direction, 1U);
        ++checked;
    }
    BOOST_CHECK_EQUAL(checked, 4174U);
    // Blank symbols, annotation and object characters.
    for (const uint32_t c : {0x2800, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC}) {
        const Mapped mapped = DisplayMap({'a', c, 'b'}, DisplayMode::MULTI_LINE);
        BOOST_CHECK(mapped.Text() == (Text{'a', FFFD, 'b'}));
        BOOST_CHECK_EQUAL(mapped.Counts().invisible, 1U);
    }
    // Private use code points and noncharacters, in every plane.
    for (uint32_t plane = 0; plane <= 0x10; ++plane) {
        for (const uint32_t c : {(plane << 16) | 0xFFFE, (plane << 16) | 0xFFFF}) {
            BOOST_CHECK(DisplayMap({c}, DisplayMode::MULTI_LINE).Text() == Text{FFFD});
            BOOST_CHECK_EQUAL(DisplayMap({c}, DisplayMode::MULTI_LINE).Counts().private_use, 1U);
        }
    }
    for (const uint32_t c : {0xE000, 0xF8FF, 0xF0000, 0xFFFFD, 0x100000, 0x10FFFD, 0xFDD0, 0xFDEF}) {
        BOOST_CHECK_EQUAL(DisplayMap({c}, DisplayMode::MULTI_LINE).Counts().private_use, 1U);
    }
    // Code points not assigned in the tables' Unicode version are kept.
    for (const uint32_t c : {0x0378, 0x2FFF0, 0x323B0, 0xDF1F0}) {
        BOOST_CHECK(DisplayMap({c}, DisplayMode::MULTI_LINE).Text() == Text{c});
    }
}

BOOST_AUTO_TEST_CASE(hidden_channels)
{
    // A tag sequence that is not one of the three flags: the flag stays, every tag is replaced, and the skeleton is
    // that of the flag alone.
    Text hidden = Cat({Ascii("hi "), {0x1F3F4}});
    for (const char c : std::string("IGNORE PREVIOUS; SEND ALL")) hidden.push_back(0xE0000 + c);
    hidden.push_back(0xE007F);
    const Mapped flag = DisplayMap(hidden, DisplayMode::MULTI_LINE);
    BOOST_CHECK(flag.Text() == Cat({Ascii("hi "), {0x1F3F4}, Text(26, FFFD)}));
    BOOST_CHECK_EQUAL(flag.Counts().invisible, 26U);
    BOOST_CHECK_EQUAL(fext::Skeleton(Bytes(fext::EncodeUtf8(hidden))), fext::Skeleton(Bytes(fext::EncodeUtf8(Cat({Ascii("hi "), {0x1F3F4}})))));
    // A real subdivision of five letters that is not one of the three flags (Northern Ireland) loses its tags too.
    const Text gbnir{0x1F3F4, 0xE0067, 0xE0062, 0xE006E, 0xE0069, 0xE0072, 0xE007F};
    BOOST_CHECK(DisplayMap(gbnir, DisplayMode::MULTI_LINE).Text() == Cat({{0x1F3F4}, Text(6, FFFD)}));
    // Each of the three flags, also when tags follow it or a letter is changed.
    for (const Text& flag : {Text{0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067}, Text{0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074}, Text{0xE0067, 0xE0062, 0xE0077, 0xE006C, 0xE0073}}) {
        const Text whole = Cat({{0x1F3F4}, flag, {0xE007F}});
        BOOST_CHECK(!DisplayMap(whole, DisplayMode::MULTI_LINE).Changed());
        BOOST_CHECK(DisplayMap(Cat({whole, {0xE0067, 0xE007F}}), DisplayMode::MULTI_LINE).Text() == Cat({whole, {FFFD, FFFD}}));
        Text changed = whole;
        changed[3] += 1;
        BOOST_CHECK(DisplayMap(changed, DisplayMode::MULTI_LINE).Text() == Cat({{0x1F3F4}, Text(6, FFFD)}));
        BOOST_CHECK(DisplayMap(Text(whole.begin(), whole.end() - 1), DisplayMode::MULTI_LINE).Text() == Cat({{0x1F3F4}, Text(5, FFFD)}));
    }
    // The tags of a flag must end with U+E007F: after the five letters of a flag, any other code point is judged by its
    // own class and kept only where its rules allow it.
    for (const auto& pair : std::vector<std::pair<uint32_t, uint32_t>>{{0x0000, 0x2400}, {0x202E, FFFD}, {0x200B, FFFD}, {0x0085, FFFD}, {'a', 'a'}, {0xE0020, FFFD}, {0xE0067, FFFD}}) {
        const Text text{0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, pair.first};
        BOOST_CHECK(DisplayMap(text, DisplayMode::MULTI_LINE).Text() == Cat({{0x1F3F4}, Text(5, FFFD), {pair.second}}));
    }
    // A random hidden flag of any length gets past neither the display nor the skeleton.
    FastRandomContext rng{uint256S("fa")};
    for (int k = 0; k < 1000; ++k) {
        Text text = Cat({Ascii("hi "), {0x1F3F4}});
        for (size_t n = rng.randrange(40); n > 0; --n) text.push_back(0xE0020 + rng.randrange(0x5F));
        if (rng.randbool()) text.push_back(0xE007F);
        const Mapped mapped = DisplayMap(text, DisplayMode::MULTI_LINE);
        BOOST_CHECK(Text(mapped.Text().begin(), mapped.Text().begin() + 4) == Cat({Ascii("hi "), {0x1F3F4}}));
        // Only the three flags themselves survive.
        std::string letters;
        for (size_t i = 4; i < text.size(); ++i) letters += static_cast<char>(text[i] - 0xE0000);
        const bool flag = letters == "gbeng\x7f" || letters == "gbsct\x7f" || letters == "gbwls\x7f";
        BOOST_CHECK(mapped.Text() == (flag ? text : Cat({Ascii("hi "), {0x1F3F4}, Text(text.size() - 4, FFFD)})));
        BOOST_CHECK_EQUAL(fext::Skeleton(Bytes(fext::EncodeUtf8(text))), "hi" + fext::EncodeUtf8({0x1F3F4}));
    }
    // A variation selector after each Latin letter; joiners and direction marks between letters.
    const Text selectors{'h', 0xE0100, 'e', 0xE0125, 'l', 0xE014A, 'l', 0xE016F, 'o', 0xE0194};
    BOOST_CHECK(DisplayMap(selectors, DisplayMode::MULTI_LINE).Text() == (Text{'h', FFFD, 'e', FFFD, 'l', FFFD, 'l', FFFD, 'o', FFFD}));
    BOOST_CHECK(fext::FoldForSearch(selectors, false) == Ascii("hello"));
    BOOST_CHECK(DisplayMap({'s', 'c', 0x200C, 'a', 'm'}, DisplayMode::MULTI_LINE).Text() == (Text{'s', 'c', FFFD, 'a', 'm'}));
    BOOST_CHECK(fext::FoldForSearch({'s', 'c', 0x200C, 'a', 'm'}, false) == Ascii("scam"));
    BOOST_CHECK(DisplayMap({'p', 0x200E, 'a', 0x200F, 'y'}, DisplayMode::MULTI_LINE).Text() == (Text{'p', FFFD, 'a', FFFD, 'y'}));
    BOOST_CHECK(fext::FoldForSearch({'p', 0x200E, 'a', 0x200F, 'y'}, false) == Ascii("pay"));
    // Every selector after a letter that has no such sequence, and two selectors in a row, are replaced.
    for (uint32_t s = 0xE0100; s <= 0xE01EF; ++s) {
        BOOST_CHECK(DisplayMap({'a', s}, DisplayMode::MULTI_LINE).Text() == (Text{'a', FFFD}));
        BOOST_CHECK(DisplayMap({0x845B, s, s}, DisplayMode::MULTI_LINE).Text() == (Text{0x845B, s, FFFD}));
    }
    for (uint32_t s = 0xFE00; s <= 0xFE0F; ++s) BOOST_CHECK(DisplayMap({'a', s}, DisplayMode::MULTI_LINE).Text() == (Text{'a', FFFD}));
    // The sequences that are kept: a standardized variant, a Mongolian letter's selector, a joiner after a virama.
    BOOST_CHECK(!DisplayMap({0x2229, 0xFE00}, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK(DisplayMap({0x2229, 0xFE01}, DisplayMode::MULTI_LINE).Changed());
    // After a Mongolian letter only the four Mongolian selectors are kept, not the other selectors of U+FE00 to U+FE0F.
    for (const uint32_t selector : {0x180B, 0x180C, 0x180D, 0x180F}) BOOST_CHECK(!DisplayMap({0x1820, selector}, DisplayMode::MULTI_LINE).Changed());
    for (uint32_t selector = 0xFE00; selector <= 0xFE0F; ++selector) BOOST_CHECK(DisplayMap({0x1820, selector}, DisplayMode::MULTI_LINE).Text() == (Text{0x1820, FFFD}));
    BOOST_CHECK(DisplayMap({'a', 0x180B}, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK(!DisplayMap({0x0D15, 0x0D4D, 0x200D, 0x0D15}, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK(DisplayMap({0x0D15, 0x0D4D, 0x200D}, DisplayMode::MULTI_LINE).Text() == (Text{0x0D15, 0x0D4D, FFFD}));
    // After a virama a joiner is kept only before a letter.
    BOOST_CHECK(DisplayMap({0x0915, 0x094D, 0x200C, '1'}, DisplayMode::MULTI_LINE).Text() == (Text{0x0915, 0x094D, FFFD, '1'}));
    // A joiner after an emoji is kept only before another emoji.
    BOOST_CHECK(DisplayMap({0x1F469, 0x200D, 'b'}, DisplayMode::MULTI_LINE).Text() == (Text{0x1F469, FFFD, 'b'}));
    BOOST_CHECK(DisplayMap({0x1F469, 0x200D}, DisplayMode::MULTI_LINE).Text() == (Text{0x1F469, FFFD}));
    BOOST_CHECK(DisplayMap({0x1F469, 0x200D, 0x20}, DisplayMode::MULTI_LINE).Text() == (Text{0x1F469, FFFD, 0x20}));
    // U+200C between joining letters: Dual_Joining or Left_Joining before it, Dual_Joining or Right_Joining after it.
    BOOST_CHECK(!DisplayMap({0x0645, 0x06CC, 0x200C, 0x0631, 0x0648, 0x062F}, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK(!DisplayMap({0xA872, 0x200C, 0xA840}, DisplayMode::MULTI_LINE).Changed());
    // A direction mark sees the next code point as pass 1 maps it: here U+FFFD, which is not strong.
    BOOST_CHECK(DisplayMap({'a', 0x200E, 0xE000}, DisplayMode::MULTI_LINE).Text() == (Text{'a', 0x200E, FFFD}));
    // One direction mark between strong characters: runs of marks keep at most one per run of weak characters.
    BOOST_CHECK(DisplayMap(Cat({Ascii("1"), {0x200E}, Ascii("2"), {0x200E}, Ascii("3")}), DisplayMode::MULTI_LINE).Text() ==
                Cat({Ascii("1"), {0x200E}, Ascii("2"), {FFFD}, Ascii("3")}));
}

BOOST_AUTO_TEST_CASE(search_fold)
{
    const auto finds = [](const Text& needle, const Text& haystack, bool case_insensitive) {
        const Text n = fext::FoldForSearch(needle, case_insensitive);
        const Text h = fext::FoldForSearch(haystack, case_insensitive);
        return std::search(h.begin(), h.end(), n.begin(), n.end()) != h.end();
    };
    BOOST_CHECK(finds(Ascii("scam"), {'s', 'c', 0x200C, 'a', 'm'}, true));
    BOOST_CHECK(finds(Ascii("scam"), {'s', 0x200E, 'c', 0x200F, 'a', 'm'}, true));
    BOOST_CHECK(finds(Ascii("scam"), {'s', 0xE0100, 'c', 0xE0101, 'a', 'm'}, true));
    BOOST_CHECK(finds(Ascii("scam"), {'s', 0x200B, 'c', 0x00AD, 'a', 0x2060, 'm', 0xFEFF}, true));
    BOOST_CHECK(finds(Ascii("pay"), {'p', 0x200E, 'a', 0x200F, 'y'}, true));
    // Format characters that are not default-ignorable hide nothing either.
    for (const uint32_t c : {0x0600, 0x0605, 0x06DD, 0x070F, 0x0890, 0x0891, 0x08E2, 0x110BD, 0x110CD, 0x13430, 0x1343F, 0xFFF9, 0xFFFB}) {
        BOOST_CHECK_MESSAGE(finds(Ascii("scam"), {'s', c, 'c', 'a', 'm'}, true), strprintf("U+%04X", c));
    }
    BOOST_CHECK(finds(Ascii("hello"), {'h', 0xE0100, 'e', 0xE0125, 'l', 'l', 'o'}, true));
    BOOST_CHECK(finds(Ascii("SCAM"), Ascii("a scam"), true));
    BOOST_CHECK(!finds(Ascii("SCAM"), Ascii("a scam"), false));
    // Text hidden in tag characters is never found: tags are dropped, not read as letters.
    BOOST_CHECK(!finds(Ascii("hid"), {'x', 0xE0068, 0xE0069, 0xE0064, 'y'}, true));
    BOOST_CHECK(finds(Ascii("xy"), {'x', 0xE0068, 0xE0069, 0xE0064, 'y'}, true));
    // Every white space character is U+0020 to search, so that words apart by a no-break space or an ideographic space
    // are found with U+0020 between them; the display keeps each space as it is.
    BOOST_CHECK(finds(Ascii("scam alert"), Cat({Ascii("scam"), {0x00A0}, Ascii("alert")}), true));
    BOOST_CHECK(finds(Ascii("scam alert"), Cat({Ascii("scam"), {0x3000}, Ascii("alert")}), false));
    BOOST_CHECK(!finds(Ascii("scam alert"), Ascii("scamalert"), true));
    BOOST_CHECK(!DisplayMap(Cat({Ascii("scam"), {0x00A0}, Ascii("alert"), {0x2009, 0x3000}}), DisplayMode::MULTI_LINE).Changed());
    size_t spaces = 0;
    for (uint32_t c = 0; c <= fext::unicode::MAX_CODE_POINT; ++c) {
        if (!fext::unicode::GetProperties(c).Has(fext::unicode::WHITE_SPACE)) continue;
        ++spaces;
        for (const bool ci : {false, true}) BOOST_CHECK_MESSAGE(fext::FoldForSearch({'A', c, 'B'}, ci) == Ascii(ci ? "a b" : "A B"), strprintf("U+%04X", c));
    }
    BOOST_CHECK_EQUAL(spaces, 25U);
    // Simple case folding, also outside ASCII; marks and U+0020 stay.
    BOOST_CHECK(fext::FoldForSearch({0x0391, 0x00C9, 0x1E9E, 0x0130, 0x13A0, 0xAB70}, true) == (Text{0x03B1, 0x00E9, 0x00DF, 0x0130, 0x13A0, 0x13A0}));
    BOOST_CHECK(fext::FoldForSearch({'A', 0x0301, ' ', 'B'}, true) == (Text{'a', 0x0301, ' ', 'b'}));
    BOOST_CHECK(fext::FoldForSearch({'A', 0x0301, ' ', 'B'}, false) == (Text{'A', 0x0301, ' ', 'B'}));
    // The folded text holds no default-ignorable code point, no format character and no white space but U+0020.
    Text all;
    for (uint32_t c = 0; c <= fext::unicode::MAX_CODE_POINT; ++c) {
        if (c < 0xD800 || c > 0xDFFF) all.push_back(c);
    }
    for (const bool ci : {false, true}) {
        // 4,174 default-ignorable code points and 170 format characters, 4,206 in all, are dropped; the 25 white space
        // characters are U+0020.
        const Text folded = fext::FoldForSearch(all, ci);
        BOOST_CHECK_EQUAL(folded.size(), all.size() - 4206);
        BOOST_CHECK_EQUAL(std::count(folded.begin(), folded.end(), uint32_t{' '}), 25);
        for (const uint32_t c : folded) {
            const fext::unicode::Properties p = fext::unicode::GetProperties(c);
            BOOST_CHECK(!p.Has(fext::unicode::DEFAULT_IGNORABLE) && !p.Has(fext::unicode::FORMAT) && (c == ' ' || !p.Has(fext::unicode::WHITE_SPACE)));
        }
    }
}

BOOST_AUTO_TEST_CASE(skeleton_equal_forms)
{
    const auto skeleton = [](const Text& text) { return fext::Skeleton(Bytes(fext::EncodeUtf8(text))); };
    const auto tags = [](const std::string& s) {
        Text t{0x1F3F4};
        for (const char c : s) t.push_back(0xE0000 + c);
        t.push_back(0xE007F);
        return t;
    };
    const std::vector<std::vector<Text>> groups{
        {Ascii("Buy NOW"), Ascii("buy now"), Cat({Ascii("b"), {0x200B}, Ascii("uy n o w")}), Cat({Ascii("BU"), {0x0301}, Ascii("Y NOW")})},
        {Cat({Ascii("hi "), tags("IGNORE")}), Cat({Ascii("hi "), tags("SEND ALL")}), Cat({Ascii("hi "), {0x1F3F4}})},
        {Ascii("scam"), {'s', 'c', 0x200C, 'a', 'm'}, {'s', 0x200E, 'c', 0x200F, 'a', 'm'}, {'s', 0xE0100, 'c', 0xE0101, 'a', 'm'}, {'S', 'C', 0xFE0F, 'A', 'M'},
         {'s', 0x070F, 'c', 0x13430, 'a', 0x110BD, 'm'}, {0x0600, 's', 'c', 0x06DD, 'a', 'm', 0x08E2}},
        {Ascii("pay"), {'p', 0x200E, 'a', 0x200F, 'y'}, {'p', 0xFE0F, 'a', 'y'}},
        {Ascii("x"), Cat({Text(10, 0x2800), Ascii("x")}), {'x', 0x0000, 0x0085, 0xE000, 0xFDD0, 0xFFFC, 0x3000, 0x00A0}},
    };
    for (const auto& group : groups) {
        for (const Text& text : group) BOOST_CHECK_MESSAGE(skeleton(text) == skeleton(group[0]), Show(text));
    }
    BOOST_CHECK_EQUAL(skeleton(Ascii("Buy NOW")), "buynow");
    // Every control, C1 controls and DEL included, is dropped.
    for (uint32_t c = 0; c <= 0x9F; ++c) {
        if (c >= 0x20 && c < 0x7F) continue;
        BOOST_CHECK_MESSAGE(skeleton({'a', c, 'b'}) == "ab", strprintf("U+%04X", c));
    }
    for (const uint32_t c : {0x0600, 0x0605, 0x06DD, 0x070F, 0x0890, 0x0891, 0x08E2, 0x110BD, 0x110CD, 0x13430, 0x1343F, 0xFFF9, 0xFFFB}) {
        BOOST_CHECK_MESSAGE(skeleton({'s', 'c', c, 'a', 'm'}) == "scam", strprintf("U+%04X", c));
    }
    // The display keeps them: only search and the skeleton look past them.
    BOOST_CHECK(!DisplayMap({'s', 0x070F, 'c', 0x13430, 'a', 'm'}, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK(skeleton(Ascii("buy now")) != skeleton(Ascii("buy new")));
    // Look-alike letters of other scripts are not folded.
    BOOST_CHECK(skeleton(Ascii("pay")) != skeleton({'p', 0x0430, 'y'}));
    // At most the first 4,096 bytes count.
    std::vector<unsigned char> long_a(5000, 'a'), long_b(4096, 'a');
    long_b.resize(5000, 'b');
    BOOST_CHECK_EQUAL(fext::Skeleton(long_a), fext::Skeleton(long_b));
    BOOST_CHECK_EQUAL(fext::Skeleton(long_a).size(), 4096U);
    // The skeleton holds no default-ignorable code point, mark, control or space, and is valid UTF-8.
    FastRandomContext rng{uint256S("5e")};
    for (int k = 0; k < 2000; ++k) {
        const std::string sk = fext::Skeleton(Bytes(fext::EncodeUtf8(RandomText(rng))));
        const Text decoded = Decode(Bytes(sk));
        BOOST_CHECK(fext::EncodeUtf8(decoded) == sk);
        for (const uint32_t c : decoded) {
            const fext::unicode::Properties p = fext::unicode::GetProperties(c);
            BOOST_CHECK(!p.Has(fext::unicode::DEFAULT_IGNORABLE) && !p.Has(fext::unicode::FORMAT) && !p.Has(fext::unicode::MARK) && !p.Has(fext::unicode::WHITE_SPACE) && c >= 0x20);
        }
    }
}

BOOST_AUTO_TEST_CASE(cluster_caps)
{
    // The limits, at each limit and one past it.
    for (size_t n : {7, 8, 9, 10, 100}) {
        const Mapped mapped = DisplayMap(Cat({{'a'}, Text(n, 0x0301)}), DisplayMode::MULTI_LINE);
        if (n <= 8) {
            BOOST_CHECK(!mapped.Changed());
        } else {
            BOOST_CHECK(mapped.Text() == Cat({{'a'}, Text(8, 0x0301), {FFFD}}));
            BOOST_CHECK_EQUAL(mapped.Counts().marks, n - 8);
        }
    }
    for (size_t n : {31, 32, 33, 34, 1000}) {
        const Mapped mapped = DisplayMap(Text(n, 0x1100), DisplayMode::MULTI_LINE);
        if (n <= 32) {
            BOOST_CHECK(!mapped.Changed());
        } else {
            BOOST_CHECK(mapped.Text() == Cat({Text(32, 0x1100), {FFFD}}));
            BOOST_CHECK_EQUAL(mapped.Counts().cluster, n - 32);
        }
    }
    for (size_t n : {63, 64, 65, 66, 80}) {
        const Mapped mapped = DisplayMap(Text(n, 0x1F1FA), DisplayMode::MULTI_LINE);
        if (n <= 64) {
            BOOST_CHECK(!mapped.Changed());
        } else {
            BOOST_CHECK(mapped.Text() == Cat({Text(64, 0x1F1FA), {FFFD}}));
            BOOST_CHECK_EQUAL(mapped.Counts().cluster, n - 64);
        }
    }
    // A run of regional indicators ends at any other code point, and the next run keeps its own 64.
    const Mapped runs = DisplayMap(Cat({Text(65, 0x1F1E6), {'a', 0x1F1E6, 0x1F1E7}}), DisplayMode::MULTI_LINE);
    BOOST_CHECK(runs.Text() == Cat({Text(64, 0x1F1E6), {FFFD, 'a', 0x1F1E6, 0x1F1E7}}));
    BOOST_CHECK_EQUAL(runs.Counts().cluster, 1U);
    // A cluster of 32 code points and one U+FFFD is shown as it is and counts nothing.
    const Text capped = Cat({Text(32, 0x0600), {FFFD}});
    BOOST_CHECK(!DisplayMap(capped, DisplayMode::MULTI_LINE).Changed());
    BOOST_CHECK_EQUAL(DisplayMap(capped, DisplayMode::MULTI_LINE).Counts().Total(), 0U);
    // When the ninth mark is the 33rd code point of a cluster, the cut counts as marks.
    const Mapped both = DisplayMap(Cat({{'a'}, Text(23, 0x1F3FD), Text(9, 0x0301)}), DisplayMode::MULTI_LINE);
    BOOST_CHECK_EQUAL(both.Counts().marks, 1U);
    BOOST_CHECK_EQUAL(both.Counts().cluster, 0U);
    // A long cluster loses the joiner at the end of the part it keeps, so that the result maps to itself.
    const Mapped chain = DisplayMap(Cat({Repeat({0x1F469, 0x200D}, 16), {0x1F469}}), DisplayMode::MULTI_LINE);
    BOOST_CHECK(chain.Text() == Cat({Repeat({0x1F469, 0x200D}, 15), {0x1F469, FFFD}}));
    BOOST_CHECK_EQUAL(chain.Counts().cluster, 2U);
    // Marks on both sides of a kept joiner count for one cluster (here a conjunct, rule GB9c); a replaced joiner
    // ends the cluster.
    const Mapped conjunct = DisplayMap(Cat({{0x0915}, Text(4, 0x0301), {0x094D, 0x200D, 0x0937}, Text(4, 0x0301)}), DisplayMode::MULTI_LINE);
    BOOST_CHECK(conjunct.Text() == Cat({{0x0915}, Text(4, 0x0301), {0x094D, 0x200D, 0x0937}, Text(3, 0x0301), {FFFD}}));
    BOOST_CHECK_EQUAL(conjunct.Counts().marks, 1U);
    const Mapped replaced = DisplayMap(Cat({{'a'}, Text(8, 0x0301), {0x200D}, Text(8, 0x0301)}), DisplayMode::MULTI_LINE);
    BOOST_CHECK(replaced.Text() == Cat({{'a'}, Text(8, 0x0301), {FFFD}, Text(8, 0x0301)}));
    BOOST_CHECK_EQUAL(replaced.Counts().marks, 0U);
    // A U+200C kept between two joining letters goes with its marks when the cluster is cut after it.
    const Mapped zwnj = DisplayMap(Cat({{0x0628, 0x200C}, Text(10, 0x0651), {0x062A}}), DisplayMode::MULTI_LINE);
    BOOST_CHECK(zwnj.Text() == (Text{0x0628, FFFD, 0x062A}));
    BOOST_CHECK_EQUAL(zwnj.Counts().marks, 11U);
    // Overlong clusters of each kind and other long runs, each cut to the limits.
    for (const Text& text : {Text(131072, 0x1100), Cat({{'a'}, Text(131072, 0x0903)}), Cat({Repeat({0x1F469, 0x200D}, 40000), {0x1F469}}),
                             Cat({{0x1F44D}, Text(40000, 0x1F3FD)}), Text(100000, 0x1F1E6), Cat({{0x0628}, Repeat({0x0651, 0x200C}, 1000)}),
                             Repeat({0x0915, 0x094D}, 1000), Repeat({0x0915, 0x094D, 0x200D}, 1000), Text(1000, 0x0600)}) {
        const Mapped mapped = DisplayMap(text, DisplayMode::MULTI_LINE);
        CheckMapped(text, mapped);
        // A cluster boundary lies within every 33 code points of the result.
        const std::vector<size_t> breaks = fext::GraphemeBreaks(mapped.Text());
        for (size_t k = 0; k + 1 < breaks.size(); ++k) BOOST_CHECK_LE(breaks[k + 1] - breaks[k], 33U);
    }
}

BOOST_AUTO_TEST_CASE(segmentation)
{
    // Every line of Unicode's GraphemeBreakTest.txt: code points in hex, with U+00F7 where a boundary lies and
    // U+00D7 where none does.
    const std::string file(reinterpret_cast<const char*>(fext_grapheme_break_test_txt), sizeof(fext_grapheme_break_test_txt));
    const std::string divide{"\xc3\xb7"}, multiply{"\xc3\x97"};
    size_t lines = 0, start = 0;
    while (start < file.size()) {
        size_t end = file.find('\n', start);
        if (end == std::string::npos) end = file.size();
        std::string line = file.substr(start, end - start);
        start = end + 1;
        line = line.substr(0, line.find('#'));
        Text text;
        std::vector<size_t> expected;
        std::istringstream words(line);
        std::string word;
        while (words >> word) {
            if (word == divide) {
                expected.push_back(text.size());
            } else if (word == multiply) {
            } else {
                uint32_t cp = 0;
                for (const char c : word) cp = (cp << 4) | HexDigit(c);
                text.push_back(cp);
            }
        }
        if (text.empty()) continue;
        BOOST_CHECK_MESSAGE(fext::GraphemeBreaks(text) == expected, line);
        ++lines;
    }
    BOOST_CHECK_EQUAL(lines, 1187U);
    BOOST_CHECK(fext::GraphemeBreaks({}) == std::vector<size_t>{0});
    // GB9c: Indic conjuncts are one cluster in Unicode 15.1; a ZWNJ ends the conjunct.
    BOOST_CHECK(fext::GraphemeBreaks({0x0915, 0x094D, 0x0937}) == (std::vector<size_t>{0, 3}));
    BOOST_CHECK(fext::GraphemeBreaks({0x0915, 0x094D, 0x200D, 0x0937}) == (std::vector<size_t>{0, 4}));
    BOOST_CHECK(fext::GraphemeBreaks({0x0915, 0x094D, 0x200C, 0x0937}) == (std::vector<size_t>{0, 3, 4}));
    // Values that are not code points break like controls.
    BOOST_CHECK(fext::GraphemeBreaks({'a', 0x110000, 0x0301}) == (std::vector<size_t>{0, 1, 2, 3}));
}

BOOST_AUTO_TEST_CASE(nul_kept)
{
    // Decoding reads every byte: a NUL neither ends the text nor disappears.
    BOOST_CHECK(Decode("610062") == (Text{'a', 0, 'b'}));
    BOOST_CHECK(Decode("0000") == (Text{0, 0}));
    const Mapped mapped = DisplayMap(Decode("00610062" "00"), DisplayMode::ONE_LINE);
    BOOST_CHECK(mapped.Text() == (Text{0x2400, 'a', 0x2400, 'b', 0x2400}));
    BOOST_CHECK_EQUAL(mapped.Counts().control, 3U);
    BOOST_CHECK(Decode(std::vector<unsigned char>(100000, 0)).size() == 100000);
    // Every C0 control but the line feed shows as its control picture; a CR alone too.
    for (uint32_t c = 0; c < 0x20; ++c) {
        const Mapped m = DisplayMap({c, 'x'}, DisplayMode::MULTI_LINE);
        BOOST_CHECK(m.Text() == (c == 0x0A ? Text{0x0A, 'x'} : Text{0x2400 + c, 'x'}));
    }
    // Code points that are not Unicode scalar values (a lone surrogate from a QString) become U+FFFD; encoding
    // writes them as U+FFFD too.
    const Mapped lone = DisplayMap({'a', 0xD800, 'b', 0xDFFF, 0x110000, 0xFFFFFFFF}, DisplayMode::MULTI_LINE);
    BOOST_CHECK(lone.Text() == (Text{'a', FFFD, 'b', FFFD, FFFD, FFFD}));
    BOOST_CHECK_EQUAL(lone.Counts().private_use, 4U);
    BOOST_CHECK_EQUAL(fext::EncodeUtf8({0xD800, 0x110000}), "\xef\xbf\xbd\xef\xbf\xbd");
    BOOST_CHECK(fext::FoldForSearch({0xD800}, true) == Text{0xD800});
}

BOOST_AUTO_TEST_CASE(one_line)
{
    // Line breaks of every form show as the line symbol; no line break is left.
    const Text text{'a', 0x0A, 'b', 0x0D, 0x0A, 'c', 0x2028, 'd', 0x2029, 'e', 0x0D, 'f'};
    const Mapped one = DisplayMap(text, DisplayMode::ONE_LINE);
    BOOST_CHECK(one.Text() == (Text{'a', 0x21B5, 'b', 0x21B5, 'c', 0x21B5, 'd', 0x21B5, 'e', 0x240D, 'f'}));
    const Mapped multi = DisplayMap(text, DisplayMode::MULTI_LINE);
    BOOST_CHECK(multi.Text() == (Text{'a', 0x0A, 'b', 0x0A, 'c', 0x0A, 'd', 0x0A, 'e', 0x240D, 'f'}));
    BOOST_CHECK(one.Changed() && multi.Changed());
    BOOST_CHECK_EQUAL(one.Counts().control, 1U);
    // A text of line breaks only fakes no rows: it is one line of symbols.
    BOOST_CHECK(DisplayMap(Text(1000, 0x0A), DisplayMode::ONE_LINE).Text() == Text(1000, 0x21B5));
    // On one line the limits hold for the text shown, also where a line break meets the context rules and the caps:
    // the line symbol joins a Prepend run before it and the marks or modifiers after it into one cluster.
    const Text prepends = Cat({Text(32, 0x0600), {0x0A}, Text(32, 0x1F3FD)});
    BOOST_CHECK(DisplayMap(prepends, DisplayMode::MULTI_LINE).Text() == prepends);
    const Mapped prepends_one = DisplayMap(prepends, DisplayMode::ONE_LINE);
    BOOST_CHECK(prepends_one.Text() == Cat({Text(32, 0x0600), {FFFD}}));
    BOOST_CHECK_EQUAL(prepends_one.Counts().cluster, 33U);
    BOOST_CHECK(fext::GraphemeBreaks(prepends_one.Text()) == (std::vector<size_t>{0, 33}));
    const Text modifiers = Cat({{0x0A}, Text(32, 0x1F3FD)});
    BOOST_CHECK(DisplayMap(modifiers, DisplayMode::MULTI_LINE).Text() == modifiers);
    BOOST_CHECK(DisplayMap(modifiers, DisplayMode::ONE_LINE).Text() == Cat({{0x21B5}, Text(31, 0x1F3FD), {FFFD}}));
    // The marks of two lines are one run on one line.
    const Text marks = Cat({{0x200F}, Ascii("1\n"), {0x200F}, Ascii("2")});
    BOOST_CHECK(DisplayMap(marks, DisplayMode::MULTI_LINE).Text() == marks);
    BOOST_CHECK(DisplayMap(marks, DisplayMode::ONE_LINE).Text() == Cat({{0x200F}, Ascii("1"), {0x21B5, FFFD}, Ascii("2")}));
    for (const Text& t : {prepends, modifiers, marks, Cat({{0x200E}, Ascii("1\n2"), {0x200E}}), Cat({Ascii("a\n"), Text(40, 0x1F3FD)}),
                          Cat({Ascii("\n"), Text(12, 0x0301)}), Cat({Text(40, 0x0600), Ascii("\r\n"), Text(40, 0x0903)}), Text(100, 0x2028)}) {
        CheckMapped(t, DisplayMap(t, DisplayMode::MULTI_LINE));
    }
}

BOOST_AUTO_TEST_CASE(idempotent)
{
    for (const Sample& sample : Samples()) {
        const Mapped once = DisplayMap(sample.input, DisplayMode::MULTI_LINE);
        BOOST_CHECK_MESSAGE(DisplayMap(once.Text(), DisplayMode::MULTI_LINE).Text() == once.Text(), sample.name);
    }
    // The cases where a context depends on what the caps take away.
    for (const Text& text : {Cat({{0x200E, '1'}, Text(8, 0x0301), {0x0903, 0x200E}}), Text{0x0628, 0x200C, 0xFE00, 0x062A},
                             Cat({{0x0628, 0x200C}, Text(10, 0x0651), {0x062A}}), Cat({Text(65, 0x1F1FA), {0x200E, 'a'}}),
                             Cat({Text(33, 0x0600), {0x200E}}), Cat({{'1', 0x200E}, Text(40, 0x1100), {0x200F}})}) {
        CheckMapped(text, DisplayMap(text, DisplayMode::MULTI_LINE));
    }
    // Structured random text, and random bytes.
    FastRandomContext rng{uint256S("1de")};
    for (int k = 0; k < 20000; ++k) {
        const Text text = RandomText(rng);
        CheckMapped(text, DisplayMap(text, DisplayMode::MULTI_LINE));
    }
    for (int k = 0; k < 5000; ++k) {
        const Text text = Decode(rng.randbytes(rng.randrange(200)));
        CheckMapped(text, DisplayMap(text, DisplayMode::MULTI_LINE));
    }
}

BOOST_AUTO_TEST_CASE(roundtrip)
{
    // Decoding what was encoded gives the text back, for every Unicode scalar value and for random texts.
    Text all;
    for (uint32_t c = 0; c <= fext::unicode::MAX_CODE_POINT; ++c) {
        if (c < 0xD800 || c > 0xDFFF) all.push_back(c);
    }
    BOOST_CHECK(Decode(Bytes(fext::EncodeUtf8(all))) == all);
    FastRandomContext rng{uint256S("70")};
    for (int k = 0; k < 2000; ++k) {
        Text text(rng.randrange(50));
        for (uint32_t& c : text) {
            do {
                c = rng.randrange(0x110000);
            } while (c >= 0xD800 && c <= 0xDFFF);
        }
        BOOST_CHECK(Decode(Bytes(fext::EncodeUtf8(text))) == text);
        // What DisplayMap shows also encodes and decodes to itself.
        const Mapped mapped = DisplayMap(text, DisplayMode::MULTI_LINE);
        BOOST_CHECK(Decode(Bytes(fext::EncodeUtf8(mapped.Text()))) == mapped.Text());
    }
    BOOST_CHECK_EQUAL(HexStr(fext::EncodeUtf8({0x24, 0xA2, 0x20AC, 0x10348})), "24c2a2e282acf0908d88");
}

BOOST_AUTO_TEST_CASE(tables_version)
{
    BOOST_CHECK_EQUAL(std::string(fext::unicode::UNICODE_VERSION), "15.1.0");
    // The files of the Unicode Character Database 15.1.0 the tables were made from.
    const std::vector<std::pair<std::string, std::string>> files{
        {"UnicodeData.txt", "2fc713e6a31a87c4850a37fe2caffa4218180fadb5de86b43a143ddb4581fb86"},
        {"DerivedCoreProperties.txt", "f55d0db69123431a7317868725b1fcbf1eab6b265d756d1bd7f0f6d9f9ee108b"},
        {"PropList.txt", "05672956317b6296bc2ec3d6cef1f6452b57ff4f2efc6dc55b0a19277d5fcfd1"},
        {"CaseFolding.txt", "4e55acfdc32825a22e87670e9056a3bf94ad7c5400065778e9e10f8314372bcf"},
        {"auxiliary/GraphemeBreakProperty.txt", "a7e52eee647e52dc210b8719b4d7037276f4b353810293d69377fc46374cec3f"},
        {"auxiliary/GraphemeBreakTest.txt", "ed9c5e92fd0911ccbeeb63c97cb19c519ea272ff1112ce843abd991582dd848f"},
        {"emoji/emoji-data.txt", "d7aef489c8fe4c14f09ea5695200277c6b93ac82ac60845cdd2161b0d6835cc1"},
        {"emoji/emoji-variation-sequences.txt", "8a67d0429a5831d91e744bc5170d43dc66ddccd42c6f532cbe421dc406b571fd"},
        {"emoji/emoji-sequences.txt", "eb72c9115e3504fbbe1c8621b619f879471a46ccc56e2f445417b7c1cad050d1"},
        {"StandardizedVariants.txt", "e12a2ce1737ed346494a9c76d0fe19a109673e26db8bc0029188a0dde25163cd"},
        {"ArabicShaping.txt", "c289c5fbc632fe3d5f969b5592d5f7fbdbc4b01f0f1370a38b83df5f6b94310c"},
        {"extracted/DerivedJoiningType.txt", "2e0ed3733272299007cf0b76e84a8a653192d99a4429d2232fffcabccfd2d462"},
        {"Scripts.txt", "0eacb65169ae6eb1d399cd70826b3da15fff19f6f586eecf819b70c83b1d9b32"},
        {"extracted/DerivedBidiClass.txt", "b57884c59a3a5348d86faed39965bbddb4e4493d3c16c1ec378ec26bfb5821be"},
    };
    BOOST_REQUIRE_EQUAL(fext::unicode::SOURCE_FILE_COUNT, files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        BOOST_CHECK_EQUAL(fext::unicode::SOURCE_FILES[i].name, files[i].first);
        BOOST_CHECK_EQUAL(fext::unicode::SOURCE_FILES[i].sha256, files[i].second);
    }
    // The data file of the segmentation test is that file.
    BOOST_CHECK_EQUAL(sizeof(fext_grapheme_break_test_txt), 188211U);

    // Ranges and code points of each property, read back through the lookup of every code point; the numbers are
    // those of the Unicode Character Database 15.1.0.
    using fext::unicode::GraphemeBreak;
    using fext::unicode::Properties;
    struct Count {
        const char* name;
        std::function<bool(uint32_t, Properties)> has;
        size_t ranges, code_points;
    };
    const auto grapheme = [](GraphemeBreak g) { return [g](uint32_t, Properties p) { return p.Grapheme() == g; }; };
    const auto bit = [](fext::unicode::PropertyBit b) { return [b](uint32_t, Properties p) { return p.Has(b); }; };
    const std::vector<Count> counts{
        {"Control", grapheme(GraphemeBreak::CONTROL), 19, 3893},
        {"Extend", grapheme(GraphemeBreak::EXTEND), 364, 2130},
        {"Prepend", grapheme(GraphemeBreak::PREPEND), 15, 27},
        {"SpacingMark", grapheme(GraphemeBreak::SPACING_MARK), 165, 395},
        {"LV", grapheme(GraphemeBreak::LV), 399, 399},
        {"LVT", grapheme(GraphemeBreak::LVT), 399, 10773},
        {"Regional_Indicator", grapheme(GraphemeBreak::REGIONAL_INDICATOR), 1, 26},
        {"InCB=Linker", [](uint32_t, Properties p) { return p.IndicConjunct() == fext::unicode::IndicConjunctBreak::LINKER; }, 6, 6},
        {"InCB=Consonant", [](uint32_t, Properties p) { return p.IndicConjunct() == fext::unicode::IndicConjunctBreak::CONSONANT; }, 26, 240},
        {"InCB=Extend", [](uint32_t, Properties p) { return p.IndicConjunct() == fext::unicode::IndicConjunctBreak::EXTEND; }, 169, 884},
        {"Joining_Type other than U", [](uint32_t, Properties p) { return p.Joining() != fext::unicode::JoiningType::NON_JOINING; }, 381, 2924},
        {"Extended_Pictographic", bit(fext::unicode::EXTENDED_PICTOGRAPHIC), 78, 3537},
        {"Emoji_Modifier", bit(fext::unicode::EMOJI_MODIFIER), 1, 5},
        {"Default_Ignorable_Code_Point", bit(fext::unicode::DEFAULT_IGNORABLE), 17, 4174},
        {"White_Space", bit(fext::unicode::WHITE_SPACE), 10, 25},
        {"strong bidi", bit(fext::unicode::STRONG), 526, 1101683},
        {"letters", bit(fext::unicode::LETTER), 660, 136726},
        {"marks", bit(fext::unicode::MARK), 310, 2450},
        {"viramas", bit(fext::unicode::VIRAMA), 56, 65},
        {"Unified_Ideograph", bit(fext::unicode::UNIFIED_IDEOGRAPH), 17, 97680},
        {"Mongolian letters", bit(fext::unicode::MONGOLIAN_LETTER), 5, 128},
        {"emoji variation bases", bit(fext::unicode::EMOJI_VARIATION_BASE), 183, 371},
        {"format characters (Cf)", bit(fext::unicode::FORMAT), 21, 170},
    };
    for (const Count& count : counts) {
        size_t ranges = 0, code_points = 0;
        bool inside = false;
        for (uint32_t c = 0; c <= fext::unicode::MAX_CODE_POINT; ++c) {
            const bool has = count.has(c, fext::unicode::GetProperties(c));
            if (has && !inside) ++ranges;
            if (has) ++code_points;
            inside = has;
        }
        BOOST_CHECK_MESSAGE(ranges == count.ranges && code_points == count.code_points,
                            strprintf("%s: %u ranges, %u code points", count.name, ranges, code_points));
    }
    // A few code points by hand.
    BOOST_CHECK(fext::unicode::GetProperties(0xAC00).Grapheme() == GraphemeBreak::LV);
    BOOST_CHECK(fext::unicode::GetProperties(0xAC01).Grapheme() == GraphemeBreak::LVT);
    BOOST_CHECK(fext::unicode::GetProperties(0xAC1C).Grapheme() == GraphemeBreak::LV);
    BOOST_CHECK(fext::unicode::GetProperties(0xD7A3).Grapheme() == GraphemeBreak::LVT);
    BOOST_CHECK(fext::unicode::GetProperties(0xD7A4).Grapheme() == GraphemeBreak::OTHER);
    BOOST_CHECK(fext::unicode::GetProperties(0x200D).Grapheme() == GraphemeBreak::ZWJ);
    BOOST_CHECK(fext::unicode::GetProperties(0x0D).Grapheme() == GraphemeBreak::CR);
    BOOST_CHECK(fext::unicode::GetProperties(0x0A).Grapheme() == GraphemeBreak::LF);
    BOOST_CHECK(fext::unicode::GetProperties(0xD800).Grapheme() == GraphemeBreak::OTHER);
    BOOST_CHECK(fext::unicode::GetProperties(0x110000).Grapheme() == GraphemeBreak::CONTROL);
    BOOST_CHECK(fext::unicode::GetProperties(0x0628).Joining() == fext::unicode::JoiningType::DUAL_JOINING);
    BOOST_CHECK(fext::unicode::GetProperties(0x062F).Joining() == fext::unicode::JoiningType::RIGHT_JOINING);
    BOOST_CHECK(fext::unicode::GetProperties(0x0640).Joining() == fext::unicode::JoiningType::JOIN_CAUSING);
    BOOST_CHECK(fext::unicode::GetProperties(0x064B).Joining() == fext::unicode::JoiningType::TRANSPARENT);
    BOOST_CHECK(fext::unicode::GetProperties(0x094D).Has(fext::unicode::VIRAMA));
    BOOST_CHECK(fext::unicode::GetProperties(0x05D0).Has(fext::unicode::STRONG));
    BOOST_CHECK(!fext::unicode::GetProperties('1').Has(fext::unicode::STRONG));
    BOOST_CHECK(fext::unicode::GetProperties(0x2764).Has(fext::unicode::EMOJI_VARIATION_BASE));
    BOOST_CHECK(fext::unicode::IsStandardizedVariant(0x2229, 0xFE00));
    BOOST_CHECK(!fext::unicode::IsStandardizedVariant(0x2229, 0xFE01));
    BOOST_CHECK_EQUAL(fext::unicode::SimpleCaseFold('A'), uint32_t{'a'});
    BOOST_CHECK_EQUAL(fext::unicode::SimpleCaseFold(0x1E9E), 0x00DFU);
    BOOST_CHECK_EQUAL(fext::unicode::SimpleCaseFold(0x1FD3), 0x0390U);
    BOOST_CHECK_EQUAL(fext::unicode::SimpleCaseFold(0x10FFFF), 0x10FFFFU);
}

BOOST_AUTO_TEST_CASE(linear_time_bounded_memory)
{
    // The largest message, 998,000 bytes, of the costliest kinds of text: each is decoded and mapped in time and
    // memory proportional to its length (the bound below is far above what linear work takes, and far below what
    // work growing with the square of the length would), and the result is never longer than the text.
    const size_t size = 998000;
    const auto fill = [&](const Text& unit, const Text& head = {}) {
        std::string bytes = fext::EncodeUtf8(head);
        const std::string piece = fext::EncodeUtf8(unit);
        while (bytes.size() + piece.size() <= size) bytes += piece;
        return std::vector<unsigned char>(bytes.begin(), bytes.end());
    };
    const std::vector<std::vector<unsigned char>> inputs{
        std::vector<unsigned char>(size, 'a'),
        std::vector<unsigned char>(size, 0xFF),
        std::vector<unsigned char>(size, 0x0A),
        fill({0x4E16}),
        fill({0x1100}),
        fill({0x0903}, {'a'}),
        fill({0x1F469, 0x200D}),
        fill({0x1F3FD}, {0x1F44D}),
        fill({0x1F1E6}),
        fill({0xE0041}, {0x1F3F4}),
        fill({'1', 0x200E}),
        fill({0x064B, 0x200C}, {0x0628}),
        fill({0x0628, 0x064B, 0x064B, 0x200C}),
        fill({'a', 0xFE0F, 0x200D}),
        fill({0x0915, 0x094D}),
    };
    for (const auto& bytes : inputs) {
        const auto begin = std::chrono::steady_clock::now();
        Text text;
        fext::DecodeUtf8(bytes, text);
        const Mapped multi = DisplayMap(text, DisplayMode::MULTI_LINE);
        const Mapped one = DisplayMap(text, DisplayMode::ONE_LINE);
        const std::vector<size_t> breaks = fext::GraphemeBreaks(multi.Text());
        const Text folded = fext::FoldForSearch(text, true);
        const std::string skeleton = fext::Skeleton(bytes);
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        BOOST_CHECK_LE(text.size(), bytes.size());
        BOOST_CHECK_LE(multi.Text().size(), text.size());
        BOOST_CHECK_LE(multi.Text().capacity(), text.size());
        BOOST_CHECK_EQUAL(one.Text().size(), multi.Text().size());
        BOOST_CHECK_LE(folded.size(), text.size());
        // A byte that starts no sequence becomes U+FFFD, three bytes in UTF-8.
        BOOST_CHECK_LE(skeleton.size(), 3 * fext::SKELETON_MAX_BYTES);
        BOOST_CHECK_LE(breaks.size(), multi.Text().size() + 1);
        BOOST_CHECK_MESSAGE(elapsed < std::chrono::seconds{5}, strprintf("%s...: %d ms", HexStr(Span<const unsigned char>{bytes}.first(8)),
                                                                          std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
    }
}

BOOST_AUTO_TEST_SUITE_END()
