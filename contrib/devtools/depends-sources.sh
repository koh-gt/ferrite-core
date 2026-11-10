#!/usr/bin/env bash
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fetch the depends sources for the Linux, Windows and macOS builds from their own
# hosts, and collect the files the depends source mirror does not have yet, each
# named by its SHA-256 hash. See "Source mirror" in depends/README.md.
#
# Usage: contrib/devtools/depends-sources.sh <outdir>
#
#   <outdir>/sources      every source file, as depends/sources holds them
#   <outdir>/SHA256SUMS   the hash and name of each of them
#   <outdir>/mirror       the files the mirror lacks, ready to upload

export LC_ALL=C
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <outdir>" >&2
    exit 1
fi

TOPDIR=${TOPDIR:-$(git rev-parse --show-toplevel)}
MIRROR=${MIRROR_DOWNLOAD_PATH:-$(sed -n 's/^MIRROR_DOWNLOAD_PATH ?= //p' "$TOPDIR/depends/Makefile")}
if command -v sha256sum >/dev/null; then
    SHA256SUM="sha256sum"
else
    SHA256SUM="shasum -a 256"
fi

mkdir -p "$1/sources" "$1/mirror"
OUTDIR=$(cd "$1" && pwd)

# depends checks each file against its pinned hash. The mirror is skipped so that every
# file comes from its own host (or FALLBACK_DOWNLOAD_PATH); -k fetches the rest when one fails.
status=0
make -k -C "$TOPDIR/depends" SOURCES_PATH="$OUTDIR/sources" MIRROR_DOWNLOAD_PATH= download || status=$?

: > "$OUTDIR/SHA256SUMS"
total=0
missing=0
for file in "$OUTDIR"/sources/*; do
    [ -f "$file" ] || continue
    hash=$($SHA256SUM "$file" | cut -d' ' -f1)
    echo "$hash  $(basename "$file")" >> "$OUTDIR/SHA256SUMS"
    total=$((total + 1))
    # One byte is enough to tell whether the mirror has the file.
    if ! curl -sfL -r 0-0 -o /dev/null "$MIRROR/$hash"; then
        ln -f "$file" "$OUTDIR/mirror/$hash" 2>/dev/null || cp "$file" "$OUTDIR/mirror/$hash"
        missing=$((missing + 1))
    fi
done

echo "$total source files; $missing of them are not at $MIRROR and are in $OUTDIR/mirror."
if [ "$missing" -gt 0 ]; then
    if [[ "$MIRROR" =~ ^https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)$ ]]; then
        echo "Upload them with:"
        echo "  gh release upload ${BASH_REMATCH[2]} $OUTDIR/mirror/* --repo ${BASH_REMATCH[1]}"
    else
        echo "Copy them to $MIRROR."
    fi
fi
if [ "$status" -ne 0 ]; then
    echo "Some sources could not be fetched from their own hosts; see the make output above." >&2
fi
exit "$status"
