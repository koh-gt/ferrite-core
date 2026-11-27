#!/usr/bin/env python3
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Write the Unicode tables of the FEXT text rules (src/fext/unicode_data.cpp).

The text rules of src/fext/text.cpp decide every property of a code point from these tables, never from the
character tables of Qt or of the C++ library, so that the rules give the same result whatever the Qt version. The
tables come from the files of the Unicode Character Database of one version, read from a directory: the content of
UCD.zip, with emoji/emoji-sequences.txt added from the emoji files of the same version. They are downloaded by hand
once per Unicode version. Run this script when the Unicode version is raised, and commit what it writes:

    contrib/devtools/gen-fext-unicode.py <directory of the Unicode Character Database files>

It writes src/fext/unicode_data.cpp, with the SHA-256 of every file it read, and copies
auxiliary/GraphemeBreakTest.txt, Unicode's own test of grapheme cluster boundaries, to
src/test/data/fext_grapheme_break_test.txt for the unit tests. Before writing it checks the files against each
other and, when Python's unicodedata module has the same Unicode version, the general categories, bidirectional
classes and combining classes against that module.
"""

import argparse
import hashlib
import os
import re
import shutil
import sys
import unicodedata

MAX_CODE_POINT = 0x10FFFF
SIZE = MAX_CODE_POINT + 1
HANGUL_FIRST, HANGUL_LAST, HANGUL_T_COUNT = 0xAC00, 0xD7A3, 28

# The packed properties of a code point. Keep these in step with src/fext/unicode_data.h.
GRAPHEME_BREAK = {
    'Other': 0, 'CR': 1, 'LF': 2, 'Control': 3, 'Extend': 4, 'ZWJ': 5, 'Regional_Indicator': 6, 'Prepend': 7,
    'SpacingMark': 8, 'L': 9, 'V': 10, 'T': 11, 'LV': 12, 'LVT': 13,
}
INDIC_CONJUNCT_BREAK = {'None': 0, 'Linker': 1, 'Consonant': 2, 'Extend': 3}
INDIC_CONJUNCT_BREAK_SHIFT = 4
JOINING_TYPE = {'U': 0, 'C': 1, 'D': 2, 'L': 3, 'R': 4, 'T': 5}
JOINING_TYPE_SHIFT = 6
EXTENDED_PICTOGRAPHIC = 1 << 9
EMOJI_MODIFIER = 1 << 10
DEFAULT_IGNORABLE = 1 << 11
WHITE_SPACE = 1 << 12
STRONG = 1 << 13
LETTER = 1 << 14
MARK = 1 << 15
VIRAMA = 1 << 16
UNIFIED_IDEOGRAPH = 1 << 17
MONGOLIAN_LETTER = 1 << 18
EMOJI_VARIATION_BASE = 1 << 19
FORMAT = 1 << 20

# The files read, in the order of the list in the generated file.
FILES = [
    'UnicodeData.txt',
    'DerivedCoreProperties.txt',
    'PropList.txt',
    'CaseFolding.txt',
    'auxiliary/GraphemeBreakProperty.txt',
    'auxiliary/GraphemeBreakTest.txt',
    'emoji/emoji-data.txt',
    'emoji/emoji-variation-sequences.txt',
    'emoji/emoji-sequences.txt',
    'StandardizedVariants.txt',
    'ArabicShaping.txt',
    'extracted/DerivedJoiningType.txt',
    'Scripts.txt',
    'extracted/DerivedBidiClass.txt',
]

# The only emoji tag sequences that are recommended for general interchange (RGI): the flags of England, Scotland
# and Wales. src/fext/text.cpp keeps exactly these.
RGI_TAG_SEQUENCES = {
    (0x1F3F4, 0xE0067, 0xE0062, 0xE0065, 0xE006E, 0xE0067, 0xE007F),
    (0x1F3F4, 0xE0067, 0xE0062, 0xE0073, 0xE0063, 0xE0074, 0xE007F),
    (0x1F3F4, 0xE0067, 0xE0062, 0xE0077, 0xE006C, 0xE0073, 0xE007F),
}

TEST_DATA = 'src/test/data/fext_grapheme_break_test.txt'
OUTPUT = 'src/fext/unicode_data.cpp'


def fail(message):
    sys.exit('gen-fext-unicode.py: ' + message)


class Reader:
    """Reads the data files of one directory and remembers what it read."""

    def __init__(self, directory):
        self.directory = directory
        self.hashes = {}

    def lines(self, name):
        path = os.path.join(self.directory, name)
        with open(path, 'rb') as f:
            data = f.read()
        self.hashes[name] = hashlib.sha256(data).hexdigest()
        return data.decode('utf8').splitlines()

    def fields(self, name):
        """The fields of every data line, comments removed."""
        for line in self.lines(name):
            line = line.split('#', 1)[0].strip()
            if line:
                yield [field.strip() for field in line.split(';')]


def code_points(text):
    """The code points of "XXXX" or "XXXX..YYYY" as a range."""
    if '..' in text:
        first, last = text.split('..')
        return range(int(first, 16), int(last, 16) + 1)
    return range(int(text, 16), int(text, 16) + 1)


def check_versions(reader, version):
    """Every file must belong to the same version of the Unicode Character Database and of the emoji data."""
    emoji_version = '.'.join(version.split('.')[:2])
    for name in FILES:
        if name == 'UnicodeData.txt':
            continue
        head = '\n'.join(reader.lines(name)[:12])
        base = os.path.basename(name)[:-len('.txt')]
        if name.startswith('emoji/'):
            if not re.search(r'(Emoji Version|# Version:) %s\b' % re.escape(emoji_version), head):
                fail('%s is not of emoji version %s' % (name, emoji_version))
        elif not head.startswith('# %s-%s.txt' % (base, version)):
            fail('%s is not of Unicode version %s' % (name, version))


def read_version(reader):
    head = reader.lines('DerivedCoreProperties.txt')[0]
    match = re.match(r'# DerivedCoreProperties-(\d+\.\d+\.\d+)\.txt$', head)
    if not match:
        fail('cannot read the Unicode version from DerivedCoreProperties.txt')
    return match.group(1)


def read_unicode_data(reader):
    """The general category and the canonical combining class of every code point (Cn and 0 when unassigned)."""
    category = ['Cn'] * SIZE
    combining = [0] * SIZE
    first = None
    for fields in reader.fields('UnicodeData.txt'):
        cp, name, gc, ccc = int(fields[0], 16), fields[1], fields[2], int(fields[3])
        if name.endswith(', First>'):
            first = cp
            continue
        start = first if name.endswith(', Last>') else cp
        first = None
        for c in range(start, cp + 1):
            category[c] = gc
            combining[c] = ccc
    return category, combining


def read_binary(reader, name, prop):
    """The code points that have a binary property in a file of "range ; property" lines."""
    found = set()
    for fields in reader.fields(name):
        if len(fields) == 2 and fields[1] == prop:
            found.update(code_points(fields[0]))
    if not found:
        fail('no %s in %s' % (prop, name))
    return found


def read_grapheme_break(reader):
    values = ['Other'] * SIZE
    for fields in reader.fields('auxiliary/GraphemeBreakProperty.txt'):
        if fields[1] not in GRAPHEME_BREAK:
            fail('unknown grapheme break value ' + fields[1])
        for c in code_points(fields[0]):
            values[c] = fields[1]
    # The Hangul syllables are LV when they have no trailing consonant and LVT otherwise; the tables keep LV for
    # all of them and the lookup tells them apart by the syllable's number.
    for c in range(HANGUL_FIRST, HANGUL_LAST + 1):
        expected = 'LV' if (c - HANGUL_FIRST) % HANGUL_T_COUNT == 0 else 'LVT'
        if values[c] != expected:
            fail('U+%04X: grapheme break %s, the syllable formula gives %s' % (c, values[c], expected))
    for c in range(SIZE):
        if values[c] in ('LV', 'LVT') and not HANGUL_FIRST <= c <= HANGUL_LAST:
            fail('U+%04X is LV or LVT outside the Hangul syllables' % c)
    return values


def read_indic_conjunct_break(reader):
    values = ['None'] * SIZE
    for fields in reader.fields('DerivedCoreProperties.txt'):
        if len(fields) == 3 and fields[1] == 'InCB':
            if fields[2] not in INDIC_CONJUNCT_BREAK:
                fail('unknown InCB value ' + fields[2])
            for c in code_points(fields[0]):
                values[c] = fields[2]
    return values


def read_joining_type(reader, category):
    """Joining_Type from ArabicShaping.txt, with the rule for code points it does not list: Transparent for the
    general categories Mn, Me and Cf, Non_Joining for all others. Checked against extracted/DerivedJoiningType.txt."""
    listed = {}
    for fields in reader.fields('ArabicShaping.txt'):
        listed[int(fields[0], 16)] = fields[2]
    values = []
    for c in range(SIZE):
        if c in listed:
            values.append(listed[c])
        elif category[c] in ('Mn', 'Me', 'Cf'):
            values.append('T')
        else:
            values.append('U')
    names = {'Join_Causing': 'C', 'Dual_Joining': 'D', 'Left_Joining': 'L', 'Right_Joining': 'R',
             'Transparent': 'T', 'Non_Joining': 'U'}
    derived = ['U'] * SIZE
    for fields in reader.fields('extracted/DerivedJoiningType.txt'):
        for c in code_points(fields[0]):
            derived[c] = names.get(fields[1], fields[1])
    for c in range(SIZE):
        if values[c] != derived[c]:
            fail('U+%04X: joining type %s from ArabicShaping.txt, %s in DerivedJoiningType.txt' % (c, values[c], derived[c]))
    return values


def read_bidi_class(reader):
    """The bidirectional class of every code point, with the defaults of the file's @missing lines."""
    short = {'Left_To_Right': 'L', 'Right_To_Left': 'R', 'Arabic_Letter': 'AL', 'European_Terminator': 'ET',
             'Boundary_Neutral': 'BN', 'Nonspacing_Mark': 'NSM', 'European_Number': 'EN', 'Arabic_Number': 'AN'}
    values = [None] * SIZE
    for line in reader.lines('extracted/DerivedBidiClass.txt'):
        match = re.match(r'# @missing: ([0-9A-F.]+); (\w+)', line)
        if match:
            if match.group(2) not in short:
                fail('unknown default bidi class ' + match.group(2))
            for c in code_points(match.group(1)):
                values[c] = short[match.group(2)]
    for fields in reader.fields('extracted/DerivedBidiClass.txt'):
        for c in code_points(fields[0]):
            values[c] = fields[1]
    if None in values:
        fail('DerivedBidiClass.txt leaves a code point without a class')
    return values


def read_scripts(reader, script):
    return read_binary(reader, 'Scripts.txt', script)


def read_emoji_variation_bases(reader):
    """The bases of the emoji variation sequences; each must have both a text and an emoji presentation sequence."""
    by_selector = {0xFE0E: set(), 0xFE0F: set()}
    for fields in reader.fields('emoji/emoji-variation-sequences.txt'):
        base, selector = [int(x, 16) for x in fields[0].split()]
        if selector not in by_selector:
            fail('unexpected selector in emoji-variation-sequences.txt: %X' % selector)
        by_selector[selector].add(base)
    if by_selector[0xFE0E] != by_selector[0xFE0F]:
        fail('the bases of U+FE0E and U+FE0F differ in emoji-variation-sequences.txt')
    return by_selector[0xFE0F]


def read_standardized_variants(reader):
    """The standardized variation sequences of U+FE00 to U+FE0D, as (base, selector) pairs. The Mongolian free
    variation selectors are kept after any Mongolian letter and need no list; ideographic variation sequences
    (U+E0100 to U+E01EF) are not in this file."""
    pairs = set()
    for fields in reader.fields('StandardizedVariants.txt'):
        base, selector = [int(x, 16) for x in fields[0].split()]
        if 0xFE00 <= selector <= 0xFE0D:
            pairs.add((base, selector))
        elif selector not in (0x180B, 0x180C, 0x180D, 0x180F):
            fail('unexpected selector in StandardizedVariants.txt: %X' % selector)
    return pairs


def read_case_folding(reader):
    """Simple case folding: the mappings of status C and S."""
    folding = {}
    for fields in reader.fields('CaseFolding.txt'):
        if fields[1] in ('C', 'S'):
            target = fields[2].split()
            if len(target) != 1:
                fail('a simple case folding of more than one code point: ' + fields[0])
            folding[int(fields[0], 16)] = int(target[0], 16)
    return folding


def check_tag_sequences(reader):
    found = set()
    for fields in reader.fields('emoji/emoji-sequences.txt'):
        if fields[1] == 'RGI_Emoji_Tag_Sequence':
            found.add(tuple(int(x, 16) for x in fields[0].split()))
    if found != RGI_TAG_SEQUENCES:
        fail('the RGI emoji tag sequences are no longer the flags of England, Scotland and Wales; '
             'src/fext/text.cpp must be changed with the tables')


def check_against_python(version, category, combining, bidi):
    """Compares with Python's unicodedata when it has the same Unicode version."""
    if unicodedata.unidata_version != version:
        print('Not compared with Python\'s unicodedata, which has Unicode %s, not %s' % (unicodedata.unidata_version, version))
        return
    for c in range(SIZE):
        ch = chr(c)
        if unicodedata.category(ch) != category[c]:
            fail('U+%04X: general category %s, Python says %s' % (c, category[c], unicodedata.category(ch)))
        if unicodedata.combining(ch) != combining[c]:
            fail('U+%04X: combining class %d, Python says %d' % (c, combining[c], unicodedata.combining(ch)))
        # Python gives no bidirectional class to an unassigned code point.
        if category[c] != 'Cn' and unicodedata.bidirectional(ch) != bidi[c]:
            fail('U+%04X: bidi class %s, Python says %s' % (c, bidi[c], unicodedata.bidirectional(ch)))
    print('Compared the general categories, combining classes and bidi classes with Python\'s unicodedata %s' % version)


def ranges_of(predicate):
    """The ranges of code points for which predicate holds, as (first, last) pairs."""
    result = []
    start = None
    for c in range(SIZE + 1):
        inside = c < SIZE and predicate(c)
        if inside and start is None:
            start = c
        elif not inside and start is not None:
            result.append((start, c - 1))
            start = None
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('ucd', help='the directory of the Unicode Character Database files')
    parser.add_argument('--top', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'),
                        help='the top of the source tree (default: the one this script is in)')
    args = parser.parse_args()

    reader = Reader(args.ucd)
    version = read_version(reader)
    check_versions(reader, version)

    category, combining = read_unicode_data(reader)
    grapheme = read_grapheme_break(reader)
    incb = read_indic_conjunct_break(reader)
    joining = read_joining_type(reader, category)
    bidi = read_bidi_class(reader)
    default_ignorable = read_binary(reader, 'DerivedCoreProperties.txt', 'Default_Ignorable_Code_Point')
    white_space = read_binary(reader, 'PropList.txt', 'White_Space')
    unified = read_binary(reader, 'PropList.txt', 'Unified_Ideograph')
    pictographic = read_binary(reader, 'emoji/emoji-data.txt', 'Extended_Pictographic')
    modifier = read_binary(reader, 'emoji/emoji-data.txt', 'Emoji_Modifier')
    mongolian = read_scripts(reader, 'Mongolian')
    emoji_bases = read_emoji_variation_bases(reader)
    variants = read_standardized_variants(reader)
    folding = read_case_folding(reader)
    check_tag_sequences(reader)
    reader.lines('auxiliary/GraphemeBreakTest.txt')
    check_against_python(version, category, combining, bidi)

    def packed(c):
        g = grapheme[c]
        if g == 'LVT':
            g = 'LV'
        bits = GRAPHEME_BREAK[g]
        bits |= INDIC_CONJUNCT_BREAK[incb[c]] << INDIC_CONJUNCT_BREAK_SHIFT
        bits |= JOINING_TYPE[joining[c]] << JOINING_TYPE_SHIFT
        if c in pictographic:
            bits |= EXTENDED_PICTOGRAPHIC
        if c in modifier:
            bits |= EMOJI_MODIFIER
        if c in default_ignorable:
            bits |= DEFAULT_IGNORABLE
        if c in white_space:
            bits |= WHITE_SPACE
        if bidi[c] in ('L', 'R', 'AL'):
            bits |= STRONG
        if category[c].startswith('L'):
            bits |= LETTER
        if category[c].startswith('M'):
            bits |= MARK
        if combining[c] == 9:
            bits |= VIRAMA
        if c in unified:
            bits |= UNIFIED_IDEOGRAPH
        if c in mongolian and category[c] == 'Lo':
            bits |= MONGOLIAN_LETTER
        if c in emoji_bases:
            bits |= EMOJI_VARIATION_BASE
        if category[c] == 'Cf':
            bits |= FORMAT
        return bits

    values = [packed(c) for c in range(SIZE)]
    runs = []
    for c in range(SIZE):
        if c == 0 or values[c] != values[c - 1]:
            runs.append((c, values[c]))

    # Range counts of the properties, for the comment of the generated file and the unit tests.
    counts = [
        ('Grapheme_Cluster_Break=Control', ranges_of(lambda c: grapheme[c] == 'Control')),
        ('Grapheme_Cluster_Break=Extend', ranges_of(lambda c: grapheme[c] == 'Extend')),
        ('Grapheme_Cluster_Break=Prepend', ranges_of(lambda c: grapheme[c] == 'Prepend')),
        ('Grapheme_Cluster_Break=SpacingMark', ranges_of(lambda c: grapheme[c] == 'SpacingMark')),
        ('Grapheme_Cluster_Break=L, V, T', ranges_of(lambda c: grapheme[c] in ('L', 'V', 'T'))),
        ('Grapheme_Cluster_Break=LV, LVT', ranges_of(lambda c: grapheme[c] in ('LV', 'LVT'))),
        ('Grapheme_Cluster_Break=Regional_Indicator', ranges_of(lambda c: grapheme[c] == 'Regional_Indicator')),
        ('Indic_Conjunct_Break=Linker', ranges_of(lambda c: incb[c] == 'Linker')),
        ('Indic_Conjunct_Break=Consonant', ranges_of(lambda c: incb[c] == 'Consonant')),
        ('Indic_Conjunct_Break=Extend', ranges_of(lambda c: incb[c] == 'Extend')),
        ('Joining_Type=D, R, L, T, C', ranges_of(lambda c: joining[c] != 'U')),
        ('Extended_Pictographic', ranges_of(lambda c: c in pictographic)),
        ('Emoji_Modifier', ranges_of(lambda c: c in modifier)),
        ('Default_Ignorable_Code_Point', ranges_of(lambda c: c in default_ignorable)),
        ('White_Space', ranges_of(lambda c: c in white_space)),
        ('Bidi_Class=L, R, AL', ranges_of(lambda c: bidi[c] in ('L', 'R', 'AL'))),
        ('General_Category=L', ranges_of(lambda c: category[c].startswith('L'))),
        ('General_Category=M', ranges_of(lambda c: category[c].startswith('M'))),
        ('General_Category=Cf', ranges_of(lambda c: category[c] == 'Cf')),
        ('Canonical_Combining_Class=9', ranges_of(lambda c: combining[c] == 9)),
        ('Unified_Ideograph', ranges_of(lambda c: c in unified)),
        ('Script=Mongolian and General_Category=Lo', ranges_of(lambda c: c in mongolian and category[c] == 'Lo')),
        ('Emoji variation bases', ranges_of(lambda c: c in emoji_bases)),
    ]

    top = os.path.abspath(args.top)
    out = []
    w = out.append
    w('// Copyright (c) 2026 The Ferrite Core developers')
    w('// Distributed under the MIT software license, see the accompanying')
    w('// file COPYING or http://www.opensource.org/licenses/mit-license.php.')
    w('')
    w('// This file is generated by contrib/devtools/gen-fext-unicode.py from the files of the Unicode Character')
    w('// Database %s. Do not edit it; run the script again when the Unicode version is raised.' % version)
    w('//')
    w('// Ranges and code points of each property, as the script counted them:')
    for name, ranges in counts:
        w('//   %-44s %5d ranges, %7d code points' % (name, len(ranges), sum(last - first + 1 for first, last in ranges)))
    w('//   %-44s %5d pairs' % ('Standardized variation sequences, U+FE00..FE0D', len(variants)))
    w('//   %-44s %5d pairs' % ('Simple case folding (status C and S)', len(folding)))
    w('//   %-44s %5d' % ('Runs of the packed properties', len(runs)))
    w('')
    w('#include <fext/unicode_data.h>')
    w('')
    w('#include <algorithm>')
    w('#include <iterator>')
    w('')
    w('namespace fext {')
    w('namespace unicode {')
    w('')
    w('const char* const UNICODE_VERSION = "%s";' % version)
    w('')
    w('const SourceFile SOURCE_FILES[] = {')
    for name in FILES:
        w('    {"%s", "%s"},' % (name, reader.hashes[name]))
    w('};')
    w('const size_t SOURCE_FILE_COUNT = sizeof(SOURCE_FILES) / sizeof(SOURCE_FILES[0]);')
    w('')
    w('namespace {')
    w('')
    w('struct Run {')
    w('    uint32_t first;')
    w('    uint32_t bits;')
    w('};')
    w('')
    w('//! The packed properties of every code point, as runs: an entry holds the first code point of a run and the')
    w('//! properties of every code point up to the next entry\'s first. The Hangul syllables are all LV here.')
    w('const Run RUNS[] = {')
    for first, bits in runs:
        w('    {0x%06x, 0x%06x},' % (first, bits))
    w('};')
    w('')
    w('//! The packed properties of U+0000 to U+007F, for a lookup without a search: most messages are mostly ASCII.')
    w('const uint32_t ASCII_BITS[128] = {')
    for first in range(0, 128, 8):
        w('    ' + ' '.join('0x%06x,' % values[c] for c in range(first, first + 8)))
    w('};')
    w('')
    w('struct Pair {')
    w('    uint32_t first;')
    w('    uint32_t second;')
    w('};')
    w('')
    w('bool operator<(const Pair& a, const Pair& b) { return a.first < b.first || (a.first == b.first && a.second < b.second); }')
    w('')
    w('//! The standardized variation sequences of U+FE00 to U+FE0D (StandardizedVariants.txt): base and selector.')
    w('const Pair STANDARDIZED_VARIANTS[] = {')
    for base, selector in sorted(variants):
        w('    {0x%05x, 0x%04x},' % (base, selector))
    w('};')
    w('')
    w('//! Simple case folding (CaseFolding.txt, status C and S): code point and its folding.')
    w('const Pair CASE_FOLDING[] = {')
    for source in sorted(folding):
        w('    {0x%05x, 0x%05x},' % (source, folding[source]))
    w('};')
    w('')
    w('} // namespace')
    w('')
    w('Properties GetProperties(uint32_t cp)')
    w('{')
    w('    if (cp < 0x80) return Properties{ASCII_BITS[cp]};')
    w('    if (cp > MAX_CODE_POINT) return Properties{static_cast<uint32_t>(GraphemeBreak::CONTROL)};')
    w('    const Run* run = std::upper_bound(std::begin(RUNS), std::end(RUNS), cp, [](uint32_t c, const Run& r) { return c < r.first; }) - 1;')
    w('    uint32_t bits = run->bits;')
    w('    if ((bits & GRAPHEME_BREAK_MASK) == static_cast<uint32_t>(GraphemeBreak::LV) && (cp - HANGUL_FIRST) % HANGUL_T_COUNT != 0) {')
    w('        bits = (bits & ~GRAPHEME_BREAK_MASK) | static_cast<uint32_t>(GraphemeBreak::LVT);')
    w('    }')
    w('    return Properties{bits};')
    w('}')
    w('')
    w('bool IsStandardizedVariant(uint32_t base, uint32_t selector)')
    w('{')
    w('    return std::binary_search(std::begin(STANDARDIZED_VARIANTS), std::end(STANDARDIZED_VARIANTS), Pair{base, selector});')
    w('}')
    w('')
    w('uint32_t SimpleCaseFold(uint32_t cp)')
    w('{')
    w('    const Pair* found = std::lower_bound(std::begin(CASE_FOLDING), std::end(CASE_FOLDING), Pair{cp, 0});')
    w('    return found != std::end(CASE_FOLDING) && found->first == cp ? found->second : cp;')
    w('}')
    w('')
    w('} // namespace unicode')
    w('} // namespace fext')

    with open(os.path.join(top, OUTPUT), 'w', encoding='utf8', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    shutil.copyfile(os.path.join(args.ucd, 'auxiliary/GraphemeBreakTest.txt'), os.path.join(top, TEST_DATA))
    print('Wrote %s (%d runs) and %s' % (OUTPUT, len(runs), TEST_DATA))


if __name__ == '__main__':
    main()
