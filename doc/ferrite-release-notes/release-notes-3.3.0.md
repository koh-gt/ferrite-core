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

## Changes in v3.3.0

### Miners and pools: signal for the MWEB soft fork from block 1,250,000
v3.3.0 carries an MWEB soft fork that miners activate by signalling. It makes two malformed MWEB kernel encodings
invalid: a kernel that sets the peg-out feature without a peg-out, and a kernel that sets the extra-data feature
without extra data. Normal wallets never create them, and v3.3.0 already refuses to relay or mine them.

From block **1,250,000** (expected 8–19 March 2027), blocks mined with v3.3.0 signal on version bit 5. When 39 of
the 40 blocks in a window signal, the soft fork locks in for the next window and is enforced after it, from block
1,250,080 at the earliest. Until enough blocks signal, nothing changes, and nothing activates it without
signalling.
> Run v3.3.0 before block 1,250,000 and build blocks with the `version` that `getblocktemplate` returns. Pool
software that hard-codes the block version does not signal, and the soft fork waits until nearly every block
signals.

Wallets and other nodes keep working on v3.2.x, but without the security updates below. v3.2.x nodes log
"Warning: unknown new rules activated (versionbit 5)" once the soft fork locks in, and show it in the wallet and in
RPC `warnings` once it is active.

### Security Updates
MWEB security fixes from Litecoin Core v0.21.5.4 – v0.21.5.8, released after the Litecoin MWEB incident:
* MWEB inputs must match the outputs they spend, and kernel lock heights cannot exceed the block height (from block 903,321)
* Stricter HogEx, peg-in and fee-overflow checks
* Better handling of mutated MWEB blocks and compact blocks; peers that send them are discouraged
* Rate limits for MWEB leafset requests, protection against MWEB relay censorship, safer fast relay of new blocks
* Conflicting MWEB transactions are evicted from the mempool when a block connects

Bitcoin Core v25.0 fix [Vulnerability CVE-2024-35202](https://www.cvedetails.com/cve/CVE-2024-35202/): a peer
could crash a node with a crafted compact-block message.

**No blacklist.** Unlike Litecoin, Ferrite freezes no coins and grandfathers no block. A scan of Ferrite's complete
MWEB history (blocks 150,120 to 1,057,006) found no block that breaks the new rules.

### Checkpoints
Added checkpoints at heights 450,000, 602,214 (2nd halving), 750,000, 903,321 (3rd halving) and 1,000,000.
Assumed-valid block and minimum chain work updated to block 1,046,922.

### Miscellaneous
Version and copyright year increment
The source tree builds again, with the unit and functional test suites working, and every change is now checked by
GitHub Actions CI
New tool `contrib/devtools/mweb_history_check.py`: read-only scan of a node's MWEB history
RPC: no command or argument changed. `getblockchaininfo` lists the new `mwebkernel` soft fork under `softforks`;
`getblocktemplate` shows it under `vbavailable` while miners signal and under `rules` once active. `getmempoolentry`
returns an error for MWEB-only transactions on nodes started with `-rpcserialversion=0` or `1`; `maxfeerate=0` in
`sendrawtransaction` / `testmempoolaccept` now means "no limit" for MWEB transactions too; `dumpwallet` files gain a
`# mweb view keys:` comment line
Mempool: MWEB transactions with too many inputs, too much weight, invalid output keys or empty peg-out kernels are no
longer accepted or relayed
Windows: `ferrited` has no daemon mode. Remove `daemon=1` from `ferrite.conf` (or `-daemon` from the command line)
> Downgrading: v3.3.0 records an extra flag in the block undo files (`rev*.dat`). If you go back to an older version
after running v3.3.0, start it once with `-reindex-chainstate`.

For more details see https://github.com/koh-gt/ferrite-core/pull/65 and `doc/ferrite-3.3.0-review.md`

## Future changes
v4.0.0 soft fork: post-quantum (P2QRH) outputs, under evaluation
v5.0.0 hard fork: CPU proof of work and tail emission, under evaluation
Litecoin Core updates

## Mining
[**List of pools and settings**](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
Ferrite runs on Scrypt algorithm -compatible with Litecoin/Dogecoin miners
**Scrypt is not an ASIC-resistant algorithm.**
Scrypt ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).
**Pools should run v3.3.0 before block 1,250,000** and mine with the block version from `getblocktemplate`, so that
their blocks signal for the MWEB soft fork. `getblocktemplate` still accepts `{"rules": ["segwit"]}`; as before, the
template always carries the MWEB block, and pools must include it in the blocks they mine.

## Other notes (on v1, v2 wallets before _13 Apr 2022_).
### All versions of Ferrite Core **wallets** will maintain backwards compatibility
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.


* Windows and Linux builds compiled on Ubuntu Linux by kohgt
Uploaded on <date>.

## What's Changed
* v3.3.0 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/65

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.2.0...v3.3.0
