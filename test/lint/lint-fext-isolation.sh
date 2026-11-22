#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the OP_RETURN record stays apart from the rest of the node, and the FEXT GUI from the plain programs:
#
#  - Only the record's own files, the FEXT GUI's files and their tests include a header of src/opreturn/ or name
#    the namespace opreturn. The one exception is src/init.cpp, which may hold exactly these lines, each at most
#    once: the include of opreturn/node.h, the call that registers the record's options, and the call that makes
#    the record.
#  - src/Makefile.am lists the record's files only among the node's headers (BITCOIN_CORE_H) and the sources of
#    the server library (libbitcoin_server_a_SOURCES).
#  - Only the FEXT GUI's files and their tests include a header of src/fext/ or src/qt/fext/ or name the namespace
#    fext.
#  - The GUI's extension points (qt/guiextension.h, g_gui_extension) are named only by their own files, the FEXT
#    GUI, its tests and the check of the plain GUI that no extension is set. The four files of the plain GUI that
#    call them hold exactly the lines listed below, each once: the include and the calls.
#  - Without those lines, and with the two lines that two of the calls replace put back, each of the four files is
#    the file of the plain GUI, whose git blob this lint records; and the call made after the node's base
#    initialization comes right after that initialization succeeds and right before the node is started. So any
#    other change to these files, which ferrite-qt is built from, fails until its blob is recorded here on purpose.
#  - The GUI's makefiles name a file or target of the FEXT GUI only in variables and rules of their own (a name that
#    contains "fext"), in the lists of programs, libraries, tests and files to clean, and in conditions; the one
#    exception is the FEXT letter icon, which the plain GUI holds among its icons. So no file of the FEXT GUI goes
#    into a plain program or library.
#
# The include and name checks read the C++ sources and headers and the Objective-C++ sources (*.mm) of the macOS GUI.
#
# Given a commit range (FEXT_ISOLATION_RANGE='<base>..<tip>', or a base commit as the argument, checked up to
# HEAD), it also checks that the range adds only the files of the record and of the FEXT GUI and changes no
# existing file but the ones they need, that src/init.cpp gains and loses only the lines above, and that the four
# files with the GUI's calls gain only those lines and lose only the two lines that two of them replace. The base
# is the commit before the record's first one: a range that starts later finds the record's own files changed, not
# added, and fails. The range is not read from COMMIT_RANGE, which the tree's other lints take as the commits of any
# change.

export LC_ALL=C

cd "$(dirname "$0")/../.." || exit 1

EXIT_CODE=0

# Files that may include the record's headers and name its namespace, as git grep prints them ("path:line:").
OWN_FILES='^src/(opreturn/[^:]+|rpc/opreturn\.(h|cpp)|interfaces/opreturn\.h|fext/[^:]+|qt/fext/[^:]+|test/opreturn_[a-z0-9_]+\.cpp|test/util/opreturn\.(h|cpp)|test/fuzz/opreturn_[a-z0-9_]+\.cpp|bench/opreturn_[a-z0-9_]+\.cpp|qt/test/fext[a-z0-9_]*\.(h|cpp)):'

# Files that may include the FEXT GUI's headers and name its namespace, as git grep prints them.
FEXT_OWN_FILES='^src/(fext/[^:]+|qt/fext/[^:]+|test/fext_[a-z0-9_]+\.cpp|test/util/fext[a-z0-9_]*\.(h|cpp)|test/fuzz/fext_[a-z0-9_]+\.cpp|bench/fext_[a-z0-9_]+\.cpp|qt/test/fext[a-z0-9_]*\.(h|cpp)):'

# Files that may name the GUI's extension points freely, as git grep prints them.
EXTENSION_OWN_FILES='^src/(qt/guiextension\.(h|cpp)|qt/fext/[^:]+|qt/test/fext[a-z0-9_]*\.(h|cpp)):'

# The lines src/init.cpp may hold, without their indentation.
INIT_LINES=(
    "#include <opreturn/node.h>"
    "opreturn::AddArgs(argsman);"
    "return opreturn::Construct(node);"
)

# The files of the plain GUI that call the extension points, and the lines each holds, without their indentation.
HOOK_FILES=(src/qt/bitcoin.cpp src/qt/bitcoingui.cpp src/qt/utilitydialog.cpp src/qt/sendcoinsdialog.cpp)
# The call after the node's base initialization, and the lines right before and after it.
ATTACH_NODE_LINE="if (g_gui_extension) g_gui_extension->attachNode(node_context);"
ATTACH_NODE_BEFORE="if (app.baseInitialize()) {"
ATTACH_NODE_AFTER="app.requestInitialize();"
hook_lines() {
    echo "#include <qt/guiextension.h>"
    case "$1" in
        src/qt/bitcoin.cpp)
            echo "${ATTACH_NODE_LINE}"
            ;;
        src/qt/bitcoingui.cpp)
            echo "setCentralWidget(g_gui_extension ? g_gui_extension->wrapCentralWidget(*this, *walletFrame) : walletFrame);"
            echo "if (g_gui_extension) g_gui_extension->addTabActions(*this, *tabGroup);"
            echo "if (g_gui_extension) g_gui_extension->addToolBarActions(*this, *toolbar);"
            echo "if (g_gui_extension) window_title = g_gui_extension->windowTitle(window_title);"
            ;;
        src/qt/utilitydialog.cpp)
            echo "if (g_gui_extension) header = g_gui_extension->usageHeader(header);"
            ;;
        src/qt/sendcoinsdialog.cpp)
            echo "if (g_gui_extension) g_gui_extension->extendSendPage(*this, *ui->verticalLayout, ui->verticalLayout->indexOf(ui->frameFee));"
            echo "prepareStatus = g_gui_extension ? g_gui_extension->prepareSend(*this, *model, *m_current_transaction, *m_coin_control) : model->prepareTransaction(*m_current_transaction, *m_coin_control);"
            echo "if (g_gui_extension) g_gui_extension->describeSend(*this, *m_current_transaction, question_string, informative_text, detailed_text);"
            echo "if (g_gui_extension) g_gui_extension->clearSendPage(*this);"
            ;;
    esac
}
# The line of a hook file that a call replaces, given the call (empty for the calls that replace nothing).
replaced_line() {
    case "$1" in
        "setCentralWidget(g_gui_extension ? "*) echo "setCentralWidget(walletFrame);" ;;
        "prepareStatus = g_gui_extension ? "*) echo "prepareStatus = model->prepareTransaction(*m_current_transaction, *m_coin_control);" ;;
    esac
}

# The git blob of each of the four files without the lines above and with the lines two of them replace put back
# (plain_file): the file the plain GUI had before the extension points were added. Whoever changes another line of
# one of these files changes ferrite-qt, and records the file's new blob here on purpose.
plain_blob() {
    case "$1" in
        src/qt/bitcoin.cpp) echo 9089550cd432bbf8b40c05a1c4074f92b1798519 ;;
        src/qt/bitcoingui.cpp) echo f10a45a9a84079e806d20fe24f8e4058c51776af ;;
        src/qt/utilitydialog.cpp) echo 92dfcb57af39eab97b940ba2dc725848ffb0bf7e ;;
        src/qt/sendcoinsdialog.cpp) echo 3c97f37f7b8a338974bfded54fe5976754ec30d7 ;;
    esac
}

# The lines of the check of the plain GUI (src/qt/test/apptests.cpp) that name the extension points.
APPTESTS_LINES=(
    "#include <qt/guiextension.h>"
    "QVERIFY(g_gui_extension == nullptr);"
)

# The pattern of a mention of the extension points.
EXTENSION_PATTERN='g_gui_extension|GuiExtension|qt/guiextension\.h'

# New files a commit range may add: the record's, then the FEXT GUI's.
NEW_FILES='^(src/opreturn/[a-z0-9_]+\.(h|cpp)|src/rpc/opreturn\.(h|cpp)|src/interfaces/opreturn\.h|src/test/opreturn_[a-z0-9_]+\.cpp|src/test/util/opreturn\.(h|cpp)|src/test/fuzz/opreturn_[a-z0-9_]+\.cpp|src/test/data/opreturn_[a-z0-9_.]+|src/bench/opreturn_[a-z0-9_]+\.cpp|test/functional/(feature|rpc)_opreturn[a-z0-9_]*\.py|test/lint/[^ ]*fext[^ ]*|contrib/devtools/[^ ]*fext[^ ]*|contrib/devtools/gen-opreturn-[a-z0-9-]+\.py|doc/[^ ]+'
NEW_FILES+='|src/qt/guiextension\.(h|cpp)|src/fext/[^ ]+|src/qt/fext/[^ ]+|src/qt/forms/fext[a-z0-9_]*\.ui|src/qt/res/ferrite-qt-fext-res\.rc|src/qt/test/fext[a-z0-9_]*\.(h|cpp)|src/test/fext_[a-z0-9_]+\.cpp|src/test/util/fext[a-z0-9_]*\.(h|cpp)|src/test/fuzz/fext_[a-z0-9_]+\.cpp|src/test/data/fext_[^ ]+|src/bench/fext_[a-z0-9_]+\.cpp|test/functional/feature_fext[a-z0-9_]*\.py)$'

# Existing files a commit range may change: the record's, then the FEXT GUI's (the build files and .gitignore, the
# four files with the GUI's calls, the FEXT letter icon, the files that name OP_RETURN outputs in the transaction
# list and the English catalogue that holds the plural of that name, and the tests of the plain GUI).
CHANGED_FILES='^(src/init\.cpp|src/rpc/client\.cpp|src/Makefile\.am|src/Makefile\.test\.include|src/Makefile\.test_util\.include|src/Makefile\.bench\.include|test/lint/check-rpc-mappings\.py|test/functional/test_runner\.py|\.github/workflows/ci\.yml|doc/[^ ]+'
CHANGED_FILES+='|configure\.ac|Makefile\.am|\.gitignore|src/Makefile\.qt\.include|src/Makefile\.qttest\.include|share/setup\.nsi\.in|contrib/devtools/gen-manpages\.sh|src/qt/(bitcoin|bitcoingui|utilitydialog|sendcoinsdialog)\.cpp|src/qt/res/icons/fext_letter\.png|src/qt/transactiontablemodel\.(h|cpp)|src/qt/walletview\.cpp|src/qt/locale/bitcoin_en\.ts|src/qt/test/(apptests|rpcnestedtests)\.cpp|src/qt/test/wallettests\.(h|cpp))$'

trim() {
    sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
}

is_init_line() {
    local line
    for line in "${INIT_LINES[@]}"; do
        [ "$1" = "$line" ] && return 0
    done
    return 1
}

# is_hook_line <file> <line>: whether line is one of the lines of hook_lines <file>.
is_hook_line() {
    grep -q -x -F -e "$2" <<< "$(hook_lines "$1")"
}

# plain_file <file>: the file without its lines of hook_lines, each line a call replaces put back in its place with
# the call's indentation.
plain_file() {
    local line pairs=""
    while IFS= read -r line; do
        pairs+="${line}"$'\t'"$(replaced_line "${line}")"$'\n'
    done < <(hook_lines "$1")
    PAIRS=${pairs} awk '
        BEGIN {
            n = split(ENVIRON["PAIRS"], rows, "\n")
            for (i = 1; i <= n; i++) {
                tab = index(rows[i], "\t")
                if (tab > 1) replacement[substr(rows[i], 1, tab - 1)] = substr(rows[i], tab + 1)
            }
        }
        {
            text = $0
            sub(/^[ \t]+/, "", text)
            sub(/[ \t]+$/, "", text)
            if (text in replacement) {
                if (replacement[text] != "") {
                    indent = $0
                    sub(/[^ \t].*$/, "", indent)
                    print indent replacement[text]
                }
                next
            }
            print
        }' "$1"
}

# 1. Includes of the record's headers.
OUTPUT=$(git grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]opreturn/' -- '*.cpp' '*.h' '*.mm' | grep -v -E "${OWN_FILES}" | grep -v -E '^src/init\.cpp:[0-9]+:#include <opreturn/node\.h>$')
if [ -n "${OUTPUT}" ]; then
    echo "Only the OP_RETURN record's own files may include its headers (src/init.cpp only opreturn/node.h):"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 2. Uses of the namespace outside the record's files.
OUTPUT=$(git grep -n -E '(^|[^A-Za-z0-9_])opreturn::|namespace[[:space:]]+opreturn([^A-Za-z0-9_]|$)' -- '*.cpp' '*.h' '*.mm' | grep -v -E "${OWN_FILES}" | grep -v -E '^src/init\.cpp:')
if [ -n "${OUTPUT}" ]; then
    echo "Only the OP_RETURN record's own files and src/init.cpp may name the namespace opreturn:"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 3. src/init.cpp holds at most the three lines, each at most once.
SEEN=""
while IFS= read -r numbered; do
    line=$(printf '%s\n' "${numbered#*:}" | trim)
    if ! is_init_line "${line}"; then
        echo "src/init.cpp may mention the OP_RETURN record only in the lines this lint lists; found line ${numbered%%:*}: ${line}"
        EXIT_CODE=1
    elif grep -q -x -F -e "${line}" <<< "${SEEN}"; then
        echo "src/init.cpp holds this line more than once: ${line}"
        EXIT_CODE=1
    fi
    SEEN="${SEEN}${line}"$'\n'
done < <(grep -n -i 'opreturn' src/init.cpp)

# 4. The build lists the record's files among the node's headers and the server library's sources only.
OUTPUT=$(awk '
    {
        if (!continued) {
            variable = ""
            if (match($0, /^[A-Za-z_][A-Za-z0-9_]*[ \t]*(\+=|:=|=)/)) {
                variable = $0
                sub(/[ \t]*(\+=|:=|=).*$/, "", variable)
            }
        }
        if ($0 ~ /opreturn/) {
            entry = $0
            sub(/^[ \t]+/, "", entry)
            sub(/[ \t]*\\$/, "", entry)
            header = entry ~ /^(opreturn\/[a-z0-9_]+|rpc\/opreturn|interfaces\/opreturn)\.h$/
            source = entry ~ /^(opreturn\/[a-z0-9_]+|rpc\/opreturn)\.cpp$/
            if (!((variable == "BITCOIN_CORE_H" && header) || (variable == "libbitcoin_server_a_SOURCES" && source))) {
                print FILENAME ":" FNR ": " $0
            }
        }
        continued = ($0 ~ /\\$/)
    }' src/Makefile.am)
if [ -n "${OUTPUT}" ]; then
    echo "src/Makefile.am may list the OP_RETURN record's headers only in BITCOIN_CORE_H and its sources only in libbitcoin_server_a_SOURCES:"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 5. Includes of the FEXT GUI's headers and uses of its namespace outside its files.
OUTPUT=$(git grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](qt/)?fext/|(^|[^A-Za-z0-9_])fext::|namespace[[:space:]]+fext([^A-Za-z0-9_]|$)' -- '*.cpp' '*.h' '*.mm' | grep -v -E "${FEXT_OWN_FILES}")
if [ -n "${OUTPUT}" ]; then
    echo "Only the FEXT GUI's own files and their tests may include its headers or name the namespace fext:"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 6. The extension points are named only where they may be.
OUTPUT=$(git grep -n -E "${EXTENSION_PATTERN}" -- '*.cpp' '*.h' '*.mm' | grep -v -E "${EXTENSION_OWN_FILES}" | while IFS= read -r numbered; do
    file=${numbered%%:*}
    rest=${numbered#*:}
    line=$(printf '%s\n' "${rest#*:}" | trim)
    case "${file}" in
        src/qt/bitcoin.cpp|src/qt/bitcoingui.cpp|src/qt/utilitydialog.cpp|src/qt/sendcoinsdialog.cpp) continue ;;
        src/qt/test/apptests.cpp)
            grep -q -x -F -e "${line}" <<< "$(printf '%s\n' "${APPTESTS_LINES[@]}")" && continue
            ;;
    esac
    echo "${numbered}"
done)
if [ -n "${OUTPUT}" ]; then
    echo "Only the GUI's extension points themselves, the four files that call them, the FEXT GUI, its tests and the lines of src/qt/test/apptests.cpp this lint lists may name them:"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 7. Each of the four files with the GUI's calls holds exactly its lines, each once.
for file in "${HOOK_FILES[@]}"; do
    FOUND=$(grep -n -E "${EXTENSION_PATTERN}" "${file}")
    while IFS= read -r numbered; do
        [ -z "${numbered}" ] && continue
        line=$(printf '%s\n' "${numbered#*:}" | trim)
        if ! is_hook_line "${file}" "${line}"; then
            echo "${file} may name the GUI's extension points only in the lines this lint lists; found line ${numbered%%:*}: ${line}"
            EXIT_CODE=1
        fi
    done <<< "${FOUND}"
    while IFS= read -r expected; do
        count=$(printf '%s\n' "${FOUND}" | sed 's/^[0-9]*://' | trim | grep -c -x -F -e "${expected}")
        if [ "${count}" != 1 ]; then
            echo "${file} must hold this line once, not ${count} times: ${expected}"
            EXIT_CODE=1
        fi
    done < <(hook_lines "${file}")
done

# 8. Without their lines, each of the four files is the plain GUI's file, and the call after the node's base
#    initialization is in its place.
for file in "${HOOK_FILES[@]}"; do
    blob=$(plain_file "${file}" | git hash-object --stdin)
    if [ "${blob}" != "$(plain_blob "${file}")" ]; then
        echo "${file} without the GUI's calls is not the file of the plain GUI (its git blob is ${blob}, not $(plain_blob "${file}")): only the lines this lint lists may differ from it"
        EXIT_CODE=1
    fi
done
OUTPUT=$(awk -v line="${ATTACH_NODE_LINE}" -v before="${ATTACH_NODE_BEFORE}" -v after="${ATTACH_NODE_AFTER}" '
    {
        text[NR] = $0
        sub(/^[ \t]+/, "", text[NR])
        sub(/[ \t]+$/, "", text[NR])
    }
    END {
        for (i = 1; i <= NR; i++) {
            if (text[i] == line && !(text[i - 1] == before && text[i + 1] == after)) print i
        }
    }' src/qt/bitcoin.cpp)
if [ -n "${OUTPUT}" ]; then
    echo "src/qt/bitcoin.cpp must make this call right after \"${ATTACH_NODE_BEFORE}\" and right before \"${ATTACH_NODE_AFTER}\"; found it at line ${OUTPUT}: ${ATTACH_NODE_LINE}"
    EXIT_CODE=1
fi

# 9. The GUI's makefiles put no file of the FEXT GUI into a plain program or library.
OUTPUT=$(awk '
    function fext_only(names,    n, i, parts) {
        n = split(names, parts, /[ \t]+/)
        for (i = 1; i <= n; i++) {
            if (parts[i] != "" && tolower(parts[i]) !~ /fext/) return 0
        }
        return 1
    }
    FNR == 1 { continued = 0 }
    {
        if (!continued) {
            if ($0 ~ /^\t/) {
                # A recipe line belongs to the rule above it.
            } else if (match($0, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*(\+=|:=|\?=|=)/)) {
                kind = "variable"
                name = $0
                sub(/^[ \t]*/, "", name)
                sub(/[ \t]*(\+=|:=|\?=|=).*$/, "", name)
            } else if ($0 ~ /^[ \t]*(if|else|endif)([ \t]|$)/) {
                kind = "condition"
            } else if ($0 ~ /^[^#\t][^=]*:/) {
                kind = "rule"
                name = $0
                sub(/:.*$/, "", name)
            } else {
                kind = "other"
                name = ""
            }
        }
        if (tolower($0) ~ /fext/ && $0 !~ /^[ \t]*#/) {
            allowed = kind == "condition"
            if (kind == "variable") {
                allowed = tolower(name) ~ /fext/ || name ~ /^(bin_PROGRAMS|EXTRA_LIBRARIES|TESTS|CLEANFILES)$/ || (name == "RES_ICONS" && $0 ~ /^[ \t]*qt\/res\/icons\/fext_letter\.png[ \t]*\\?$/)
            } else if (kind == "rule") {
                allowed = fext_only(name)
            }
            if (!allowed) print FILENAME ":" FNR ": " $0
        }
        continued = ($0 ~ /\\$/)
    }' src/Makefile.am src/Makefile.qt.include src/Makefile.qttest.include)
if [ -n "${OUTPUT}" ]; then
    echo "The makefiles may name the FEXT GUI only in its own variables and rules, the lists of programs, libraries, tests and files to clean, and conditions:"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 10. With a commit range: the files it adds and changes, and the lines of src/init.cpp and of the files with the
#     GUI's calls.
RANGE=${FEXT_ISOLATION_RANGE:-}
if [ -z "${RANGE}" ] && [ -n "${1:-}" ]; then
    RANGE="$1..HEAD"
fi
if [ -z "${RANGE}" ]; then
    echo "lint-fext-isolation.sh: no commit range given, so the files a change touches were not checked (set FEXT_ISOLATION_RANGE or pass a base commit)."
    exit ${EXIT_CODE}
fi
if ! CHANGES=$(git diff --no-renames --name-status "${RANGE}"); then
    echo "lint-fext-isolation.sh: cannot list the changes of ${RANGE}"
    exit 1
fi
while IFS=$'\t' read -r status path; do
    [ -z "${status}" ] && continue
    if [ "${status}" = "A" ]; then
        if ! grep -q -E "${NEW_FILES}" <<< "${path}" && ! grep -q -E "${CHANGED_FILES}" <<< "${path}"; then
            echo "${RANGE} adds a file that is not one of the OP_RETURN record's or the FEXT GUI's: ${path}"
            EXIT_CODE=1
        fi
    elif ! grep -q -E "${CHANGED_FILES}" <<< "${path}"; then
        echo "${RANGE} changes or removes a file the OP_RETURN record and the FEXT GUI may not change: ${path} (${status})"
        EXIT_CODE=1
    fi
done <<< "${CHANGES}"
ADDED_CONSTRUCT=0
REMOVED_RETURN=0
while IFS= read -r line; do
    sign=${line:0:1}
    text=$(printf '%s\n' "${line:1}" | trim)
    # Blank lines change nothing.
    [ -z "${text}" ] && continue
    if [ "${sign}" = "+" ]; then
        if ! is_init_line "${text}"; then
            echo "${RANGE} adds a line to src/init.cpp that is not one of the expected ones: ${text}"
            EXIT_CODE=1
        fi
        [ "${text}" = "return opreturn::Construct(node);" ] && ADDED_CONSTRUCT=$((ADDED_CONSTRUCT + 1))
    else
        if [ "${text}" != "return true;" ]; then
            echo "${RANGE} removes a line from src/init.cpp other than the one the record replaces: ${text}"
            EXIT_CODE=1
        fi
        REMOVED_RETURN=$((REMOVED_RETURN + 1))
    fi
done < <(git diff -U0 "${RANGE}" -- src/init.cpp | grep -E '^[+-]' | grep -v -E '^(\+\+\+|---)( |$)')
if [ "${ADDED_CONSTRUCT}" != "${REMOVED_RETURN}" ]; then
    echo "${RANGE}: src/init.cpp must replace one \"return true;\" with the call that makes the record, and nothing else"
    EXIT_CODE=1
fi
for file in "${HOOK_FILES[@]}"; do
    ADDED=""
    REMOVED=""
    while IFS= read -r line; do
        sign=${line:0:1}
        text=$(printf '%s\n' "${line:1}" | trim)
        [ -z "${text}" ] && continue
        if [ "${sign}" = "+" ]; then
            if ! is_hook_line "${file}" "${text}"; then
                echo "${RANGE} adds a line to ${file} that is not one of the GUI's calls: ${text}"
                EXIT_CODE=1
            fi
            ADDED="${ADDED}${text}"$'\n'
        else
            REMOVED="${REMOVED}${text}"$'\n'
        fi
    done < <(git diff -U0 "${RANGE}" -- "${file}" | grep -E '^[+-]' | grep -v -E '^(\+\+\+|---)( |$)')
    # Each removed line must be one that an added call replaces, as often as that call is added.
    EXPECTED_REMOVED=$(printf '%s' "${ADDED}" | while IFS= read -r text; do replaced_line "${text}"; done | sort)
    if [ "$(printf '%s' "${REMOVED}" | sort)" != "${EXPECTED_REMOVED}" ]; then
        echo "${RANGE} removes lines from ${file} other than those the GUI's calls replace:"
        diff <(printf '%s\n' "${EXPECTED_REMOVED}") <(printf '%s' "${REMOVED}" | sort) | grep '^[<>]'
        EXIT_CODE=1
    fi
done

exit ${EXIT_CODE}
