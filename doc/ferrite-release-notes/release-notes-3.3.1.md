### This is a revision of Ferrite Core v3.3.0 with security and stability fixes.
v3.3.0 and its pre-release v3.3.0r1 do not contain these fixes. Every node should upgrade to v3.3.1: wallets, miners,
pools and exchanges included.

## v3.3.1 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/ce12f90e-286d-4229-b5db-db4c462e6256 height=32> <br> `-        Windows        -` | SHA256 Hash |
|-|-|
| [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.1/ferrite-3.3.1-win64-setup.exe) | `<SHA256>` <br> **New install** _ferrite-3.3.1-win64-setup.exe_ |
| [<img alt=download-2 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.1/ferrite-qt.exe) |  `<SHA256>` <br> **Wallet Update** _ferrite-qt.exe_ |
| [<img alt=download-3 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.1/ferrite-win-3.3.1.zip) | `<SHA256>` <br> **Server** |

| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/1c13b30a-95de-43bc-82c7-864a86e4658c height=32> <br> `-         Linux         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.1/ferrite-qt)           | `<SHA256>` <br> **Wallet** _ferrite-qt_   |
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.3.1/ferrite-unix-3.3.1.zip)               | `<SHA256>` <br> **Server**  |

## v3.1.3 wallets
| <img alt=download-logo-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/c07963c6-718e-4ee3-a9d5-a9a96010f99a height=32> <br> `-         MacOS         -` | SHA256 Hash |
|-|-|
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Qt.dmg)               | `e076cfd1515066708fc2b9417d13311ea1cbcf16102962b6b8bf9dfadcda6003` <br> **Wallet (v3.1.3)** _Ferrite-Qt.dmg_      | 
|  [<img alt=download-1 src=https://github.com/koh-gt/ferrite-core/assets/101822992/70ae208a-caaa-40ff-856c-42e87745e8c3 height=32>](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.3/Ferrite-Mac.7z)               | `335a9d3f41682f67ac2ac82f5832b4ea26ad0e8c215eb10db3b9f1f66cb918d6` <br> **Server (v3.1.3)**  |

The macOS downloads are still v3.1.3. They lack the security updates of v3.2.0, v3.3.0 and v3.3.1 and do not signal for
the MWEB soft fork.

## Changes in v3.3.1
v3.3.1 is v3.3.0 with the changes below. What the v3.3.0 release notes describe still applies, the MWEB soft fork
included; they are in `doc/ferrite-release-notes/release-notes-3.3.0.md`. The consensus rules are those of v3.3.0; the
new checkpoint is described under Checkpoints.

### How to upgrade
Shut down the version you are running and wait until it has stopped completely. Then replace its programs with those of
v3.3.1 and start again. v3.3.1 uses the data directory, the wallets and the `ferrite.conf` of v3.3.0 as they are and
needs no `-reindex`. The one exception is a node that holds an MWEB block it cannot read back; see the note under
Security Updates. Your `ferrite.conf` is not changed: "New ferrite.conf in new data directories" below lists the lines
worth removing from a file that an earlier version wrote. If you upgrade from v3.2.x or older, the v3.3.0 release notes
apply as well.
> Downgrading: v3.3.0 reads what v3.3.1 writes (block files, chain state, wallets, `peers.dat` and a `ferrite.conf`
written by v3.3.1), so going back to v3.3.0 needs no `-reindex`. v3.3.0 then has the problems listed under Security
Updates again. Going back to a version older than v3.3.0 needs `-reindex`, because of the block undo files, as the
v3.3.0 release notes describe.

### Miners and pools
The MWEB soft fork of v3.3.0 is unchanged, and v3.3.1 signals for it as v3.3.0 does: from block 1,250,000, blocks built
with the `version` that `getblocktemplate` returns signal on version bit 5, as the v3.3.0 release notes describe. Two
changed defaults affect block templates: the node now mines data carrier (OP_RETURN) transactions with a script of up
to 100,000 bytes, and the time of a new template follows the time offset of outbound peers only. Both are described
under "Changed defaults and settings".

### Security Updates
v3.3.0 and v3.3.0r1 do not have these fixes; most of the problems also affect v3.2.x. None of the fixes changes which
blocks are valid.
* MWEB blocks (Ferrite fix): a block holding certain unusual MWEB kernels could be accepted and then fail to be read
  back from disk, which could stop the node. Such blocks are now stored and relayed in a form that reads back. Kernel
  IDs, kernel roots and block hashes are computed exactly as before.
* MWEB addresses (Ferrite fix): a malformed MWEB address, given to an RPC, ferrite-qt or ferrite-tx, or found in a
  wallet's address book, could stop the node. It is now rejected like any other invalid address. Valid MWEB addresses
  are not affected.
* Signing (Bitcoin Core #21330 and part of #21365): a transaction that spends a Taproot output could stop the node in
  `signrawtransactionwithkey`, `signrawtransactionwithwallet` or `combinerawtransaction`, and could stop `ferrite-tx`.
  They still cannot check the signature of such an input, but now return the input without its witness.
  `signrawtransactionwithkey` and `signrawtransactionwithwallet` report it as not complete; `combinerawtransaction` and
  `ferrite-tx` give no error. The signing code also no longer makes or accepts a witness v0 signature for an input whose
  amount is negative, which stands for a missing amount.
* Wallet (from Bitcoin Core #23304): a legacy wallet that has been moved to a new HD seed with `sethdseed` could stop
  the node when it received a payment to some keys of its earlier seed. Such payments are now recorded normally. The
  wallet also derives the next keys of an earlier seed when an encrypted wallet is unlocked or its key pool is refilled,
  so later payments to them are no longer missed, and keys it derives for an earlier seed after the wallet is loaded
  again get the right derivation path. It no longer records MWEB keys of its current seed under an earlier seed.
* PSBT (Bitcoin Core #29855): a malformed PSBT could stop the node in one of the PSBT RPCs. Such a PSBT is now
  rejected when it is decoded.
* Block index (Bitcoin Core #35070): a block waiting for the data of an earlier block could be recorded twice as
  waiting. When that data arrived, the block was processed twice, which could leave the node's set of candidate chain
  tips inconsistent. Each waiting block is now recorded only once.
* Network, builds other than Linux, such as the Windows release (CVE-2026-117350, Bitcoin Core #36665): the node
  watched every peer connection for exceptional conditions, also while it had paused receiving from that peer, and a
  peer could use this to keep the node reading from it. Connections are now watched for these conditions only while the
  node reads from them. Linux builds wait for sockets with `poll()` instead of `select()` and were not affected.
* Network (CVE-2026-117349, Bitcoin Core #36667): a peer whose send buffer stays full for more than 20 minutes is now
  disconnected. Before, a peer that read too slowly to drain its send buffer could keep its connection slot for much
  longer, in some cases indefinitely.
* Network (Ferrite fix): incoming messages are limited to 4 MB again, apart from blocks; see "Network message sizes"
  below.

> An upgrade does not repair an MWEB block of the kind above that an earlier version already stored. A node holding
one may refuse to start, or stop later when it reads that block. Start it once with `-reindex` after upgrading: it then
downloads that block again from a peer that runs v3.3.1. `-reindex` rebuilds the block index and the chain state from
the block files, which can take hours; a pruned node downloads the chain again.

### Changed defaults and settings
#### New ferrite.conf in new data directories
When Ferrite Core starts on a data directory that has no `ferrite.conf`, it still writes one, but the file now holds
only comments and the `addnode` lines: the main network's under `[main]` and the test network's under `[test]`. The
`addnode` lines name only the project's own nodes; the file that earlier versions wrote also named nodes run by others,
three of them in the file of v3.3.0, and those lines can be removed from an existing file. Every other setting comes
from the built-in defaults. Compared with the file that v3.3.0 and earlier versions wrote, a new install:
* uses cookie authentication for RPC: no RPC user name or password is written, and `ferrite-cli` on the same computer
  logs in through the `.cookie` file, also during the first run;
* runs ferrite-qt without an RPC server unless you add `server=1` (`ferrited` always runs one);
* does not run `ferrited` in the background (add `daemon=1` on Linux or macOS if you want that; Windows does not
  support it);
* keeps no transaction index (add `txindex=1` if you need one);
* follows the pruning, incoming-connection and UPnP choices made in ferrite-qt;
* uses the default limits: 125 connections, 4 RPC threads, a wallet fee limit (`-maxtxfee`) of 500 FEC and the default
  OP_RETURN relay size (`-datacarriersize`);
* starts on regtest and signet with the file in place.

> Existing installs: the file is written only when none exists, so your `ferrite.conf` is not changed.
> * Files written by earlier versions (since v2.1.1) contain an `rpcuser` and an `rpcpassword` line with the same
>   values on every install. Remove both lines (`ferrite-cli` then uses the cookie file), or replace them with your own
>   `rpcauth` line.
> * Depending on the version that wrote them, these files also set `daemon=1`, `server=1`, `txindex=1`, `listen=1`,
>   `upnp=1`, `rpcallowip=127.0.0.1`, `rpcthreads=32` and `maxconnections=500` (since v2.1.1), `prune=0` (since
>   v2.1.2), `blockfilterindex=basic` and `maxtxfee=1000` (since v3.1.2), and `datacarriersize=10000` (v3.2.0 and
>   v3.3.0). These lines override the defaults and the ferrite-qt options; remove any you did not choose. On Windows,
>   remove every `daemon=1` line, the one in the `[test]` section included.
> * Removing `prune=0` lets the pruning option of ferrite-qt take effect, if it is on. A pruning node does not start
>   with `txindex=1` or `blockfilterindex=basic`; without them it deletes old block files, and a wallet can then no
>   longer be rescanned over the deleted blocks.
> * Files written by v2.1.1 to v3.1.0 also contain `rpcconnect=500`, with which `ferrite-cli` cannot reach the node
>   ("Could not connect to the server 500:9573"). Remove it.
> * Files written since v3.0.1 end with a `[test]` section, so a line added at the end of the file applies only to the
>   test network. Put such lines near the top instead.

#### Unrecognized ferrite.conf sections
A section header that names no known network, such as `[testnet]` instead of `[test]`, stopped the node at start-up
with an internal error. The node now prints the warning `<file>:<line> Section [<name>] is not recognized.` and starts,
as Bitcoin Core does.

#### Data carrier (OP_RETURN) size: `-datacarriersize`
The node now relays and mines transactions with a data carrier (OP_RETURN) script of up to 100,000 bytes by default, up
from 83 bytes, as Bitcoin Core does since v30. The limit counts the whole script, not just the data in it, as before,
and the `-datacarriersize` help text now says so. It applies both to OP_RETURN outputs and to the script of an MWEB
peg-out. With the new default, the standard transaction weight limit is what limits such a script, to just under
100,000 bytes. A standard transaction may still carry only one OP_RETURN script among its outputs and peg-outs. A
`ferrite.conf` written by v3.2.0 or v3.3.0 contains `datacarriersize=10000`, which keeps a 10,000-byte limit; remove
the line to use the new default. Use `-datacarriersize=83` to keep the old limit, or `-datacarrier=0` to relay and mine
no data carrier transactions. This changes relay and mining policy only, not which blocks are valid.

#### Network time
The time offset that the node applies to its own clock (`getnetworkinfo` "timeoffset", also shown by
`ferrite-cli -getinfo`) is now taken only from outbound peers, the connections the node opens itself, as in Bitcoin Core
23.0 and later (Bitcoin Core #23631). Inbound peers no longer change it and no longer count toward the warning to check the
computer's date and time. Everything that uses the node's adjusted time, such as the time of new block templates,
follows the same offset. `getpeerinfo` still shows each peer's own time offset. The help text of `-maxtimeadjustment`
now says that only outbound peers influence the node's time (Bitcoin Core #24609).

#### Network message sizes
The node again limits incoming network messages to 4 MB, as Bitcoin Core does; v3.3.0 accepted every message of up to
32 MB. The exception is a block or compact block message from a peer that has completed the version handshake: it may
be up to 32 MB, because a block with MWEB data can be larger than 4 MB. The large messages that inbound peers are still
sending also share a total of 128 MB; messages from outbound peers do not count toward it. A peer whose message exceeds
one of these limits is disconnected. Relay of large MWEB blocks is unchanged.

#### Built-in peer list
The fixed seeds, the addresses a node falls back to when it knows no peer addresses a minute after starting, still held
Litecoin's nodes. They now hold the project's nodes on mainnet (see `contrib/seeds/nodes_main.txt`). Testnet has no
fixed seeds: a testnet node finds peers through the DNS seeds, `-addnode` or `-seednode`. The DNS seeds are unchanged.

### Wallet
Not changed by this release: a wallet restored from its seed, or from a backup older than the spend of one of its MWEB
coins, can show spent MWEB coins as unspent, and a rescan does not correct this. Keep a backup of the wallet made after
your latest MWEB transaction. See "Restoring a wallet that holds MWEB coins" in the v3.3.0 release notes.

### Checkpoints
Added a checkpoint at height 1,089,000; the last checkpoint of v3.3.0 was block 1,000,000. Once a node has a checkpoint
block, it rejects any chain that forks from the known chain below that block's height.
Assumed-valid block and minimum chain work updated to block 1,089,000 (v3.3.0: block 1,046,922). To check these values
on a synced node, run `ferrite-cli getblockhash 1089000`, which should print
`849fc31014b955c1adcdc9c5757156996fd4318730c9d5bf13db65830353ae8a`, and `ferrite-cli getblockheader` with that hash,
whose `chainwork` should be `0000000000000000000000000000000000000000000000006b03e4a31762f086`.

### Miscellaneous
Version increment
New unit and functional tests for the changes above; GitHub Actions CI runs them
RPC: no command or argument changed. Every PSBT RPC (`decodepsbt`, `analyzepsbt`, `combinepsbt`, `finalizepsbt`,
`joinpsbts`, `utxoupdatepsbt`, `walletprocesspsbt`) now returns error -22
"TX decode failed Input specifies output index that does not exist: iostream error" for a PSBT input that refers to an
output its non-witness UTXO does not have.
`signrawtransactionwithkey` and `signrawtransactionwithwallet` return the Taproot inputs described under Security
Updates without their witness, with `complete` false and an `errors` entry; `combinerawtransaction` and the `sign`
command of `ferrite-tx` return them without their witness and give no error. With `SINGLE`, an input that has no output
of the same index is not signed and keeps its witness. For a block or transaction that holds one of the unusual MWEB
kernels described there, the hex now includes the kernel's empty field and `size` grows by one byte per such field;
hashes, kernel IDs, `strippedsize`, `vsize` and `weight` do not change
Legacy wallets: `keypoolrefill`, `walletpassphrase` and every key-pool top-up also derive keys of inactive HD chains;
these keys are not counted in `getwalletinfo` "keypoolsize"
P2P: with `-debug=net` the node logs `send buffer full for <n>s, disconnecting peer=<id>` and
`HEADER ERROR - LARGE MESSAGES TOTAL` when it disconnects a peer for these reasons, and `HEADER ERROR - SIZE` also for
messages over 4 MB that may not be that large

For more details see https://github.com/koh-gt/ferrite-core/pull/67

## Future changes
v4.0.0 soft fork: post-quantum (P2QRH) outputs, under evaluation
Litecoin Core updates

## Mining
[**List of pools and settings**](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
Ferrite runs on Scrypt algorithm -compatible with Litecoin/Dogecoin miners
**Scrypt is not an ASIC-resistant algorithm.**
Scrypt ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).
**Pools should run v3.3.1 now**: its security updates apply at once, without signalling. Mine with the block version
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
* v3.3.1: security and stability fixes for v3.3.0 in https://github.com/koh-gt/ferrite-core/pull/67

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.3.0...v3.3.1
