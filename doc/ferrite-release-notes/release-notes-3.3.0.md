### v3.3.1 replaces this release
v3.3.0 and its pre-release v3.3.0r1 lack the security and stability fixes of v3.3.1. Run v3.3.1 instead; its release
notes are in `doc/ferrite-release-notes/release-notes-3.3.1.md`.

## v3.3.0 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/ce12f90e-286d-4229-b5db-db4c462e6256 height=32> <br> `-        Windows        -` | SHA256 Hash |
|-|-|
| [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.0/ferrite-3.3.0-win64-setup.exe) | `<SHA256>` <br> **New install** _ferrite-3.3.0-win64-setup.exe_ |
| [<img alt=download-2 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.0/ferrite-qt.exe) |  `<SHA256>` <br> **Wallet Update** _ferrite-qt.exe_ |
| [<img alt=download-3 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.0/ferrite-win-3.3.0.zip) | `<SHA256>` <br> **Server** |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/1c13b30a-95de-43bc-82c7-864a86e4658c height=32> <br> `-         Linux         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.0/ferrite-qt)           | `<SHA256>` <br> **Wallet** _ferrite-qt_   |
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.0/ferrite-unix-3.3.0.zip)               | `<SHA256>` <br> **Server**  |

## v3.1.3 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/c07963c6-718e-4ee3-a9d5-a9a96010f99a height=32> <br> `-         MacOS         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Qt.dmg)               | `e076cfd1515066708fc2b9417d13311ea1cbcf16102962b6b8bf9dfadcda6003` <br> **Wallet (v3.1.3)** _Ferrite-Qt.dmg_      | 
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Mac.7z)               | `335a9d3f41682f67ac2ac82f5832b4ea26ad0e8c215eb10db3b9f1f66cb918d6` <br> **Server (v3.1.3)**  |

The macOS downloads are still v3.1.3. They lack the security updates of v3.2.0 and v3.3.0 and do not signal for the
MWEB soft fork.

## Changes in v3.3.0

### Miners and pools: signal for the MWEB soft fork from block 1,250,000
v3.3.0 carries an MWEB soft fork that miners activate by signalling. It makes two malformed MWEB kernel encodings
invalid: a kernel that sets the peg-out feature without a peg-out, and a kernel that sets the extra-data feature
without extra data. Normal wallets never create them, and on mainnet v3.3.0 already refuses to relay or mine them.

From block **1,250,000** (expected in mid to late March 2027), blocks mined with v3.3.0 signal on version bit 5. When
39 of the 40 blocks in a window signal, the soft fork locks in for the next window and is enforced after it, from block
1,250,080 at the earliest. Until enough blocks signal, nothing changes. Without signalling, the soft fork would lock in
only at its timeout height, block 100,000,000, about two centuries away.
> Upgrade now, not only before block 1,250,000. The security updates below need no signalling: they take effect as
soon as a node runs v3.3.0. To signal, build blocks with the `version` that `getblocktemplate` returns. Pool
software that hard-codes the block version does not signal, and the soft fork waits until nearly every block
signals.

Nodes that stay on v3.2.x lack the security updates below, and they cannot download the largest blocks that MWEB data
allows, so they can fall behind the chain. Upgrade every node, wallets included. v3.2.x nodes log "Warning: unknown new
rules activated (versionbit 5)" once the soft fork locks in, and show it in the wallet and in RPC `warnings` once it is
active.

### Security Updates
MWEB security fixes from Litecoin Core v0.21.5.4 – v0.21.5.8, released after the Litecoin MWEB incident. The block
rules among them apply from the moment a node runs v3.3.0, without signalling:
* MWEB inputs must match the outputs they spend, in every block, and a kernel's lock height cannot exceed the height of
  its block, in every block after block 903,321
* Stricter HogEx, peg-in and fee-overflow checks
* Better handling of mutated MWEB blocks and compact blocks; peers that send them are discouraged
* Rate limits for MWEB leafset requests, protection against MWEB relay censorship, safer fast relay of new blocks
* Conflicting MWEB transactions are evicted from the mempool when a block connects
* Nodes with a wallet no longer stop on malformed MWEB outputs

Bitcoin Core v25.0 fix [Vulnerability CVE-2024-35202](https://www.cvedetails.com/cve/CVE-2024-35202/): a peer
could crash a node with a crafted compact-block message.

**No blacklist.** Unlike Litecoin, Ferrite freezes no coins and grandfathers no block. A scan of Ferrite's complete
MWEB history (blocks 150,120 to 1,057,006) found no block that breaks the new rules.

### Wallet
**Peg-outs to your own addresses** (from Litecoin Core): the amount of an MWEB transaction now counts a peg-out to one
of the wallet's own addresses as received. `gettransaction` "amount" is now 0 for a peg-out from your MWEB coins to your
own address (before: minus the peg-out amount), and the peg-out amount for a peg-out that another wallet sends to your
address, when your wallet saw that transaction before it was in a block (before: 0). The ferrite-qt transaction list and
`listwallettransactions` now show such a received peg-out with its amount. The pegged-out coins arrive in a separate
transaction of the same block (the HogEx transaction), which `gettransaction` and `listtransactions` also show as
received: when you add up amounts across transactions, count each such peg-out once. Balances are not affected.

**Restoring a wallet that holds MWEB coins.** A wallet that learned of an MWEB coin only from a block, and then learns
of that coin's spend only from a block too, keeps showing the coin as unspent. This happens when a wallet is restored
from its seed (`sethdseed`) and rescans the chain, and it can happen when a `wallet.dat` backup older than the spend is
restored. The balance is then too high, and transactions that use such a coin cannot be broadcast. A rescan
(`rescanblockchain`) does not correct this in this version or in v3.3.1; a later release is to fix it. Until then, keep
a backup of the wallet made after your latest MWEB transaction, and restore from that backup rather than from the seed.
Make the backup with File > Backup Wallet in ferrite-qt or with `backupwallet`: a copy of `wallet.dat` taken while the
wallet is loaded can be damaged.

### Checkpoints
Added checkpoints at heights 292,000, 450,000, 602,214 (2nd halving), 750,000, 903,321 (3rd halving) and 1,000,000;
the last checkpoint of v3.2.0 was block 200,000. Once a node has a checkpoint block, it rejects any chain that forks
from the known chain below that block's height.
Assumed-valid block and minimum chain work updated to block 1,046,922 (v3.2.0: block 200,000).

### Miscellaneous
Version and copyright year increment
The source tree builds again, and the unit tests and the functional test framework run again. GitHub Actions CI builds
every change, with and without the GUI, and runs the unit tests and a list of functional tests, most of them MWEB tests.
Many other functional tests still fail on test data inherited from Bitcoin Core and Litecoin
New tool `contrib/devtools/mweb_history_check.py`: read-only scan of a node's MWEB history
RPC: no command or argument changed. `getblockchaininfo` lists the new `mwebkernel` soft fork under `softforks`;
`getblocktemplate` shows it under `vbavailable` while miners signal and under `rules` once active. `getmempoolentry`
returns an error for MWEB-only transactions on nodes started with `-rpcserialversion=0` or `1`; `maxfeerate=0` in
`sendrawtransaction` / `testmempoolaccept` now means "no limit" for MWEB transactions too; `dumpwallet` files gain a
`# mweb view keys:` comment line; `gettransaction` amounts of peg-outs to the wallet change (see Wallet)
Mempool: MWEB transactions with too many inputs, too much weight, invalid output keys or empty peg-out kernels are no
longer accepted or relayed. Peg-outs now pass the same standard-script and dust checks as outputs, and a transaction may
carry only one OP_RETURN script among its outputs and peg-outs
Windows: `ferrited` has no daemon mode. Remove `daemon=1` from `ferrite.conf` (or `-daemon` from the command line).
The `ferrite.conf` that v3.3.0 writes into a new data directory contains this line; fixed in v3.3.1, whose notes list
the other lines of that file worth removing
> Downgrading: v3.3.0 stores an extra flag in the block undo files (`rev*.dat`) that older versions read wrongly. To go
back to an older version after running v3.3.0, shut v3.3.0 down and start the older version once with `-reindex`
(`ferrited -reindex` or `ferrite-qt -reindex`), not `-reindex-chainstate`, which keeps the undo files. Without
`-reindex` the older version can refuse to start ("Corrupted block database detected") or stop during a chain
reorganisation ("Failed to disconnect block"). `-reindex` rebuilds the block index, the chain state and the undo files
from the block files, which can take hours; a pruned node downloads the chain again.

For more details see https://github.com/koh-gt/ferrite-core/pull/66 and `doc/ferrite-3.3.0-review.md`

### Building
Windows and Linux builds with `depends` work again, the Qt wallet included: the Boost libraries are built again, the Qt
and zlib downloads point at their current paths, and Qt no longer needs a manual edit for GCC 11. The macOS recipes parse again
OpenSSL is no longer needed: scrypt uses Ferrite Core's own SHA-256, and the proof-of-work hash is unchanged
fmt is used header-only: any `libfmt-dev` from 6.1.2 on works, and no fmt library is linked
`depends` tries Ferrite Core's copy of each source file first (the `depends-sources` release), then the file's own host,
then bitcoincore.org, and uses a file only if it matches its pinned SHA-256 hash. qrencode 3.4.4 is no longer on its own host,
so until the mirror is filled it comes from bitcoincore.org
Linux binaries to distribute are best built with `depends`, as `doc/build-unix.md` now describes. Built on Ubuntu 22.04, they
need glibc 2.34 or later (Ubuntu 22.04, Debian 12 or newer), and `ferrite-qt` also needs the system's fontconfig, FreeType and libxcb

## Future changes
v4.0.0 soft fork: post-quantum (P2QRH) outputs, under evaluation
v5.0.0 hard fork: CPU proof of work and tail emission, under evaluation
Litecoin Core updates

## Mining
[**List of pools and settings**](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
Ferrite runs on Scrypt algorithm -compatible with Litecoin/Dogecoin miners
**Scrypt is not an ASIC-resistant algorithm.**
Scrypt ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).
**Pools should run v3.3.0 now**: its security updates apply at once, without signalling. Mine with the block version
from `getblocktemplate`, so that blocks signal for the MWEB soft fork from block 1,250,000. `getblocktemplate` still
accepts `{"rules": ["segwit"]}`; as before, the template always carries the MWEB block, and pools must include it in the
blocks they mine.

## Other notes (on v1, v2 wallets before _13 Apr 2022_).
### All versions of Ferrite Core **wallets** will maintain backwards compatibility
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.


* Windows and Linux builds compiled on Ubuntu Linux by kohgt
Uploaded on <date>.

## What's Changed
* v3.3.0 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/66

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.2.0...v3.3.0
