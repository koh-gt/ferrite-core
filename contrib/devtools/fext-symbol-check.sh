#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check with nm which built programs contain the OP_RETURN record and the code of the FEXT GUI:
#
#  - ferrited, ferrite-qt, ferrite-node, ferrite-gui, ferrite-cli, ferrite-tx and ferrite-wallet have no symbol
#    whose name contains "fext" in any case;
#  - ferrite-qt-fext has the symbols of its GUI extension (FextExtension), and symbols of the namespace fext exactly
#    when it links the FEXT node library (src/Makefile.qt.include adds $(LIBFEXT_NODE) to its LDADD); until then it
#    holds only the FEXT GUI's own classes;
#  - ferrite-cli, ferrite-tx and ferrite-wallet have no symbol of the namespace opreturn;
#  - ferrited, ferrite-qt, ferrite-qt-fext, ferrite-node and ferrite-gui have symbols of the namespace opreturn
#    exactly when src/init.cpp makes the record (opreturn::Construct). Until it does, nothing in them refers to the
#    record and the linker leaves its code out.
#
# Usage: contrib/devtools/fext-symbol-check.sh [<build directory>]
# The build directory defaults to the top of the source tree. A program that was not built is reported and
# skipped; a program without a symbol table fails the check.

export LC_ALL=C

TOP=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-${TOP}}
NM=${NM:-nm}

if grep -q -F 'opreturn::Construct(' "${TOP}/src/init.cpp"; then
    RECORD_LINKED=1
else
    RECORD_LINKED=0
fi
if grep -q -E '^qt_ferrite_qt_fext_LDADD[[:space:]]*\+?=.*\$\(LIBFEXT_NODE\)' "${TOP}/src/Makefile.qt.include"; then
    FEXT_NODE_LINKED=1
else
    FEXT_NODE_LINKED=0
fi

EXIT_CODE=0
CHECKED=0

# check <program> <whether opreturn symbols are expected: 1, 0> <whether it is the FEXT GUI: 1, 0>
check() {
    local program=$1 expected=$2 fext_gui=$3 path="${BUILD}/src/$1" symbols fext extension fext_namespace record failed=0
    if [ ! -f "${path}" ]; then
        echo "${program}: not built, skipped"
        return
    fi
    if ! symbols=$("${NM}" -C "${path}" 2>/dev/null) || [ -z "${symbols}" ]; then
        echo "${program}: no symbol table, so it cannot be checked"
        EXIT_CODE=1
        return
    fi
    CHECKED=$((CHECKED + 1))
    fext=$(grep -c -i 'fext' <<< "${symbols}")
    record=$(grep -c 'opreturn::' <<< "${symbols}")
    if [ "${fext_gui}" = 1 ]; then
        extension=$(grep -c 'FextExtension' <<< "${symbols}")
        fext_namespace=$(grep -c -E '(^|[^A-Za-z0-9_])fext::' <<< "${symbols}")
        if [ "${extension}" = 0 ]; then
            echo "${program}: no symbol of FextExtension, the FEXT GUI's extension"
            failed=1
        fi
        if [ "${FEXT_NODE_LINKED}" = 1 ] && [ "${fext_namespace}" = 0 ]; then
            echo "${program}: no symbol of the namespace fext, although it links the FEXT node library"
            failed=1
        elif [ "${FEXT_NODE_LINKED}" = 0 ] && [ "${fext_namespace}" != 0 ]; then
            echo "${program}: ${fext_namespace} symbols of the namespace fext, although it does not link the FEXT node library:"
            grep -E '(^|[^A-Za-z0-9_])fext::' <<< "${symbols}" | head -20
            failed=1
        fi
    elif [ "${fext}" != 0 ]; then
        echo "${program}: ${fext} symbols contain \"fext\":"
        grep -i 'fext' <<< "${symbols}" | head -20
        failed=1
    fi
    if [ "${expected}" = 1 ] && [ "${record}" = 0 ]; then
        echo "${program}: no symbol of the namespace opreturn, although src/init.cpp makes the record"
        failed=1
    elif [ "${expected}" = 0 ] && [ "${record}" != 0 ]; then
        echo "${program}: ${record} symbols of the namespace opreturn:"
        grep 'opreturn::' <<< "${symbols}" | head -20
        failed=1
    fi
    if [ "${failed}" = 1 ]; then
        EXIT_CODE=1
    elif [ "${fext_gui}" = 1 ]; then
        echo "${program}: ${record} symbols of the namespace opreturn, ${extension} of FextExtension, ${fext_namespace} of the namespace fext: as expected"
    else
        echo "${program}: ${record} symbols of the namespace opreturn, none containing \"fext\": as expected"
    fi
}

for program in ferrited qt/ferrite-qt ferrite-node qt/ferrite-gui; do
    check "${program}" "${RECORD_LINKED}" 0
done
check qt/ferrite-qt-fext "${RECORD_LINKED}" 1
for program in ferrite-cli ferrite-tx ferrite-wallet; do
    check "${program}" 0 0
done

if [ "${CHECKED}" = 0 ]; then
    echo "No program found under ${BUILD}/src"
    EXIT_CODE=1
fi
exit ${EXIT_CODE}
