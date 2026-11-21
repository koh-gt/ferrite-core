#!/usr/bin/env bash
#
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Check that the OP_RETURN record changes no object of the node's libraries but the ones it must.
#
#   contrib/devtools/fext-identical-objects.sh <base> <tip> [<configure argument>...]
#       Exports <base> and then <tip> with git archive into one directory, builds each with the same configure
#       arguments (by default those of the CI build, without tests), and compares their objects.
#   contrib/devtools/fext-identical-objects.sh --record <built tree> <list>
#       Writes every object of the node's libraries in a built tree, with its SHA-256, to <list>.
#   contrib/devtools/fext-identical-objects.sh --compare <base list> <tip list>
#       Compares two such lists.
#
# Every object of every library is the same byte for byte, except:
#   - clientversion.o, which holds the version of the commit it was built from;
#   - in libbitcoin_server.a, init.o, which may call the record, and the record's new objects, those built from
#     src/opreturn/;
#   - in libbitcoin_cli.a, client.o, which may list the record's calls for ferrite-cli.
# No object may be missing from the tip, and no library may appear or disappear.
#
# The builds run Qt's resource compiler with QT_RCC_SOURCE_DATE_OVERRIDE=1, so that a build with the GUI compares
# too: the compiler writes the time of each resource file into the GUI library, and git archive gives the files
# the time of their commit.
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

usage() {
    sed -n '9,16p' "$0" | sed 's/^# \{0,3\}//'
    exit 2
}

# record <built tree> <list>: one line per object, "<library> <name> <sha256>", sorted. An archive keeps only the last
# part of an object's file name, and two objects of one library can share it (src/init.cpp and src/wallet/init.cpp
# both give libbitcoin_server_a-init.o), so an object is named by its file in the build when the build's files of
# that name are exactly the archive's objects of that name, and otherwise by its name and its place among them.
record() {
    local tree=$1 list=$2 unpacked library archive member count n file files in_archive on_disk
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
    sort "${list}.unsorted" > "${list}" && rm -f "${list}.unsorted"
    if [ ! -s "${list}" ]; then
        echo "No library of the node found under ${tree}/src"
        return 1
    fi
    echo "Recorded $(wc -l < "${list}") objects of $(cut -d' ' -f1 "${list}" | sort -u | wc -l) libraries in ${list}"
}

# may_differ <library> <object file>: whether the object may differ between base and tip.
may_differ() {
    case "$2" in
        */libbitcoin_util_a-clientversion.o) return 0 ;;
    esac
    [ "$1" = libbitcoin_server.a ] && [ "$2" = src/libbitcoin_server_a-init.o ] && return 0
    [ "$1" = libbitcoin_cli.a ] && [ "$2" = src/rpc/libbitcoin_cli_a-client.o ] && return 0
    return 1
}

# compare <base list> <tip list>
compare() {
    local base=$1 tip=$2 status=0 library object hash
    if ! diff <(cut -d' ' -f1 "${base}" | sort -u) <(cut -d' ' -f1 "${tip}" | sort -u) > /dev/null; then
        echo "The tip does not build the same libraries as the base:"
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
        if [ "${library}" = libbitcoin_server.a ] && [[ "${object}" == src/opreturn/* ]] && ! grep -q "^${library} ${object} " "${base}"; then
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
CONFIGURE=("$@")
[ ${#CONFIGURE[@]} -eq 0 ] && CONFIGURE=(--with-incompatible-bdb --without-gui --disable-tests --disable-bench)
WORK=${FEXT_IDOBJ_DIR:-$(mktemp -d)}
JOBS=${FEXT_IDOBJ_JOBS:-$(nproc)}
TREE="${WORK}/tree"
mkdir -p "${WORK}" || exit 1

build "${BASE}" base || exit 1
build "${TIP}" tip || exit 1
compare "${WORK}/base.list" "${WORK}/tip.list"
