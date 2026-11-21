#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check with nm which built programs contain the OP_RETURN record:
#
#  - ferrited, ferrite-qt, ferrite-node, ferrite-gui, ferrite-cli, ferrite-tx and ferrite-wallet have no symbol
#    whose name contains "fext" in any case;
#  - ferrite-cli, ferrite-tx and ferrite-wallet have no symbol of the namespace opreturn;
#  - ferrited, ferrite-qt, ferrite-node and ferrite-gui have symbols of the namespace opreturn exactly when
#    src/init.cpp makes the record (opreturn::Construct). Until it does, nothing in them refers to the record and
#    the linker leaves its code out.
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

EXIT_CODE=0
CHECKED=0

# check <program> <whether opreturn symbols are expected: 1, 0>
check() {
    local program=$1 expected=$2 path="${BUILD}/src/$1" symbols fext record
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
    if [ "${fext}" != 0 ]; then
        echo "${program}: ${fext} symbols contain \"fext\":"
        grep -i 'fext' <<< "${symbols}" | head -20
        EXIT_CODE=1
    fi
    if [ "${expected}" = 1 ] && [ "${record}" = 0 ]; then
        echo "${program}: no symbol of the namespace opreturn, although src/init.cpp makes the record"
        EXIT_CODE=1
    elif [ "${expected}" = 0 ] && [ "${record}" != 0 ]; then
        echo "${program}: ${record} symbols of the namespace opreturn:"
        grep 'opreturn::' <<< "${symbols}" | head -20
        EXIT_CODE=1
    else
        echo "${program}: ${record} symbols of the namespace opreturn, none containing \"fext\": as expected"
    fi
}

for program in ferrited qt/ferrite-qt ferrite-node qt/ferrite-gui; do
    check "${program}" "${RECORD_LINKED}"
done
for program in ferrite-cli ferrite-tx ferrite-wallet; do
    check "${program}" 0
done

if [ "${CHECKED}" = 0 ]; then
    echo "No program found under ${BUILD}/src"
    EXIT_CODE=1
fi
exit ${EXIT_CODE}
