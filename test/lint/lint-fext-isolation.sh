#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the OP_RETURN record stays apart from the rest of the node:
#
#  - Only the record's own files, the FEXT GUI's files and their tests include a header of src/opreturn/ or name
#    the namespace opreturn. The one exception is src/init.cpp, which may hold exactly these lines, each at most
#    once: the include of opreturn/node.h, the call that registers the record's options, and the call that makes
#    the record.
#  - src/Makefile.am lists the record's files only among the node's headers (BITCOIN_CORE_H) and the sources of
#    the server library (libbitcoin_server_a_SOURCES).
#
# Given a commit range (FEXT_ISOLATION_RANGE='<base>..<tip>', or a base commit as the argument, checked up to
# HEAD), it also checks that the range adds only the record's files and changes no existing file but the ones the
# record needs, and that src/init.cpp gains and loses only the lines above. The base is the commit before the
# record's first one: a range that starts later finds the record's own files changed, not added, and fails. The
# range is not read from COMMIT_RANGE, which the tree's other lints take as the commits of any change.

export LC_ALL=C

cd "$(dirname "$0")/../.." || exit 1

EXIT_CODE=0

# Files that may include the record's headers and name its namespace, as git grep prints them ("path:line:").
OWN_FILES='^src/(opreturn/[^:]+|rpc/opreturn\.(h|cpp)|interfaces/opreturn\.h|fext/[^:]+|qt/fext/[^:]+|test/opreturn_[a-z0-9_]+\.cpp|test/util/opreturn\.(h|cpp)|test/fuzz/opreturn_[a-z0-9_]+\.cpp|bench/opreturn_[a-z0-9_]+\.cpp|qt/test/fext[a-z0-9_]*\.(h|cpp)):'

# The lines src/init.cpp may hold, without their indentation.
INIT_LINES=(
    "#include <opreturn/node.h>"
    "opreturn::AddArgs(argsman);"
    "return opreturn::Construct(node);"
)

# New files a commit range may add.
NEW_FILES='^(src/opreturn/[a-z0-9_]+\.(h|cpp)|src/rpc/opreturn\.(h|cpp)|src/interfaces/opreturn\.h|src/test/opreturn_[a-z0-9_]+\.cpp|src/test/util/opreturn\.(h|cpp)|src/test/fuzz/opreturn_[a-z0-9_]+\.cpp|src/test/data/opreturn_[a-z0-9_.]+|src/bench/opreturn_[a-z0-9_]+\.cpp|test/functional/(feature|rpc)_opreturn[a-z0-9_]*\.py|test/lint/[^ ]*fext[^ ]*|contrib/devtools/[^ ]*fext[^ ]*|contrib/devtools/gen-opreturn-[a-z0-9-]+\.py|doc/[^ ]+)$'

# Existing files a commit range may change.
CHANGED_FILES='^(src/init\.cpp|src/rpc/client\.cpp|src/Makefile\.am|src/Makefile\.test\.include|src/Makefile\.test_util\.include|src/Makefile\.bench\.include|test/lint/check-rpc-mappings\.py|test/functional/test_runner\.py|\.github/workflows/ci\.yml|doc/[^ ]+)$'

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

# 1. Includes of the record's headers.
OUTPUT=$(git grep -n -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]opreturn/' -- '*.cpp' '*.h' | grep -v -E "${OWN_FILES}" | grep -v -E '^src/init\.cpp:[0-9]+:#include <opreturn/node\.h>$')
if [ -n "${OUTPUT}" ]; then
    echo "Only the OP_RETURN record's own files may include its headers (src/init.cpp only opreturn/node.h):"
    echo "${OUTPUT}"
    echo
    EXIT_CODE=1
fi

# 2. Uses of the namespace outside the record's files.
OUTPUT=$(git grep -n -E '(^|[^A-Za-z0-9_])opreturn::|namespace[[:space:]]+opreturn([^A-Za-z0-9_]|$)' -- '*.cpp' '*.h' | grep -v -E "${OWN_FILES}" | grep -v -E '^src/init\.cpp:')
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

# 5. With a commit range: the files it adds and changes, and the lines of src/init.cpp.
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
            echo "${RANGE} adds a file that is not one of the OP_RETURN record's: ${path}"
            EXIT_CODE=1
        fi
    elif ! grep -q -E "${CHANGED_FILES}" <<< "${path}"; then
        echo "${RANGE} changes or removes a file the OP_RETURN record may not change: ${path} (${status})"
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

exit ${EXIT_CODE}
