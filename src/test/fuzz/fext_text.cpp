// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fext/text.h>
#include <fext/unicode_data.h>
#include <test/fuzz/fuzz.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

using namespace fext;

namespace {

bool IsVariationSelector(uint32_t c)
{
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180D) || c == 0x180F;
}
bool IsTag(uint32_t c) { return c >= 0xE0020 && c <= 0xE007F; }
bool IsDirectionMark(uint32_t c) { return c == 0x200E || c == 0x200F || c == 0x061C; }
bool IsContextFormat(uint32_t c) { return c == 0x200C || c == 0x200D || IsDirectionMark(c) || IsVariationSelector(c) || IsTag(c); }
bool Has(uint32_t c, unicode::PropertyBit bit) { return unicode::GetProperties(c).Has(bit); }

//! Whether bytes are well-formed UTF-8, read by the table of well-formed byte sequences of the Unicode standard.
bool WellFormed(const std::vector<uint8_t>& bytes)
{
    struct Form {
        uint8_t first_lo, first_hi, second_lo, second_hi;
        size_t more;
    };
    static constexpr Form FORMS[]{{0x00, 0x7f, 0x00, 0x00, 0}, {0xc2, 0xdf, 0x80, 0xbf, 1}, {0xe0, 0xe0, 0xa0, 0xbf, 2},
                                  {0xe1, 0xec, 0x80, 0xbf, 2}, {0xed, 0xed, 0x80, 0x9f, 2}, {0xee, 0xef, 0x80, 0xbf, 2},
                                  {0xf0, 0xf0, 0x90, 0xbf, 3}, {0xf1, 0xf3, 0x80, 0xbf, 3}, {0xf4, 0xf4, 0x80, 0x8f, 3}};
    for (size_t i = 0; i < bytes.size();) {
        const Form* form = std::find_if(std::begin(FORMS), std::end(FORMS), [&](const Form& f) { return bytes[i] >= f.first_lo && bytes[i] <= f.first_hi; });
        if (form == std::end(FORMS) || bytes.size() - i <= form->more) return false;
        for (size_t j = 1; j <= form->more; ++j) {
            if (bytes[i + j] < (j == 1 ? form->second_lo : 0x80) || bytes[i + j] > (j == 1 ? form->second_hi : 0xbf)) return false;
        }
        i += form->more + 1;
    }
    return true;
}

bool CountsAsMark(uint32_t c)
{
    const unicode::Properties p = unicode::GetProperties(c);
    return (p.Grapheme() == unicode::GraphemeBreak::EXTEND || p.Grapheme() == unicode::GraphemeBreak::SPACING_MARK) && c != 0x200C && c != 0x200D &&
           !IsVariationSelector(c) && !p.Has(unicode::EMOJI_MODIFIER) && !IsTag(c);
}

//! The checks of shown text that need no context: the replaced classes are gone, and each format character that
//! was kept stands where its rule allows it.
void CheckShown(const std::vector<uint32_t>& out)
{
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t c = out[i];
        assert(c <= unicode::MAX_CODE_POINT && !(c >= 0xD800 && c <= 0xDFFF));
        assert(!(c < 0x20 && c != LINE_FEED) && !(c >= 0x7F && c <= 0x9F));
        assert(!((c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069)));
        assert(!Has(c, unicode::DEFAULT_IGNORABLE) || IsContextFormat(c));
        assert(c != 0x2800 && !(c >= 0xFFF9 && c <= 0xFFFC));
        assert(!((c >= 0xE000 && c <= 0xF8FF) || c >= 0xF0000 || (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE));
        const uint32_t before = i > 0 ? out[i - 1] : 0;
        const bool has_before = i > 0;
        const bool has_after = i + 1 < out.size();
        const uint32_t after = has_after ? out[i + 1] : 0;
        if (IsVariationSelector(c)) {
            // Directly after a base that is not a format character or U+FFFD.
            assert(has_before && !IsContextFormat(before) && before != REPLACEMENT_CHARACTER);
            if (c == 0xFE0E || c == 0xFE0F) assert(Has(before, unicode::EMOJI_VARIATION_BASE));
            else if (c <= 0xFE0D && c >= 0xFE00) assert(unicode::IsStandardizedVariant(before, c));
            else if (c >= 0xE0100) assert(Has(before, unicode::UNIFIED_IDEOGRAPH));
            else assert(Has(before, unicode::MONGOLIAN_LETTER));
        } else if (c == 0x200C || c == 0x200D) {
            // After a virama, an emoji or (U+200C) a joining letter or one of its marks, and never at the end.
            assert(has_before && has_after);
            const unicode::Properties p = unicode::GetProperties(before);
            assert(p.Has(unicode::VIRAMA) || p.Has(unicode::EXTENDED_PICTOGRAPHIC) || p.Has(unicode::EMOJI_MODIFIER) || before == 0xFE0F ||
                   (c == 0x200C && p.Joining() != unicode::JoiningType::NON_JOINING));
        } else if (IsDirectionMark(c)) {
            // Never after another mark, and never between two strong characters.
            assert(!(has_before && IsDirectionMark(before)));
            assert(!(has_before && has_after && Has(before, unicode::STRONG) && Has(after, unicode::STRONG) && after != LINE_FEED));
        } else if (IsTag(c)) {
            // Only inside the three flags: U+1F3F4, five tag letters, U+E007F.
            size_t start = i;
            while (start > 0 && IsTag(out[start - 1])) --start;
            assert(start > 0 && out[start - 1] == 0x1F3F4);
        }
    }
}

//! The limits of shown text: every grapheme cluster has at most 32 code points before the U+FFFD that replaced the
//! rest of a longer one, and at most 8 marks; no run of regional indicators is longer than 64.
void CheckLimits(const std::vector<uint32_t>& shown)
{
    const std::vector<size_t> breaks = GraphemeBreaks(shown);
    assert(breaks.front() == 0 && breaks.back() == shown.size());
    for (size_t k = 0; k + 1 < breaks.size(); ++k) {
        const size_t size = breaks[k + 1] - breaks[k];
        assert(size > 0);
        assert(size <= MAX_CLUSTER_CODE_POINTS || (size == MAX_CLUSTER_CODE_POINTS + 1 && shown[breaks[k + 1] - 1] == REPLACEMENT_CHARACTER));
        assert(static_cast<size_t>(std::count_if(shown.begin() + breaks[k], shown.begin() + breaks[k + 1], CountsAsMark)) <= MAX_CLUSTER_MARKS);
    }
    size_t run = 0;
    for (const uint32_t c : shown) {
        run = c >= 0x1F1E6 && c <= 0x1F1FF ? run + 1 : 0;
        assert(run <= MAX_REGIONAL_INDICATORS);
    }
}

//! The text with every line break (CR LF, LF, U+2028, U+2029) replaced by the line symbol.
std::vector<uint32_t> LineSymbols(const std::vector<uint32_t>& text)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == 0x0D && i + 1 < text.size() && text[i + 1] == LINE_FEED) continue;
        out.push_back(text[i] == LINE_FEED || text[i] == 0x2028 || text[i] == 0x2029 ? LINE_SYMBOL : text[i]);
    }
    return out;
}

//! Both modes of the display of any code points: the result is no longer than the text, passes the checks of shown
//! text and keeps the limits, maps to itself, and on one line is the text with line symbols as shown on several.
void CheckDisplay(const std::vector<uint32_t>& text)
{
    const Mapped multi = DisplayMap(text, DisplayMode::MULTI_LINE);
    const std::vector<uint32_t>& shown = multi.Text();
    assert(multi.Mode() == DisplayMode::MULTI_LINE);
    assert(shown.size() <= text.size());
    assert(multi.Changed() == (shown != text));
    assert(multi.Changed() || multi.Counts().Total() == 0);
    CheckShown(shown);
    CheckLimits(shown);
    const Mapped twice = DisplayMap(shown, DisplayMode::MULTI_LINE);
    assert(twice.Text() == shown && !twice.Changed() && twice.Counts().Total() == 0);

    const Mapped one = DisplayMap(text, DisplayMode::ONE_LINE);
    const std::vector<uint32_t>& line = one.Text();
    assert(one.Mode() == DisplayMode::ONE_LINE);
    assert(line.size() <= text.size());
    assert(line == DisplayMap(LineSymbols(text), DisplayMode::MULTI_LINE).Text());
    assert(std::none_of(line.begin(), line.end(), [](uint32_t c) { return c == LINE_FEED || c == 0x0D || c == 0x2028 || c == 0x2029; }));
    std::vector<uint32_t> lf_symbols = text;
    std::replace(lf_symbols.begin(), lf_symbols.end(), LINE_FEED, LINE_SYMBOL);
    assert(one.Changed() == (line != lf_symbols));
    assert(one.Changed() || one.Counts().Total() == 0);
    CheckShown(line);
    CheckLimits(line);
    const Mapped one_twice = DisplayMap(line, DisplayMode::ONE_LINE);
    assert(one_twice.Text() == line && !one_twice.Changed() && one_twice.Counts().Total() == 0);
}

//! The folded text holds no default-ignorable code point, no format character and no white space but U+0020.
void CheckFolded(const std::vector<uint32_t>& text)
{
    for (const bool case_insensitive : {false, true}) {
        const std::vector<uint32_t> folded = FoldForSearch(text, case_insensitive);
        assert(folded.size() <= text.size());
        for (const uint32_t c : folded) assert(!Has(c, unicode::DEFAULT_IGNORABLE) && !Has(c, unicode::FORMAT) && (c == ' ' || !Has(c, unicode::WHITE_SPACE)));
    }
}

} // namespace

void test_one_input(const std::vector<uint8_t>& buffer)
{
    // Decoding: every byte is read, ill-formed input gives U+FFFD, well-formed input gives itself back.
    std::vector<uint32_t> text;
    std::vector<uint32_t> offsets;
    DecodeUtf8(buffer, text, &offsets);
    assert(text.size() == offsets.size() && text.size() <= buffer.size());
    for (size_t i = 0; i < offsets.size(); ++i) {
        assert(offsets[i] < buffer.size() && (i == 0 ? offsets[i] == 0 : offsets[i] > offsets[i - 1]));
    }
    const std::string encoded = EncodeUtf8(text);
    const std::vector<uint8_t> encoded_bytes(encoded.begin(), encoded.end());
    if (WellFormed(buffer)) assert(encoded_bytes == buffer);
    std::vector<uint32_t> again;
    DecodeUtf8(encoded_bytes, again);
    assert(again == text);

    CheckDisplay(text);
    CheckFolded(text);

    // The input as code points too, every 32-bit value included (the GUI may pass any QString, and a caller any
    // vector): the display, the segmentation, the folding and the encoding take them all.
    std::vector<uint32_t> raw;
    for (size_t i = 0; i + 4 <= buffer.size(); i += 4) {
        raw.push_back(uint32_t(buffer[i]) | uint32_t(buffer[i + 1]) << 8 | uint32_t(buffer[i + 2]) << 16 | uint32_t(buffer[i + 3]) << 24);
    }
    CheckDisplay(raw);
    CheckFolded(raw);
    const std::vector<size_t> raw_breaks = GraphemeBreaks(raw);
    assert(raw_breaks.front() == 0 && raw_breaks.back() == raw.size());
    for (size_t k = 0; k + 1 < raw_breaks.size(); ++k) assert(raw_breaks[k] < raw_breaks[k + 1]);
    const std::string raw_encoded = EncodeUtf8(raw);
    std::vector<uint32_t> raw_decoded;
    DecodeUtf8(Span<const unsigned char>{reinterpret_cast<const unsigned char*>(raw_encoded.data()), raw_encoded.size()}, raw_decoded);
    assert(raw_decoded.size() == raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        const bool scalar = raw[i] <= unicode::MAX_CODE_POINT && !(raw[i] >= 0xD800 && raw[i] <= 0xDFFF);
        assert(raw_decoded[i] == (scalar ? raw[i] : REPLACEMENT_CHARACTER));
    }

    // The skeleton holds no default-ignorable code point, format character, space, mark or control.
    const std::string skeleton = Skeleton(buffer);
    std::vector<uint32_t> skeleton_text;
    DecodeUtf8(Span<const unsigned char>{reinterpret_cast<const unsigned char*>(skeleton.data()), skeleton.size()}, skeleton_text);
    assert(EncodeUtf8(skeleton_text) == skeleton);
    for (const uint32_t c : skeleton_text) {
        assert(!Has(c, unicode::DEFAULT_IGNORABLE) && !Has(c, unicode::FORMAT) && !Has(c, unicode::WHITE_SPACE) && !Has(c, unicode::MARK) && c >= 0x20);
    }
}
