#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the OP_RETURN record and the FEXT GUI change no object of the node's libraries, of the plain GUI's
# library and of the plain programs but the ones they must.
#
#   contrib/devtools/fext-identical-objects.sh <base> <tip> [<configure argument>...]
#       Exports <base> and then <tip> with git archive into one directory, builds each with the same configure
#       arguments, those of the CI build (without tests and without the GUI) without debug information, followed
#       by the ones given, which override them (give --with-gui=qt5 to compare the GUI's library too), and compares
#       their objects.
#   contrib/devtools/fext-identical-objects.sh --record <built tree> <list>
#       Writes every object of the node's libraries and every object of a plain program's own (its main, and on
#       Windows its resources) in a built tree, with its SHA-256, to <list>.
#   contrib/devtools/fext-identical-objects.sh --compare <base list> <tip list>
#       Compares two such lists.
#
# Every object of every library is the same byte for byte, except:
#   - clientversion.o, which holds the version of the commit it was built from;
#   - in libbitcoin_server.a, init.o, which may call the record, and the record's new objects, those built from
#     src/opreturn/;
#   - in libbitcoin_cli.a, client.o, which may list the record's calls for ferrite-cli;
#   - in qt/libbitcoinqt.a, the GUI's extension points (guiextension.o, new) and the four files that call them
#     (bitcoin.o, bitcoingui.o, utilitydialog.o, sendcoinsdialog.o); the transaction list and the notification of
#     new transactions, which name OP_RETURN outputs (transactiontablemodel.o, walletview.o), and the GUI's
#     translations, whose English catalogue holds the singular and plural of that name (qrc_bitcoin_locale.o); and
#     the GUI's resources, which hold the smaller FEXT letter icon (qrc_bitcoin.o).
# The plain programs' own objects (ferrited's, ferrite-cli's, ferrite-tx's, ferrite-wallet's and ferrite-qt's, and
# ferrite-node's and ferrite-gui's when they are built) are all the same. No other object may be added or missing
# from the tip, and no library or program may appear or disappear. The FEXT GUI's own libraries and program are not
# compared: no plain program links them.
#
# The builds run Qt's resource compiler with QT_RCC_SOURCE_DATE_OVERRIDE=1, so that a build with the GUI compares
# too: the compiler writes the time of each resource file into the GUI library, and git archive gives the files
# the time of their commit. They compile with -O2 -g0, as the CI build but without debug information, which the
# compiler keeps apart from the code: it records the line of each declaration an object uses, so an object would
# differ whenever a header it includes gains or loses a line, though its code does not.
#
# Environment: FEXT_IDOBJ_DIR, the directory to build in (default: a new temporary directory; it is kept, with the
# logs, the lists and each build's ferrited); FEXT_IDOBJ_JOBS, the number of make jobs (default: the processors).

export LC_ALL=C

TOP=$(cd "$(dirname "$0")/../.." && pwd)

# The node's libraries, relative to src/.
LIBRARIES=(
    libbitcoin_server.a libbitcoin_common.a libbitcoin_util.a libbitcoin_wallet.a libbitcoin_wallet_tool.a
    libbitcoin_consensus.a libbitcoin_zmq.a libbitcoin_cli.a libmw.a .libs/libbitcoinconsensus.a qt/libbitcoinqt.a
    leveldb/libleveldb.a leveldb/libleveldb_sse42.a leveldb/libmemenv.a crc32c/libcrc32c.a crc32c/libcrc32c_sse42.a
    univalue/.libs/libunivalue.a secp256k1-zkp/.libs/libsecp256k1.a
)

# The plain programs, each with the start of the names of the objects of its own sources (automake names them
# after the program), relative to the tree.
PROGRAMS=(
    "ferrited src/ferrited-" "ferrite-node src/ferrite_node-" "ferrite-cli src/ferrite_cli-" "ferrite-tx src/ferrite_tx-"
    "ferrite-wallet src/ferrite_wallet-" "qt/ferrite-qt src/qt/ferrite_qt-" "qt/ferrite-gui src/qt/ferrite_gui-"
)

usage() {
    sed -n '10,19p' "$0" | sed 's/^# \{0,3\}//'
    exit 2
}

# record <built tree> <list>: one line per object, sorted: "<library> <name> <sha256>" for the objects of the
# libraries and "<program> <file> <sha256>" for the plain programs' own. An archive keeps only the last part of an
# object's file name, and two objects of one library can share it (src/init.cpp and src/wallet/init.cpp both give
# libbitcoin_server_a-init.o), so an object is named by its file in the build when the build's files of that name
# are exactly the archive's objects of that name, and otherwise by its name and its place among them.
record() {
    local tree=$1 list=$2 unpacked library archive member count n file files in_archive on_disk libraries entry program
    local prefix
    unpacked=$(mktemp -d) || return 1
    : > "${list}.unsorted"
    for library in "${LIBRARIES[@]}" $(cd "${tree}/src" && ls crypto/libbitcoin_crypto_*.a 2>/dev/null); do
        archive="${tree}/src/${library}"
        [ -f "${archive}" ] || continue
        while read -r count member; do
            in_archive=()
            for n in $(seq 1 "${count}"); do
                # GNU ar extracts the n-th object of a name with xN.
                (cd "${unpacked}" && ar xN "${n}" "${archive}" "${member}") || { rm -rf "${unpacked}"; return 1; }
                in_archive+=("$(sha256sum "${unpacked}/${member}" | cut -d' ' -f1)")
                rm -f "${unpacked}/${member}"
            done
            files=$(cd "${tree}" && find src -name "${member}" -type f | sort)
            on_disk=()
            for file in ${files}; do
                on_disk+=("$(sha256sum "${tree}/${file}" | cut -d' ' -f1)")
            done
            if [ "$(printf '%s\n' "${in_archive[@]}" | sort)" = "$(printf '%s\n' "${on_disk[@]}" | sort)" ]; then
                n=0
                for file in ${files}; do
                    echo "${library} ${file} ${on_disk[${n}]}" >> "${list}.unsorted"
                    n=$((n + 1))
                done
            else
                for n in $(seq 1 "${count}"); do
                    echo "${library} ${member}#${n} ${in_archive[$((n - 1))]}" >> "${list}.unsorted"
                done
            fi
        done < <(ar t "${archive}" | sort | uniq -c)
    done
    rm -rf "${unpacked}"
    if [ ! -s "${list}.unsorted" ]; then
        echo "No library of the node found under ${tree}/src"
        rm -f "${list}.unsorted"
        return 1
    fi
    libraries=$(cut -d' ' -f1 "${list}.unsorted" | sort -u | wc -l)
    for entry in "${PROGRAMS[@]}"; do
        read -r program prefix <<< "${entry}"
        [ -f "${tree}/src/${program}" ] || [ -f "${tree}/src/${program}.exe" ] || continue
        for file in $(cd "${tree}" && ls "${prefix}"*.o 2> /dev/null); do
            echo "${program} ${file} $(sha256sum "${tree}/${file}" | cut -d' ' -f1)" >> "${list}.unsorted"
        done
    done
    sort "${list}.unsorted" > "${list}" && rm -f "${list}.unsorted"
    echo "Recorded $(wc -l < "${list}") objects of ${libraries} libraries and $(( $(cut -d' ' -f1 "${list}" | sort -u | wc -l) - libraries )) programs in ${list}"
}

# may_differ <library> <object file>: whether the object may differ between base and tip.
may_differ() {
    case "$2" in
        */libbitcoin_util_a-clientversion.o) return 0 ;;
    esac
    [ "$1" = libbitcoin_server.a ] && [ "$2" = src/libbitcoin_server_a-init.o ] && return 0
    [ "$1" = libbitcoin_cli.a ] && [ "$2" = src/rpc/libbitcoin_cli_a-client.o ] && return 0
    if [ "$1" = qt/libbitcoinqt.a ]; then
        case "$2" in
            src/qt/libbitcoinqt_a-bitcoin.o|src/qt/libbitcoinqt_a-bitcoingui.o|src/qt/libbitcoinqt_a-utilitydialog.o) return 0 ;;
            src/qt/libbitcoinqt_a-sendcoinsdialog.o) return 0 ;;
            src/qt/libbitcoinqt_a-transactiontablemodel.o|src/qt/libbitcoinqt_a-walletview.o) return 0 ;;
            src/qt/libbitcoinqt_a-qrc_bitcoin_locale.o) return 0 ;;
            src/qt/libbitcoinqt_a-qrc_bitcoin.o) return 0 ;;
        esac
    fi
    return 1
}

# may_be_added <library> <object file>: whether the object may be new in the tip.
may_be_added() {
    [ "$1" = libbitcoin_server.a ] && [[ "$2" == src/opreturn/* ]] && return 0
    [ "$1" = qt/libbitcoinqt.a ] && [ "$2" = src/qt/libbitcoinqt_a-guiextension.o ] && return 0
    return 1
}

# compare <base list> <tip list>
compare() {
    local base=$1 tip=$2 status=0 library object hash
    if ! diff <(cut -d' ' -f1 "${base}" | sort -u) <(cut -d' ' -f1 "${tip}" | sort -u) > /dev/null; then
        echo "The tip does not build the same libraries and programs as the base:"
        diff <(cut -d' ' -f1 "${base}" | sort -u) <(cut -d' ' -f1 "${tip}" | sort -u)
        status=1
    fi
    # Objects of the base that the tip does not have as they were.
    while read -r library object hash; do
        if may_differ "${library}" "${object}" && grep -q "^${library} ${object} " "${tip}"; then
            echo "allowed to differ: ${library} ${object}"
            continue
        fi
        if grep -q "^${library} ${object} " "${tip}"; then
            echo "CHANGED: ${library} ${object}"
        else
            echo "MISSING: ${library} ${object}"
        fi
        status=1
    done < <(comm -23 "${base}" "${tip}")
    # Objects only the tip has.
    while read -r library object hash; do
        may_differ "${library}" "${object}" && grep -q "^${library} ${object} " "${base}" && continue
        if may_be_added "${library}" "${object}" && ! grep -q "^${library} ${object} " "${base}"; then
            echo "added: ${library} ${object}"
            continue
        fi
        echo "NOT ALLOWED: ${library} ${object} is new or changed"
        status=1
    done < <(comm -13 "${base}" "${tip}")
    echo "Compared $(wc -l < "${base}") objects of the base with $(wc -l < "${tip}") of the tip: $(comm -12 "${base}" "${tip}" | wc -l) identical."
    if [ "${status}" = 0 ]; then
        echo "OK: no object changed but those allowed to"
    else
        echo "FAILED: objects changed that must not"
    fi
    return ${status}
}

# build <commit> <name>
build() {
    local commit=$1 name=$2
    echo "Building ${name} (${commit}) in ${TREE}"
    rm -rf "${TREE}" && mkdir -p "${TREE}" || return 1
    git -C "${TOP}" archive --format=tar "${commit}" | tar -x -C "${TREE}" || return 1
    export QT_RCC_SOURCE_DATE_OVERRIDE=1
    if ! (cd "${TREE}" && ./autogen.sh && ./configure "${CONFIGURE[@]}") > "${WORK}/${name}-configure.log" 2>&1; then
        echo "Configuring ${name} failed: ${WORK}/${name}-configure.log"
        return 1
    fi
    if ! make -C "${TREE}" -j"${JOBS}" > "${WORK}/${name}-make.log" 2>&1; then
        echo "Building ${name} failed: ${WORK}/${name}-make.log"
        return 1
    fi
    [ -f "${TREE}/src/ferrited" ] && cp "${TREE}/src/ferrited" "${WORK}/${name}-ferrited"
    record "${TREE}" "${WORK}/${name}.list"
}

case "${1:-}" in
    --record)
        [ $# -eq 3 ] || usage
        record "$2" "$3"
        exit $?
        ;;
    --compare)
        [ $# -eq 3 ] || usage
        compare "$2" "$3"
        exit $?
        ;;
    ''|-*)
        usage
        ;;
esac

[ $# -ge 2 ] || usage
BASE=$(git -C "${TOP}" rev-parse --verify "$1^{commit}") || exit 2
TIP=$(git -C "${TOP}" rev-parse --verify "$2^{commit}") || exit 2
shift 2
CONFIGURE=(--with-incompatible-bdb --without-gui --disable-tests --disable-bench "CFLAGS=-O2 -g0" "CXXFLAGS=-O2 -g0" "$@")
WORK=${FEXT_IDOBJ_DIR:-$(mktemp -d)}
JOBS=${FEXT_IDOBJ_JOBS:-$(nproc)}
TREE="${WORK}/tree"
mkdir -p "${WORK}" || exit 1

build "${BASE}" base || exit 1
build "${TIP}" tip || exit 1
compare "${WORK}/base.list" "${WORK}/tip.list"
