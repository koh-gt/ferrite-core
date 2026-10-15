# Ferrite Core v3.3.0

Security and maintenance release: Litecoin Core's MWEB incident fixes (up to v0.21.5.8), a scheduled
MWEB soft fork, new checkpoints and build fixes. **Miners and pools must upgrade before block
1,150,000.**

<!-- Downloads: add the Windows / macOS / Linux table with SHA256 hashes when the binaries are built. -->

## Upgrade notice: MWEB soft fork at block 1,150,000

From block **1,150,000** (expected between 24 and 29 December 2026 at current block times), two MWEB
kernel encodings become invalid:
* a kernel that sets the peg-out feature but carries no peg-out;
* a kernel that sets the extra-data feature but carries no extra data.

v3.3.0 already refuses to relay or mine such kernels. A pool still running v3.2.x after block
1,150,000 could mine a block that v3.3.0 nodes reject, so **all miners and pools must run v3.3.0
before that height**. No block in Ferrite's history uses either encoding.

Other nodes and wallets should upgrade too; they keep working on v3.2.x, but without the security
fixes below.

## Security fixes (from Litecoin Core v0.21.5.4 – v0.21.5.8)
* MWEB inputs must match the outputs they spend, kernel lock heights cannot exceed the block height
  (from block 903,321), and HogEx / peg-in / fee-overflow checks are stricter.
* Better handling of mutated MWEB blocks and compact blocks; peers that send them are discouraged.
* Rate limits for MWEB leafset requests, fixes for relay censorship through `recentRejects` and for
  fast relay before a block is connected, eviction of conflicting MWEB transactions, and a memory
  safety fix in script checking.
* CVE-2024-35202 (compact block reconstruction assertion).

**No blacklist.** Unlike Litecoin, Ferrite freezes no outputs and grandfathers no block. A scan of
Ferrite's complete MWEB history (blocks 150,120 to 1,057,006) found no block that breaks the new
rules.

## Other changes
* Checkpoints at 450,000, 602,214 (2nd halving), 750,000, 903,321 (3rd halving) and 1,000,000;
  assumed-valid block and minimum chain work updated to block 1,046,922.
* Mempool policy: MWEB transactions with too many inputs (`mweb-txn-too-many-inputs`), too much
  weight (`mweb-txn-oversize`), invalid output keys or empty peg-out kernels (`non-standard-mweb-tx`)
  are no longer accepted or relayed.
* RPC: no command or argument changed. `getmempoolentry` returns an error for MWEB-only
  transactions on nodes started with `-rpcserialversion=0` or `1`. `maxfeerate=0` in
  `sendrawtransaction` / `testmempoolaccept` now means "no limit" for MWEB transactions too.
  `dumpwallet` files gain a `# mweb view keys:` comment line.
* `getblocktemplate` still accepts `{"rules": ["segwit"]}`. As before, the template always carries
  the MWEB block, and pools must include it in the blocks they mine.
* New tool: `contrib/devtools/mweb_history_check.py`, a read-only scan of a node's MWEB history.
* Build and test fixes: the source builds again, and the unit and functional test suites run.

## Downgrading
v3.3.0 records an extra flag in the block undo files (`rev*.dat`). If you go back to an older
version after running v3.3.0, start it once with `-reindex-chainstate`.

## Notes
* Windows: `ferrited` has no daemon mode. Remove `daemon=1` from `ferrite.conf` (or `-daemon` from
  the command line); otherwise it stops with "-daemon is not supported on this operating system".
* Details of every change: `doc/ferrite-3.3.0-review.md`.

## Mining
[**List of pools and settings**](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
Ferrite runs on the Scrypt algorithm, compatible with Litecoin/Dogecoin miners.

## Future changes
v4.0.0 (planned hard fork, under discussion): tail emission, difficulty adjustment, post-quantum
outputs.

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.2.0...v3.3.0
