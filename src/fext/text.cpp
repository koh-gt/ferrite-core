// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <fext/text.h>

#include <fext/unicode_data.h>

#include <algorithm>

namespace fext {

namespace {

using unicode::GetProperties;
using unicode::GraphemeBreak;
using unicode::IndicConjunctBreak;
using unicode::JoiningType;
using unicode::Properties;

constexpr uint32_t CR{0x000D};
constexpr uint32_t SPACE{0x0020};
constexpr uint32_t DEL{0x007F};
constexpr uint32_t CONTROL_PICTURES{0x2400};
constexpr uint32_t DEL_PICTURE{0x2421};
constexpr uint32_t LINE_SEPARATOR{0x2028};
constexpr uint32_t PARAGRAPH_SEPARATOR{0x2029};
constexpr uint32_t ZWNJ{0x200C};
constexpr uint32_t ZWJ{0x200D};
constexpr uint32_t LRM{0x200E};
constexpr uint32_t RLM{0x200F};
constexpr uint32_t ALM{0x061C};
constexpr uint32_t VS15{0xFE0E};
constexpr uint32_t VS16{0xFE0F};
constexpr uint32_t BRAILLE_BLANK{0x2800};
constexpr uint32_t BLACK_FLAG{0x1F3F4};
constexpr uint32_t TAG_FIRST{0xE0020};
constexpr uint32_t CANCEL_TAG{0xE007F};

//! The tag letters of the three emoji tag sequences recommended for general interchange, the flags of England,
//! Scotland and Wales (the only ones in emoji-sequences.txt, as contrib/devtools/gen-fext-unicode.py checks).
constexpr uint32_t RGI_TAG_FLAGS[3][5]{
    {0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067},
    {0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074},
    {0xE0067, 0xE0062, 0xE0077, 0xE006C, 0xE0073},
};

bool IsScalarValue(uint32_t c) { return c <= unicode::MAX_CODE_POINT && (c < 0xD800 || c > 0xDFFF); }
bool IsDirectionMark(uint32_t c) { return c == LRM || c == RLM || c == ALM; }
bool IsDirectionControl(uint32_t c) { return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069); }
bool IsVariationSelector(uint32_t c)
{
    return (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0100 && c <= 0xE01EF) || (c >= 0x180B && c <= 0x180D) || c == 0x180F;
}
bool IsTag(uint32_t c) { return c >= TAG_FIRST && c <= CANCEL_TAG; }
//! The format characters that are kept only in the sequences that need them (pass 2).
bool IsContextFormat(uint32_t c) { return c == ZWNJ || c == ZWJ || IsDirectionMark(c) || IsVariationSelector(c) || IsTag(c); }
bool IsPrivateUse(uint32_t c) { return (c >= 0xE000 && c <= 0xF8FF) || (c >= 0xF0000 && c <= 0xFFFFD) || (c >= 0x100000 && c <= 0x10FFFD); }
bool IsNoncharacter(uint32_t c) { return (c >= 0xFDD0 && c <= 0xFDEF) || (c & 0xFFFE) == 0xFFFE; }

//! Whether a code point counts against the limit of combining marks in a grapheme cluster.
bool IsCountedMark(uint32_t c, Properties p)
{
    const GraphemeBreak g = p.Grapheme();
    return (g == GraphemeBreak::EXTEND || g == GraphemeBreak::SPACING_MARK) && c != ZWNJ && c != ZWJ && !IsVariationSelector(c) &&
           !p.Has(unicode::EMOJI_MODIFIER) && !IsTag(c);
}

//! Whether a line break starts at text[i] (CR LF counts as one, starting at the CR).
bool IsLineBreakAt(const std::vector<uint32_t>& text, size_t i)
{
    const uint32_t c = text[i];
    return c == LINE_FEED || c == LINE_SEPARATOR || c == PARAGRAPH_SEPARATOR || (c == CR && i + 1 < text.size() && text[i + 1] == LINE_FEED);
}

enum class Note { NONE, CONTROL, DIRECTION, INVISIBLE, PRIVATE_USE };

/**
 * Pass 1 for a code point that does not start a line break: whether its class alone replaces it, and with what. A
 * format character of pass 2 is not replaced here.
 */
bool ReplaceByClass(uint32_t c, uint32_t& replacement, Note& note)
{
    if (c < 0x20) {
        replacement = CONTROL_PICTURES + c;
        note = Note::CONTROL;
    } else if (c == DEL) {
        replacement = DEL_PICTURE;
        note = Note::CONTROL;
    } else if (c >= 0x80 && c <= 0x9F) {
        replacement = REPLACEMENT_CHARACTER;
        note = Note::CONTROL;
    } else if (IsDirectionControl(c)) {
        replacement = REPLACEMENT_CHARACTER;
        note = Note::DIRECTION;
    } else if (IsContextFormat(c)) {
        return false;
    } else if (!IsScalarValue(c)) {
        replacement = REPLACEMENT_CHARACTER;
        note = Note::PRIVATE_USE;
    } else if (GetProperties(c).Has(unicode::DEFAULT_IGNORABLE) || c == BRAILLE_BLANK || (c >= 0xFFF9 && c <= 0xFFFC)) {
        replacement = REPLACEMENT_CHARACTER;
        note = Note::INVISIBLE;
    } else if (IsPrivateUse(c) || IsNoncharacter(c)) {
        replacement = REPLACEMENT_CHARACTER;
        note = Note::PRIVATE_USE;
    } else {
        return false;
    }
    return true;
}

//! What pass 1 makes of text[i]: a line feed for a line break, the replacement of its class, or itself.
uint32_t ClassValueAt(const std::vector<uint32_t>& text, size_t i)
{
    if (IsLineBreakAt(text, i)) return LINE_FEED;
    uint32_t replacement;
    Note note;
    return ReplaceByClass(text[i], replacement, note) ? replacement : text[i];
}

/** Finds the extended grapheme cluster boundaries of UAX #29 in one pass, given one code point at a time. */
class Segmenter
{
public:
    //! Whether a cluster boundary lies before the next code point, which has properties p.
    bool Push(Properties p)
    {
        const GraphemeBreak g = p.Grapheme();
        const IndicConjunctBreak incb = p.IndicConjunct();
        const bool pictographic = p.Has(unicode::EXTENDED_PICTOGRAPHIC);
        bool boundary;
        if (m_first) {
            boundary = true; // GB1
        } else if (m_prev == GraphemeBreak::CR && g == GraphemeBreak::LF) {
            boundary = false; // GB3
        } else if (m_prev == GraphemeBreak::CONTROL || m_prev == GraphemeBreak::CR || m_prev == GraphemeBreak::LF) {
            boundary = true; // GB4
        } else if (g == GraphemeBreak::CONTROL || g == GraphemeBreak::CR || g == GraphemeBreak::LF) {
            boundary = true; // GB5
        } else if (m_prev == GraphemeBreak::L && (g == GraphemeBreak::L || g == GraphemeBreak::V || g == GraphemeBreak::LV || g == GraphemeBreak::LVT)) {
            boundary = false; // GB6
        } else if ((m_prev == GraphemeBreak::LV || m_prev == GraphemeBreak::V) && (g == GraphemeBreak::V || g == GraphemeBreak::T)) {
            boundary = false; // GB7
        } else if ((m_prev == GraphemeBreak::LVT || m_prev == GraphemeBreak::T) && g == GraphemeBreak::T) {
            boundary = false; // GB8
        } else if (g == GraphemeBreak::EXTEND || g == GraphemeBreak::ZWJ) {
            boundary = false; // GB9
        } else if (g == GraphemeBreak::SPACING_MARK) {
            boundary = false; // GB9a
        } else if (m_prev == GraphemeBreak::PREPEND) {
            boundary = false; // GB9b
        } else if (incb == IndicConjunctBreak::CONSONANT && m_conjunct == Conjunct::LINKED) {
            boundary = false; // GB9c
        } else if (m_prev == GraphemeBreak::ZWJ && m_emoji == Emoji::PICTOGRAPHIC_ZWJ && pictographic) {
            boundary = false; // GB11
        } else if (m_prev == GraphemeBreak::REGIONAL_INDICATOR && g == GraphemeBreak::REGIONAL_INDICATOR && m_regional_indicators % 2 == 1) {
            boundary = false; // GB12, GB13
        } else {
            boundary = true; // GB999
        }

        // GB11 looks back over Extended_Pictographic Extend* ZWJ, GB9c over
        // Consonant [Extend Linker]* Linker [Extend Linker]*, GB12 and GB13 at the number of regional indicators.
        if (pictographic) {
            m_emoji = Emoji::PICTOGRAPHIC;
        } else if (m_emoji == Emoji::PICTOGRAPHIC && g == GraphemeBreak::EXTEND) {
            m_emoji = Emoji::PICTOGRAPHIC;
        } else if (m_emoji == Emoji::PICTOGRAPHIC && g == GraphemeBreak::ZWJ) {
            m_emoji = Emoji::PICTOGRAPHIC_ZWJ;
        } else {
            m_emoji = Emoji::NONE;
        }
        if (incb == IndicConjunctBreak::CONSONANT) {
            m_conjunct = Conjunct::CONSONANT;
        } else if (m_conjunct != Conjunct::NONE && incb == IndicConjunctBreak::LINKER) {
            m_conjunct = Conjunct::LINKED;
        } else if (m_conjunct == Conjunct::NONE || incb != IndicConjunctBreak::EXTEND) {
            m_conjunct = Conjunct::NONE;
        }
        m_regional_indicators = g == GraphemeBreak::REGIONAL_INDICATOR ? m_regional_indicators + 1 : 0;
        m_prev = g;
        m_first = false;
        return boundary;
    }

private:
    enum class Emoji { NONE, PICTOGRAPHIC, PICTOGRAPHIC_ZWJ };
    enum class Conjunct { NONE, CONSONANT, LINKED };

    bool m_first{true};
    GraphemeBreak m_prev{GraphemeBreak::OTHER};
    Emoji m_emoji{Emoji::NONE};
    Conjunct m_conjunct{Conjunct::NONE};
    size_t m_regional_indicators{0};
};

void Count(DisplayCounts& counts, Note note, size_t n = 1)
{
    switch (note) {
    case Note::CONTROL: counts.control += n; break;
    case Note::DIRECTION: counts.direction += n; break;
    case Note::INVISIBLE: counts.invisible += n; break;
    case Note::PRIVATE_USE: counts.private_use += n; break;
    case Note::NONE: break;
    }
}

/** The text after passes 1 and 2, with what pass 3 needs to know of each code point. */
class Draft
{
public:
    std::vector<uint32_t> text;
    std::vector<Properties> properties;
    //! Whether a grapheme cluster starts at each code point.
    std::vector<bool> starts;
    //! The positions of the U+200C kept between two joining letters.
    std::vector<size_t> joining_zwnj;

    explicit Draft(size_t size)
    {
        text.reserve(size);
        properties.reserve(size);
        starts.reserve(size);
    }

    //! Appends a code point; returns whether a grapheme cluster starts with it, or false when it was left out
    //! as part of an overlong run of regional indicators.
    bool Append(uint32_t c, DisplayCounts& counts)
    {
        const Properties p = GetProperties(c);
        if (p.Grapheme() == GraphemeBreak::REGIONAL_INDICATOR) {
            if (m_dropping_regional_indicators) {
                ++counts.cluster;
                return false;
            }
            if (m_regional_indicators == MAX_REGIONAL_INDICATORS) {
                // The rest of the run becomes one U+FFFD.
                m_dropping_regional_indicators = true;
                ++counts.cluster;
                Add(REPLACEMENT_CHARACTER, GetProperties(REPLACEMENT_CHARACTER));
                m_regional_indicators = 0;
                return false;
            }
        } else {
            m_dropping_regional_indicators = false;
        }
        const bool start = Add(c, p);
        m_regional_indicators = p.Grapheme() == GraphemeBreak::REGIONAL_INDICATOR ? m_regional_indicators + 1 : 0;
        return start;
    }

    bool Empty() const { return text.empty(); }
    uint32_t Last() const { return text.back(); }
    Properties LastProperties() const { return properties.back(); }

private:
    bool Add(uint32_t c, Properties p)
    {
        const bool start = m_segmenter.Push(p);
        text.push_back(c);
        properties.push_back(p);
        starts.push_back(start);
        return start;
    }

    Segmenter m_segmenter;
    size_t m_regional_indicators{0};
    bool m_dropping_regional_indicators{false};
};

//! Whether text[pos] starts a tag sequence that is one of the three RGI flags; if so, its length.
size_t RgiFlagLength(const std::vector<uint32_t>& text, size_t pos)
{
    constexpr size_t letters{5};
    if (text.size() - pos < letters + 2 || text[pos + letters + 1] != CANCEL_TAG) return 0;
    for (const auto& flag : RGI_TAG_FLAGS) {
        if (std::equal(flag, flag + letters, text.begin() + pos + 1)) return letters + 2;
    }
    return 0;
}

//! Whether a variation selector is kept after `base`, the last code point shown so far.
bool KeepVariationSelector(uint32_t selector, const Draft& draft)
{
    if (draft.Empty()) return false;
    const uint32_t base = draft.Last();
    if (IsContextFormat(base) || base == REPLACEMENT_CHARACTER || !IsScalarValue(base)) return false;
    const Properties p = draft.LastProperties();
    if (selector == VS15 || selector == VS16) return p.Has(unicode::EMOJI_VARIATION_BASE);
    if (selector >= 0xFE00 && selector <= 0xFE0D) return unicode::IsStandardizedVariant(base, selector);
    if (selector >= 0xE0100) return p.Has(unicode::UNIFIED_IDEOGRAPH);
    return p.Has(unicode::MONGOLIAN_LETTER);
}

//! Whether the joining context of U+200C may look past a code point: a transparent combining mark of its own
//! grapheme cluster, which is never a format character of pass 2.
bool IsTransparentMark(uint32_t c, Properties p)
{
    return p.Joining() == JoiningType::TRANSPARENT && p.Grapheme() == GraphemeBreak::EXTEND && !IsContextFormat(c);
}

//! Why U+200C or U+200D is kept, if it is.
enum class Joiner { REPLACED, KEPT, KEPT_BETWEEN_LETTERS };

//! Whether U+200C or U+200D at text[i] is kept.
Joiner KeepJoiner(const std::vector<uint32_t>& text, size_t i, const Draft& draft)
{
    const uint32_t c = text[i];
    const bool has_next = i + 1 < text.size();
    const uint32_t next = has_next ? ClassValueAt(text, i + 1) : 0;
    const Properties next_p = GetProperties(next);
    // After a virama and before a letter.
    if (!draft.Empty() && draft.LastProperties().Has(unicode::VIRAMA) && has_next && next_p.Has(unicode::LETTER)) return Joiner::KEPT;
    // U+200D inside an emoji sequence.
    if (c == ZWJ) {
        if (!has_next || !next_p.Has(unicode::EXTENDED_PICTOGRAPHIC)) return Joiner::REPLACED;
        size_t k = draft.text.size();
        if (k > 0 && draft.text[k - 1] == VS16) --k;
        if (k > 0 && draft.properties[k - 1].Has(unicode::EMOJI_MODIFIER)) --k;
        return k > 0 && draft.properties[k - 1].Has(unicode::EXTENDED_PICTOGRAPHIC) ? Joiner::KEPT : Joiner::REPLACED;
    }
    // U+200C between two joining letters, looking past the transparent marks of its cluster on each side.
    size_t k = draft.text.size();
    for (size_t skipped = 0; k > 0 && skipped < MAX_CLUSTER_CODE_POINTS && IsTransparentMark(draft.text[k - 1], draft.properties[k - 1]); ++skipped) --k;
    if (k == 0) return Joiner::REPLACED;
    const JoiningType before = draft.properties[k - 1].Joining();
    if (before != JoiningType::DUAL_JOINING && before != JoiningType::LEFT_JOINING) return Joiner::REPLACED;
    size_t j = i + 1;
    for (size_t skipped = 0; j < text.size() && skipped < MAX_CLUSTER_CODE_POINTS; ++skipped, ++j) {
        const uint32_t value = ClassValueAt(text, j);
        if (!IsTransparentMark(value, GetProperties(value))) break;
    }
    if (j == text.size()) return Joiner::REPLACED;
    const JoiningType after = GetProperties(ClassValueAt(text, j)).Joining();
    return after == JoiningType::DUAL_JOINING || after == JoiningType::RIGHT_JOINING ? Joiner::KEPT_BETWEEN_LETTERS : Joiner::REPLACED;
}

//! Pass 3: the grapheme clusters of the draft, each cut to its limits.
std::vector<uint32_t> CapClusters(const Draft& draft, DisplayCounts& counts)
{
    const std::vector<uint32_t>& text = draft.text;
    std::vector<uint32_t> result;
    result.reserve(text.size());
    auto zwnj = draft.joining_zwnj.begin();
    for (size_t start = 0; start < text.size();) {
        size_t end = start + 1;
        while (end < text.size() && !draft.starts[end]) ++end;

        size_t cut = end;
        bool by_marks = false;
        size_t marks = 0;
        for (size_t k = start; k < end && k <= start + MAX_CLUSTER_CODE_POINTS; ++k) {
            if (IsCountedMark(text[k], draft.properties[k]) && ++marks > MAX_CLUSTER_MARKS) {
                cut = k;
                by_marks = true;
                break;
            }
        }
        if (!by_marks && end - start > MAX_CLUSTER_CODE_POINTS) cut = start + MAX_CLUSTER_CODE_POINTS;

        while (zwnj != draft.joining_zwnj.end() && *zwnj < start) ++zwnj;
        if (cut < end) {
            // The part kept ends with no format character, whose context the cut would take away, and keeps no
            // U+200C whose following marks it cuts.
            while (cut > start + 1 && IsContextFormat(text[cut - 1])) --cut;
            if (zwnj != draft.joining_zwnj.end() && *zwnj < cut) {
                cut = *zwnj;
                while (cut > start + 1 && IsContextFormat(text[cut - 1])) --cut;
            }
        }
        if (cut < end && !(end - cut == 1 && text[cut] == REPLACEMENT_CHARACTER)) {
            result.insert(result.end(), text.begin() + start, text.begin() + cut);
            result.push_back(REPLACEMENT_CHARACTER);
            (by_marks ? counts.marks : counts.cluster) += end - cut;
        } else {
            result.insert(result.end(), text.begin() + start, text.begin() + end);
        }
        start = end;
    }
    return result;
}

} // namespace

void DecodeUtf8(Span<const unsigned char> bytes, std::vector<uint32_t>& out, std::vector<uint32_t>* byte_offsets)
{
    out.clear();
    out.reserve(bytes.size());
    if (byte_offsets) {
        byte_offsets->clear();
        byte_offsets->reserve(bytes.size());
    }
    const size_t n = bytes.size();
    for (size_t i = 0; i < n;) {
        if (byte_offsets) byte_offsets->push_back(static_cast<uint32_t>(i));
        const unsigned char lead = bytes[i];
        if (lead < 0x80) {
            out.push_back(lead);
            ++i;
            continue;
        }
        size_t length;
        uint32_t cp;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            cp = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            cp = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            cp = lead & 0x07;
        } else {
            // A continuation byte, C0, C1 or F5 to FF: a maximal subpart of one byte.
            out.push_back(REPLACEMENT_CHARACTER);
            ++i;
            continue;
        }
        size_t j = 1;
        for (; j < length && i + j < n; ++j) {
            const unsigned char b = bytes[i + j];
            // The second byte's range excludes overlong forms (E0, F0), surrogates (ED) and values above U+10FFFF (F4).
            unsigned char lo = 0x80, hi = 0xBF;
            if (j == 1) {
                if (lead == 0xE0) lo = 0xA0;
                if (lead == 0xED) hi = 0x9F;
                if (lead == 0xF0) lo = 0x90;
                if (lead == 0xF4) hi = 0x8F;
            }
            if (b < lo || b > hi) break;
            cp = (cp << 6) | (b & 0x3F);
        }
        if (j == length) {
            out.push_back(cp);
        } else {
            out.push_back(REPLACEMENT_CHARACTER);
        }
        i += j;
    }
}

std::string EncodeUtf8(const std::vector<uint32_t>& text)
{
    std::string out;
    out.reserve(text.size());
    for (uint32_t c : text) {
        if (!IsScalarValue(c)) c = REPLACEMENT_CHARACTER;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

namespace {

//! The three passes over text, for DisplayMap.
std::vector<uint32_t> MapText(const std::vector<uint32_t>& text, DisplayCounts& counts)
{
    Draft draft(text.size());
    // Whether a direction mark was kept since the last strong character that starts a grapheme cluster. Strong
    // characters inside a cluster do not count: pass 3 may cut them off.
    bool mark_kept = false;

    for (size_t i = 0; i < text.size(); ++i) {
        const uint32_t c = text[i];
        if (IsLineBreakAt(text, i)) {
            if (c == CR) ++i;
            draft.Append(LINE_FEED, counts);
            mark_kept = false;
            continue;
        }
        uint32_t replacement;
        Note note;
        if (ReplaceByClass(c, replacement, note)) {
            draft.Append(replacement, counts);
            Count(counts, note);
            continue;
        }
        if (c == BLACK_FLAG) {
            const size_t flag = RgiFlagLength(text, i);
            for (size_t k = 0; k < std::max<size_t>(flag, 1); ++k) draft.Append(text[i + k], counts);
            if (flag > 0) i += flag - 1;
            continue;
        }
        bool keep = true;
        if (IsTag(c)) {
            keep = false;
        } else if (IsVariationSelector(c)) {
            keep = KeepVariationSelector(c, draft);
        } else if (c == ZWNJ || c == ZWJ) {
            const Joiner joiner = KeepJoiner(text, i, draft);
            keep = joiner != Joiner::REPLACED;
            if (joiner == Joiner::KEPT_BETWEEN_LETTERS) draft.joining_zwnj.push_back(draft.text.size());
        } else if (IsDirectionMark(c)) {
            const bool left_weak = draft.Empty() || !draft.LastProperties().Has(unicode::STRONG);
            const bool right_weak = i + 1 == text.size() || !GetProperties(ClassValueAt(text, i + 1)).Has(unicode::STRONG);
            keep = (draft.Empty() || !IsDirectionMark(draft.Last())) && (left_weak || right_weak) && !mark_kept;
            if (keep) mark_kept = true;
        } else {
            const bool start = draft.Append(c, counts);
            if (start && draft.LastProperties().Has(unicode::STRONG)) mark_kept = false;
            continue;
        }
        if (keep) {
            draft.Append(c, counts);
        } else {
            draft.Append(REPLACEMENT_CHARACTER, counts);
            ++counts.invisible;
        }
    }

    return CapClusters(draft, counts);
}

} // namespace

Mapped DisplayMap(const std::vector<uint32_t>& text, DisplayMode mode)
{
    Mapped mapped;
    mapped.m_mode = mode;
    if (mode == DisplayMode::MULTI_LINE) {
        mapped.m_text = MapText(text, mapped.m_counts);
        mapped.m_changed = mapped.m_text != text;
        return mapped;
    }
    // On one line every line break is shown as LINE_SYMBOL, and the rules apply to the text as it is shown, where the
    // symbol is an ordinary character: it joins a cluster as any symbol does, and it ends no run of direction marks.
    // So the limits hold for the text the line shows, and mapping that text again changes nothing.
    std::vector<uint32_t> one_line;
    one_line.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (IsLineBreakAt(text, i)) {
            if (text[i] == CR) ++i;
            one_line.push_back(LINE_SYMBOL);
        } else {
            one_line.push_back(text[i]);
        }
    }
    mapped.m_text = MapText(one_line, mapped.m_counts);
    // A line feed shown as the symbol is no change; CR LF, U+2028 and U+2029 shown as it are, as on several lines.
    mapped.m_changed = mapped.m_text.size() != text.size() ||
                       !std::equal(mapped.m_text.begin(), mapped.m_text.end(), text.begin(),
                                   [](uint32_t shown, uint32_t c) { return shown == (c == LINE_FEED ? LINE_SYMBOL : c); });
    return mapped;
}

std::vector<size_t> GraphemeBreaks(const std::vector<uint32_t>& text)
{
    std::vector<size_t> breaks;
    Segmenter segmenter;
    for (size_t i = 0; i < text.size(); ++i) {
        if (segmenter.Push(GetProperties(text[i]))) breaks.push_back(i);
    }
    breaks.push_back(text.size());
    return breaks;
}

std::vector<uint32_t> FoldForSearch(const std::vector<uint32_t>& text, bool case_insensitive)
{
    std::vector<uint32_t> folded;
    folded.reserve(text.size());
    for (const uint32_t c : text) {
        const Properties p = GetProperties(c);
        if (p.Has(unicode::DEFAULT_IGNORABLE) || p.Has(unicode::FORMAT)) continue;
        if (p.Has(unicode::WHITE_SPACE)) {
            folded.push_back(SPACE);
        } else {
            folded.push_back(case_insensitive ? unicode::SimpleCaseFold(c) : c);
        }
    }
    return folded;
}

std::string Skeleton(Span<const unsigned char> payload)
{
    std::vector<uint32_t> text;
    DecodeUtf8(payload.first(std::min(payload.size(), SKELETON_MAX_BYTES)), text);
    std::vector<uint32_t> kept;
    kept.reserve(text.size());
    for (const uint32_t c : text) {
        const Properties p = GetProperties(c);
        if (p.Has(unicode::DEFAULT_IGNORABLE) || p.Has(unicode::FORMAT) || p.Has(unicode::WHITE_SPACE) || p.Has(unicode::MARK) || c < 0x20 ||
            (c >= DEL && c <= 0x9F) || IsPrivateUse(c) || IsNoncharacter(c) || c == BRAILLE_BLANK || (c >= 0xFFF9 && c <= 0xFFFC)) {
            continue;
        }
        kept.push_back(unicode::SimpleCaseFold(c));
    }
    return EncodeUtf8(kept);
}

} // namespace fext
