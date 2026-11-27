// Copyright (c) 2026 The Ferrite Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_FEXT_UNICODE_DATA_H
#define BITCOIN_FEXT_UNICODE_DATA_H

#include <cstddef>
#include <cstdint>

/**
 * The Unicode properties that the text rules of fext/text.h use, from FEXT's own tables of one Unicode version
 * (unicode_data.cpp, written by contrib/devtools/gen-fext-unicode.py from the Unicode Character Database). The rules
 * never take a property from Qt or from the C++ library, whose tables belong to other Unicode versions.
 */
namespace fext {
namespace unicode {

//! The Unicode version of the tables, such as "15.1.0".
extern const char* const UNICODE_VERSION;

//! A file of the Unicode Character Database the tables were made from, and the SHA-256 of its content in hex.
struct SourceFile {
    const char* name;
    const char* sha256;
};
extern const SourceFile SOURCE_FILES[];
extern const size_t SOURCE_FILE_COUNT;

static constexpr uint32_t MAX_CODE_POINT{0x10FFFF};
static constexpr uint32_t HANGUL_FIRST{0xAC00};
static constexpr uint32_t HANGUL_T_COUNT{28};

//! Grapheme_Cluster_Break (UAX #29). The numbers are those the tables store.
enum class GraphemeBreak : uint8_t {
    OTHER = 0,
    CR = 1,
    LF = 2,
    CONTROL = 3,
    EXTEND = 4,
    ZWJ = 5,
    REGIONAL_INDICATOR = 6,
    PREPEND = 7,
    SPACING_MARK = 8,
    L = 9,
    V = 10,
    T = 11,
    LV = 12,
    LVT = 13,
};

//! Indic_Conjunct_Break (DerivedCoreProperties.txt), for rule GB9c of UAX #29.
enum class IndicConjunctBreak : uint8_t {
    NONE = 0,
    LINKER = 1,
    CONSONANT = 2,
    EXTEND = 3,
};

//! Joining_Type (ArabicShaping.txt).
enum class JoiningType : uint8_t {
    NON_JOINING = 0,
    JOIN_CAUSING = 1,
    DUAL_JOINING = 2,
    LEFT_JOINING = 3,
    RIGHT_JOINING = 4,
    TRANSPARENT = 5,
};

static constexpr uint32_t GRAPHEME_BREAK_MASK{0x0f};
static constexpr int INDIC_CONJUNCT_BREAK_SHIFT{4};
static constexpr uint32_t INDIC_CONJUNCT_BREAK_MASK{0x03};
static constexpr int JOINING_TYPE_SHIFT{6};
static constexpr uint32_t JOINING_TYPE_MASK{0x07};

//! The binary properties, one bit each.
enum PropertyBit : uint32_t {
    EXTENDED_PICTOGRAPHIC = 1u << 9,  //!< Extended_Pictographic (emoji-data.txt)
    EMOJI_MODIFIER = 1u << 10,        //!< Emoji_Modifier, the five skin tones (emoji-data.txt)
    DEFAULT_IGNORABLE = 1u << 11,     //!< Default_Ignorable_Code_Point (DerivedCoreProperties.txt)
    WHITE_SPACE = 1u << 12,           //!< White_Space (PropList.txt)
    STRONG = 1u << 13,                //!< Bidi_Class L, R or AL (DerivedBidiClass.txt, with its defaults)
    LETTER = 1u << 14,                //!< General_Category L (Lu, Ll, Lt, Lm, Lo)
    MARK = 1u << 15,                  //!< General_Category M (Mn, Mc, Me)
    VIRAMA = 1u << 16,                //!< Canonical_Combining_Class 9
    UNIFIED_IDEOGRAPH = 1u << 17,     //!< Unified_Ideograph (PropList.txt)
    MONGOLIAN_LETTER = 1u << 18,      //!< Script Mongolian and General_Category Lo
    EMOJI_VARIATION_BASE = 1u << 19,  //!< the base of an emoji variation sequence (emoji-variation-sequences.txt)
    FORMAT = 1u << 20,                //!< General_Category Cf
};

//! The properties of one code point.
struct Properties {
    uint32_t bits;

    GraphemeBreak Grapheme() const { return static_cast<GraphemeBreak>(bits & GRAPHEME_BREAK_MASK); }
    IndicConjunctBreak IndicConjunct() const { return static_cast<IndicConjunctBreak>((bits >> INDIC_CONJUNCT_BREAK_SHIFT) & INDIC_CONJUNCT_BREAK_MASK); }
    JoiningType Joining() const { return static_cast<JoiningType>((bits >> JOINING_TYPE_SHIFT) & JOINING_TYPE_MASK); }
    bool Has(PropertyBit bit) const { return (bits & bit) != 0; }
};

/**
 * The properties of a code point. A value above U+10FFFF, which is not a code point, gets the grapheme break
 * property Control, so that a cluster boundary lies on each side of it, and no other property.
 */
Properties GetProperties(uint32_t cp);

//! Whether base followed by selector (one of U+FE00 to U+FE0D) is a standardized variation sequence.
bool IsStandardizedVariant(uint32_t base, uint32_t selector);

//! The simple case folding of a code point (CaseFolding.txt, status C and S); the code point itself if it has none.
uint32_t SimpleCaseFold(uint32_t cp);

} // namespace unicode
} // namespace fext

#endif // BITCOIN_FEXT_UNICODE_DATA_H
