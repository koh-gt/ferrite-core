Contents
========
This directory contains tools for developers working on this repository.

clang-format-diff.py
===================

A script to format unified git diffs according to [.clang-format](../../src/.clang-format).

Requires `clang-format`, installed e.g. via `brew install clang-format` on macOS.

For instance, to format the last commit with 0 lines of context,
the script should be called from the git root folder as follows.

```
git diff -U0 HEAD~1.. | ./contrib/devtools/clang-format-diff.py -p1 -i -v
```

copyright\_header.py
====================

Provides utilities for managing copyright headers of `The Bitcoin Core
developers` in repository source files. It has three subcommands:

```
$ ./copyright_header.py report <base_directory> [verbose]
$ ./copyright_header.py update <base_directory>
$ ./copyright_header.py insert <file>
```
Running these subcommands without arguments displays a usage string.

copyright\_header.py report \<base\_directory\> [verbose]
---------------------------------------------------------

Produces a report of all copyright header notices found inside the source files
of a repository. Useful to quickly visualize the state of the headers.
Specifying `verbose` will list the full filenames of files of each category.

copyright\_header.py update \<base\_directory\> [verbose]
---------------------------------------------------------
Updates all the copyright headers of `The Bitcoin Core developers` which were
changed in a year more recent than is listed. For example:
```
// Copyright (c) <firstYear>-<lastYear> The Bitcoin Core developers
```
will be updated to:
```
// Copyright (c) <firstYear>-<lastModifiedYear> The Bitcoin Core developers
```
where `<lastModifiedYear>` is obtained from the `git log` history.

This subcommand also handles copyright headers that have only a single year. In
those cases:
```
// Copyright (c) <year> The Bitcoin Core developers
```
will be updated to:
```
// Copyright (c) <year>-<lastModifiedYear> The Bitcoin Core developers
```
where the update is appropriate.

copyright\_header.py insert \<file\>
------------------------------------
Inserts a copyright header for `The Bitcoin Core developers` at the top of the
file in either Python or C++ style as determined by the file extension. If the
file is a Python file and it has  `#!` starting the first line, the header is
inserted in the line below it.

The copyright dates will be set to be `<year_introduced>-<current_year>` where
`<year_introduced>` is according to the `git log` history. If
`<year_introduced>` is equal to `<current_year>`, it will be set as a single
year rather than two hyphenated years.

If the file already has a copyright for `The Bitcoin Core developers`, the
script will exit.

depends-sources.sh
==================

Fetches every depends source for the Linux, Windows and macOS builds from its own host
and collects the files that the depends source mirror (`MIRROR_DOWNLOAD_PATH`, see
[depends/README.md](../../depends/README.md)) does not have yet, each named by its SHA-256
hash, then prints the command that uploads them. Run it whenever a depends package is
added or its version changes:

```bash
contrib/devtools/depends-sources.sh /tmp/depends-sources
```

The release has to exist before the first upload. Create it as a pre-release, so that it
is never shown as the latest Ferrite Core release:

```bash
gh release create depends-sources --repo koh-gt/ferrite-core --prerelease \
    --title "depends sources" --notes "Source files for depends, each named by its SHA-256 hash."
```

gen-manpages.sh
===============

A small script to automatically create manpages in ../../doc/man by running the release binaries with the -help option.
This requires help2man which can be found at: https://www.gnu.org/software/help2man/

With in-tree builds this tool can be run from any directory within the
repostitory. To use this tool with out-of-tree builds set `BUILDDIR`. For
example:

```bash
BUILDDIR=$PWD/build contrib/devtools/gen-manpages.sh
```

security-check.py and test-security-check.py
============================================

Perform basic security checks on a series of executables.

symbol-check.py
===============

A script to check that the executables produced by gitian only contain
certain symbols and are only linked against allowed libraries.

For Linux this means checking for allowed gcc, glibc and libstdc++ version symbols.
This makes sure they are still compatible with the minimum supported distribution versions.

For macOS and Windows we check that the executables are only linked against libraries we allow.

Example usage after a gitian build:

    find ../gitian-builder/build -type f -executable | xargs python3 contrib/devtools/symbol-check.py

If no errors occur the return value will be 0 and the output will be empty.

If there are any errors the return value will be 1 and output like this will be printed:

    .../64/test_bitcoin: symbol memcpy from unsupported version GLIBC_2.14
    .../64/test_bitcoin: symbol __fdelt_chk from unsupported version GLIBC_2.15
    .../64/test_bitcoin: symbol std::out_of_range::~out_of_range() from unsupported version GLIBCXX_3.4.15
    .../64/test_bitcoin: symbol _ZNSt8__detail15_List_nod from unsupported version GLIBCXX_3.4.15

circular-dependencies.py
========================

Run this script from the root of the source tree (`src/`) to find circular dependencies in the source code.
This looks only at which files include other files, treating the `.cpp` and `.h` file as one unit.

Example usage:

    cd .../src
    ../contrib/devtools/circular-dependencies.py {*,*/*,*/*/*}.{h,cpp}

mweb\_history\_check.py
=======================

Read-only scan of a synced node's MWEB history against the MWEB rules added in Ferrite Core v3.3.0:
every MWEB input must match the output it spends, and kernel lock heights above 903,321 must not
exceed the block height. Kernels that would break the two MWEB rules of the `mwebkernel` soft fork,
which miners signal for from block 1,250,000, are reported for information. The script only calls `getblockchaininfo`,
`getblockhash` and `getblock`; it needs Python 3.8+ and no extra packages.

Example usage (reads the RPC cookie from the default data directory):

    python3 contrib/devtools/mweb_history_check.py
    python3 contrib/devtools/mweb_history_check.py --datadir /path/to/datadir
    python3 contrib/devtools/mweb_history_check.py --rpcuser USER --rpcpassword PASS

p2mr\_census.py
===============

Read-only census of a node's chain for the soft forks of Ferrite Core v4.0.0: every output of
witness version 2 (an unspent output with a 32-byte program can no longer be spent without a valid
P2MR script path once the `p2mr` soft fork is active), the spends of coinbase outputs 10 to 99
blocks after their block (which the `coinbasematurity` soft fork refuses for coinbases created once
it is active), and the blocks that signal for the soft forks in each window. The script only calls
`getblockchaininfo`, `getblockhash` and `getblock`, and it connects only to the host and port of
`--rpcurl`: no proxy is used and no redirect is followed. It needs Python 3.8+ and no extra
packages. On the computer that runs the node it reads the RPC cookie, or the `rpcuser` and
`rpcpassword` lines of `ferrite.conf`, from the data directory; `--rpcuser` and `--rpcpassword` are
shown in the process list to other users of the computer, and without `--rpcpassword` the script
asks for the password. The report goes to standard output and ends with `RESULT:` lines.

Example usage:

    python3 contrib/devtools/p2mr_census.py > census.txt
    python3 contrib/devtools/p2mr_census.py --datadir /path/to/datadir --start 1050000 > census.txt
    python3 contrib/devtools/p2mr_census.py --testnet > census-testnet.txt

timewarp\_count.py
==================

Read-only count of the blocks whose time is earlier than 480 seconds after the time of the block 23
below them, which the block time rule of the `timewarp` soft fork refuses once it is active (the
rule tightens the block timestamps the difficulty calculation uses), and of the blocks whose time
is ahead of the median time of the 11 blocks after them. By default it reads the last 100,000
blocks; `--start 24` reads the whole chain. It works on a pruned node. The script only calls
`getblockchaininfo`, `getblockhash`, `getblockheader` and `getblock`, and it connects and
authenticates as `p2mr_census.py` does.

Example usage:

    python3 contrib/devtools/timewarp_count.py > timewarp.txt
    python3 contrib/devtools/timewarp_count.py --start 24 > timewarp.txt
