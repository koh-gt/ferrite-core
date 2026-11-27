// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FEXT_TEXT_H
#define BITCOIN_FEXT_TEXT_H

#include <span.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * The text rules of FEXT: how the bytes of an OP_RETURN message become text, and how that text is shown, searched
 * and compared. A message is written by anyone, so no byte of it is trusted: it is decoded in exactly one place
 * (DecodeUtf8) and made into visible characters in exactly one place (DisplayMap), and what is shown,
 * copied or exported as text is DisplayMap's output. DisplayMap replaces, with a visible U+FFFD or a control
 * picture, every character that could reorder the rest of a line, hide text, imitate a line of the program or make
 * the text costly to lay out, and keeps everything a script or an emoji needs. All decisions come from FEXT's own
 * Unicode tables (fext/unicode_data.h), so they do not depend on the Qt version. Every function takes time linear
 * in its input and memory proportional to it.
 */
namespace fext {

static constexpr uint32_t REPLACEMENT_CHARACTER{0xFFFD};
//! A line break as DisplayMap shows it on several lines.
static constexpr uint32_t LINE_FEED{0x000A};
//! A line break as DisplayMap shows it on one line (U+21B5, DOWNWARDS ARROW WITH CORNER LEFTWARDS).
static constexpr uint32_t LINE_SYMBOL{0x21B5};

//! A grapheme cluster that DisplayMap returns has at most this many code points before the U+FFFD that stands for
//! the rest of a longer one.
static constexpr size_t MAX_CLUSTER_CODE_POINTS{32};
//! ... and at most this many combining marks.
static constexpr size_t MAX_CLUSTER_MARKS{8};
//! A run of regional indicators (the halves of flags) that DisplayMap returns has at most this many.
static constexpr size_t MAX_REGIONAL_INDICATORS{64};
//! Skeleton reads at most this many bytes of a payload.
static constexpr size_t SKELETON_MAX_BYTES{4096};

/**
 * Decodes bytes as UTF-8 by RFC 3629 into `out` (which it clears first). Overlong forms, encoded surrogates (U+D800
 * to U+DFFF), values above U+10FFFF and sequences cut off are ill-formed; each maximal subpart of an ill-formed
 * sequence becomes one U+FFFD, as the Unicode Standard recommends (chapter 3, "U+FFFD Substitution of Maximal
 * Subparts"). It reads exactly bytes.size() bytes and does not stop at a NUL. With byte_offsets, it also gives, for
 * each code point, the offset of its first byte; the offsets have 32 bits, so bytes must then be shorter than 2^32
 * bytes, as every OP_RETURN script is.
 */
void DecodeUtf8(Span<const unsigned char> bytes, std::vector<uint32_t>& out, std::vector<uint32_t>* byte_offsets = nullptr);

//! Encodes code points as UTF-8. A value that is not a Unicode scalar value (a surrogate or a value above
//! U+10FFFF) is encoded as U+FFFD.
std::string EncodeUtf8(const std::vector<uint32_t>& text);

//! How DisplayMap shows line breaks: as line breaks (the details window, expanded rows and cards, copied text), or
//! as LINE_SYMBOL on one line (list cells, the message line of a card, export).
enum class DisplayMode {
    MULTI_LINE,
    ONE_LINE,
};

//! How many code points DisplayMap replaced, by the kind of character.
struct DisplayCounts {
    //! C0 controls other than the line feed (shown as control pictures), DEL (shown as U+2421) and C1 controls.
    size_t control{0};
    //! Direction embeddings, overrides and isolates.
    size_t direction{0};
    //! Default-ignorable code points; joiners, direction marks, variation selectors and tag characters outside the
    //! sequences that need them; blank symbols; annotation and object characters.
    size_t invisible{0};
    //! Private use code points, noncharacters, and values that are not Unicode scalar values.
    size_t private_use{0};
    //! The code points of grapheme clusters from their ninth combining mark on.
    size_t marks{0};
    //! The code points of grapheme clusters beyond their first 32, and of runs of regional indicators beyond their
    //! first 64.
    size_t cluster{0};

    size_t Total() const { return control + direction + invisible + private_use + marks + cluster; }
};

/**
 * Text as DisplayMap made it for showing. Only DisplayMap makes one, so a type that holds shown text can require a
 * Mapped and so cannot be given decoded text by mistake.
 */
class Mapped
{
public:
    Mapped() = default;

    //! The code points to show.
    const std::vector<uint32_t>& Text() const { return m_text; }
    //! What DisplayMap replaced.
    const DisplayCounts& Counts() const { return m_counts; }
    DisplayMode Mode() const { return m_mode; }
    //! Whether the text shown differs from the decoded text. In ONE_LINE mode a line feed shown as LINE_SYMBOL is no
    //! difference; CR LF, U+2028 and U+2029 shown as it are, as they are in MULTI_LINE mode, where they become LF.
    bool Changed() const { return m_changed; }

private:
    friend Mapped DisplayMap(const std::vector<uint32_t>& text, DisplayMode mode);

    std::vector<uint32_t> m_text;
    DisplayCounts m_counts;
    DisplayMode m_mode{DisplayMode::MULTI_LINE};
    bool m_changed{false};
};

/**
 * The text to show for decoded text. It works in three passes:
 *
 * 1. Code points are mapped by their class alone: CR LF, LF, U+2028 and U+2029 become one line break; other C0
 *    controls become their control picture (U+2400 + the code point, so NUL shows as U+2400) and DEL becomes U+2421;
 *    C1 controls, direction embeddings, overrides and isolates, default-ignorable code points, U+2800, U+FFF9 to
 *    U+FFFC, private use code points, noncharacters and values that are not Unicode scalar values become U+FFFD.
 * 2. Format characters are kept only where a script or an emoji needs them, and otherwise become U+FFFD:
 *    - U+FE0E and U+FE0F directly after a base of an emoji variation sequence; U+FE00 to U+FE0D directly after a
 *      base of a standardized variation sequence with that selector; U+E0100 to U+E01EF directly after a unified
 *      ideograph; the Mongolian free variation selectors directly after a Mongolian letter;
 *    - U+200C and U+200D after a virama and before a letter; U+200D inside an emoji sequence (after an
 *      Extended_Pictographic character, optionally followed by U+FE0F or an emoji modifier, and before one);
 *      U+200C between two joining letters (Joining_Type D or L before it and D or R after it, skipping only the
 *      transparent combining marks of its own grapheme cluster);
 *    - U+200E, U+200F and U+061C where a direction mark can act: at least one neighbour is not strong (Bidi_Class
 *      other than L, R and AL; the start and the end of a line count as not strong), it does not follow another
 *      direction mark, and no other mark was kept since the last line break or strong character that starts a
 *      grapheme cluster;
 *    - U+1F3F4 followed by tag characters and U+E007F only as the flags of England, Scotland and Wales; the tag
 *      characters of any other sequence, and every other tag character, become U+FFFD.
 *    A run of regional indicators keeps its first 64; the rest of the run becomes one U+FFFD.
 * 3. The result is cut into extended grapheme clusters (GraphemeBreaks). A cluster keeps at most its first 32 code
 *    points and its first 8 combining marks (grapheme break Extend or SpacingMark, other than joiners, variation
 *    selectors, emoji modifiers and tag characters); the rest of a longer cluster becomes one U+FFFD. The part kept
 *    never ends with a format character and never keeps a U+200C whose context was cut, so that mapping the result
 *    again changes nothing.
 *
 * Everything else is kept: letters, numbers, punctuation, symbols, spaces, emoji, and code points not yet assigned in
 * the tables' Unicode version.
 *
 * In ONE_LINE mode every line break is replaced by LINE_SYMBOL first, and the three passes apply to that text, in
 * which the symbol is an ordinary character: it joins a grapheme cluster as any symbol does (after a Prepend
 * character, before a combining mark), and it ends no run of direction marks. So the result has no line break, its
 * clusters keep the limits above, and the limits hold for the text the line shows. Where no line break has a
 * Prepend character before it, a character that extends a cluster (grapheme break Extend or SpacingMark, such as a
 * combining mark or an emoji modifier) after it, or a direction mark kept on each side with no strong character
 * between them, the result is that of MULTI_LINE mode with every line break shown as LINE_SYMBOL.
 *
 * In both modes the mapping is idempotent: mapping its result again in the same mode gives the same text.
 */
Mapped DisplayMap(const std::vector<uint32_t>& text, DisplayMode mode);

/**
 * The extended grapheme clusters of text by UAX #29 of the tables' Unicode version (rules GB1 to GB13 and GB999,
 * with GB9c), as boundaries: 0, every position between two clusters, and text.size(). For empty text, {0}.
 */
std::vector<size_t> GraphemeBreaks(const std::vector<uint32_t>& text);

/**
 * The text that search and filters look at: decoded text without its default-ignorable code points (the joiners,
 * direction marks, variation selectors and tag characters that DisplayMap keeps among them) and without its format
 * characters (General_Category Cf, some of which DisplayMap keeps and a font may draw with no width), with every
 * white space character (White_Space: U+00A0, U+2000 to U+200A, U+3000, the line breaks and the others) as U+0020,
 * and with simple case folding when case_insensitive. So a word cannot hide from a search by invisible characters
 * inside it, words apart by any white space character are found with U+0020 between them, and text hidden in tag
 * characters is never found.
 */
std::vector<uint32_t> FoldForSearch(const std::vector<uint32_t>& text, bool case_insensitive);

/**
 * The skeleton of a payload, which texts that look alike share: of the text of at most its first SKELETON_MAX_BYTES
 * bytes, every code point but default-ignorable code points, format characters, controls, private use code points,
 * noncharacters, combining marks, white space, U+2800 and U+FFF9 to U+FFFC, with simple case folding, encoded as
 * UTF-8.
 */
std::string Skeleton(Span<const unsigned char> payload);

} // namespace fext

#endif // BITCOIN_FEXT_TEXT_H
