#!/usr/bin/env bash
# Copyright (c) 2026 The Ferrite Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Fetch the depends sources for the Linux, Windows and macOS builds from their own
# hosts, and collect the files the depends source mirror does not have yet, each
# named by its SHA-256 hash. See "Source mirror" in depends/README.md.
#
# Usage: contrib/devtools/depends-sources.sh [--upload] [--verify] <outdir>
#
#   <outdir>/sources      every source file, as depends/sources holds them
#   <outdir>/SHA256SUMS   the hash that its depends recipe pins for each of them, and its name
#   <outdir>/mirror       the files the mirror lacks, ready to upload (emptied first)
#
#   --upload   upload the files the mirror lacks to its GitHub release with gh,
#              after creating the release as a pre-release if it does not exist
#   --verify   read every file back from the mirror and check its SHA-256 hash
#
# Every hash is the one that the depends recipe pins, never the hash of the file at hand:
# a file in <outdir>/sources that does not match its pinned hash is reported and not
# uploaded. A file that could not be fetched is read back from the mirror: if the mirror
# has it, that is only a warning, because depends will take it from there. The script
# fails when a file was neither fetched nor is on the mirror. Whenever the download failed,
# "make download" finally runs once more with the mirror and has to pass, so that a file
# missing from the list of pinned hashes cannot go unnoticed.

export LC_ALL=C
set -euo pipefail

usage() {
    echo "Usage: $0 [--upload] [--verify] <outdir>" >&2
    exit 1
}

upload=0
verify=0
outdir=
for arg in "$@"; do
    case "$arg" in
        --upload) upload=1 ;;
        --verify) verify=1 ;;
        -*) usage ;;
        *) [ -z "$outdir" ] || usage
           outdir=$arg ;;
    esac
done
[ -n "$outdir" ] || usage

TOPDIR=${TOPDIR:-$(git rev-parse --show-toplevel)}
MIRROR=${MIRROR_DOWNLOAD_PATH:-$(sed -n 's/^MIRROR_DOWNLOAD_PATH ?= //p' "$TOPDIR/depends/Makefile")}
if command -v sha256sum >/dev/null; then
    SHA256SUM="sha256sum"
else
    SHA256SUM="shasum -a 256"
fi

sha256() {
    $SHA256SUM "$1" | cut -d' ' -f1
}

# The repository and release tag of a GitHub release download path.
REPO=
TAG=
if [[ "$MIRROR" =~ ^https://github\.com/([^/]+/[^/]+)/releases/download/([^/]+)$ ]]; then
    REPO=${BASH_REMATCH[1]}
    TAG=${BASH_REMATCH[2]}
fi
if [ "$upload" -eq 1 ] && [ -z "$REPO" ]; then
    echo "--upload needs a GitHub release as the mirror, and $MIRROR is not one." >&2
    exit 1
fi

mkdir -p "$outdir/sources" "$outdir/mirror"
OUTDIR=$(cd "$outdir" && pwd)
# Files collected by an earlier run into the same <outdir> may be on the mirror by now.
rm -f "$OUTDIR"/mirror/*

# The hash that its depends recipe pins for every source file, and the name of the file in
# depends/sources: this list decides what is uploaded and what the mirror must hold. A dry
# run of "make download" that takes nothing as fetched yet (-B) prints, for each file, the
# command with which depends checks what it fetched against that hash.
dry_run=$(make -n -B -C "$TOPDIR/depends" SOURCES_PATH="$OUTDIR/sources" MIRROR_DOWNLOAD_PATH= download)
printf '%s\n' "$dry_run" | { grep -o 'echo "[0-9a-f]\{64\}  [^"]*\.temp"' || true; } |
    sed 's|^echo "\([0-9a-f]*\)  .*/\(.*\)\.temp"$|\1  \2|' | sort -u | sort -k2 > "$OUTDIR/SHA256SUMS"
if [ ! -s "$OUTDIR/SHA256SUMS" ]; then
    echo "Cannot read the pinned hashes of the depends sources from a dry run of make download." >&2
    exit 1
fi

if [ "$upload" -eq 1 ]; then
    # Done before the download, so that a missing or signed-out gh stops the script at
    # once. A pre-release is never shown as the latest Ferrite Core release.
    if ! gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
        gh release create "$TAG" --repo "$REPO" --prerelease --title "depends sources" \
            --notes "Source files for depends, each named by its SHA-256 hash."
    fi
fi

# depends checks each file that it fetches against its pinned hash. The mirror is skipped
# so that every file comes from its own host (or FALLBACK_DOWNLOAD_PATH); -k fetches the
# rest when one fails.
make_status=0
make -k -C "$TOPDIR/depends" SOURCES_PATH="$OUTDIR/sources" MIRROR_DOWNLOAD_PATH= download || make_status=$?

total=0
missing=0
unfetched=
while read -r hash name; do
    total=$((total + 1))
    file=$OUTDIR/sources/$name
    if [ ! -f "$file" ]; then
        echo "$name could not be fetched." >&2
        unfetched="$unfetched $hash"
    elif [ "$(sha256 "$file")" != "$hash" ]; then
        # depends does not check a file that was in <outdir>/sources before this run.
        echo "$name in $OUTDIR/sources does not match its pinned hash; delete it to fetch it again." >&2
        unfetched="$unfetched $hash"
    elif ! curl -sfL --retry 3 -r 0-0 -o /dev/null "$MIRROR/$hash"; then
        # One byte is enough to tell whether the mirror has the file.
        ln -f "$file" "$OUTDIR/mirror/$hash" 2>/dev/null || cp "$file" "$OUTDIR/mirror/$hash"
        missing=$((missing + 1))
    fi
done < "$OUTDIR/SHA256SUMS"

if [ "$missing" -eq 0 ]; then
    echo "$total source files; $MIRROR has every file that was fetched."
else
    echo "$total source files; $missing of them are not at $MIRROR and are in $OUTDIR/mirror."
    if [ "$upload" -eq 1 ]; then
        # Only files that the mirror does not serve are uploaded. --clobber replaces an
        # asset of the same name that an interrupted upload left unusable.
        gh release upload "$TAG" "$OUTDIR"/mirror/* --repo "$REPO" --clobber
        echo "Uploaded them to the $TAG release of $REPO."
    elif [ -n "$REPO" ]; then
        echo "Upload them with:"
        echo "  gh release upload $TAG $OUTDIR/mirror/* --repo $REPO"
    else
        echo "Copy them to $MIRROR."
    fi
fi

# Read files back from the mirror and check their hash: every file with --verify, and in
# any case each file that was not fetched, which the mirror must then have already.
bad=0
damaged=0
mirror_only=0
tmp="$OUTDIR/verify.tmp"
while read -r hash name; do
    case " $unfetched " in
        *" $hash "*) fetched=0 ;;
        *) fetched=1 ;;
    esac
    [ "$verify" -eq 1 ] || [ "$fetched" -eq 0 ] || continue
    if ! curl -sfL --retry 3 -o "$tmp" "$MIRROR/$hash"; then
        echo "$name: $MIRROR/$hash cannot be fetched." >&2
        bad=$((bad + 1))
    elif [ "$(sha256 "$tmp")" != "$hash" ]; then
        echo "$name: $MIRROR/$hash does not match its hash." >&2
        bad=$((bad + 1))
        damaged=$((damaged + 1))
    elif [ "$fetched" -eq 0 ]; then
        echo "Warning: $name was not fetched, but $MIRROR has it." >&2
        mirror_only=$((mirror_only + 1))
    fi
done < "$OUTDIR/SHA256SUMS"
rm -f "$tmp"

if [ "$bad" -ne 0 ]; then
    echo "Missing at $MIRROR or not matching their hash there: $bad of the $total files." >&2
    if [ "$damaged" -ne 0 ] && [ -n "$REPO" ]; then
        echo "Delete a damaged file with \"gh release delete-asset $TAG <hash> --repo $REPO\";" >&2
        echo "the next run with --upload puts it back." >&2
    fi
    exit 1
fi
if [ "$verify" -eq 1 ]; then
    echo "All $total files at $MIRROR match their hash."
fi
if [ "$mirror_only" -ne 0 ]; then
    echo "Warning: files that could not be fetched and that only the mirror has: $mirror_only of the $total." >&2
fi
if [ "$make_status" -ne 0 ]; then
    # Every file of the list that was not fetched is on the mirror, or the script has stopped
    # by now. The list is read from make's output, though, and could lack a file. So depends
    # fetches what it still misses once more, this time with the mirror, and checks it
    # against its pinned hash. That fails if a file is neither on its hosts nor on the
    # mirror, whether the list has it or not, and if make failed for any other reason.
    echo "make download failed; running it again with the mirror."
    if ! make -C "$TOPDIR/depends" SOURCES_PATH="$OUTDIR/sources" MIRROR_DOWNLOAD_PATH="$MIRROR" download; then
        echo "make download failed, and it fails with the mirror as well; see its output above." >&2
        exit 2
    fi
    echo "Warning: make download failed without the mirror and passes with it." >&2
fi
exit 0
