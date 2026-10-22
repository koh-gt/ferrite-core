# Ferrite Core v3.3.0: review of the v3.2.1 draft, fixes and suggestions

| | |
|---|---|
| Branch reviewed | `ferrite-backup-3.2.1-draft` at `9406190` (160 commits ahead of `ferrite-main`, 17 behind) |
| Release | **v3.3.0** (maintainer's decision, 2026-10-15); MWEB soft fork signalled from block **1,250,000** (decision 2026-10-16) |
| Added by this review | fixes (§2), the Litecoin v0.21.5.6–v0.21.5.8 port (§5), clean-ups; listed in Appendix B |
| Merge base with `ferrite-main` | `fa52e12` (2024-12-20, "add h250000 graphic") |
| Upstream reference | Litecoin Core `v0.21.3`, `v0.21.4`, `v0.21.5.4` … `v0.21.5.8` (github.com/litecoin-project/litecoin) |
| Review date | 2026-10-15 |

Items marked **MANUAL REVIEW** need a decision or data that this review could not produce on its own.
Nothing in this document changes consensus on mainnet except where it says so explicitly.
The commands the maintainer needs (mainnet node queries, pushing the branch) are collected in §12.

---

## 1. Summary

1. **Why the build failed.** Two copy/paste errors made the tree uncompilable:
   `Block.h` lost a closing brace (`001c907`), and `mweb_node.cpp` contained the same block twice,
   redeclaring a `const bool` (`129c6e7`). Both are fixed. `ferrited`, `ferrite-cli`, `ferrite-tx`,
   `ferrite-wallet` and `test_ferrite` now build with `make` exit code 0.
2. **Is the port sane?** Mostly yes. Every one of the 160 commits was traced to its upstream source
   (Appendix A), and every changed file was compared with an "ideal port" (upstream v0.21.5.5 with
   Ferrite's own customisations re-applied, §3). Apart from the two build breaks, the manual merges
   silently **undid upstream fixes in three places** (consensus error handling, MWEB block assembly,
   a regression test that never ran) and broke the functional test framework. All of these are fixed
   in this branch (§2).
3. **No blacklist.** Litecoin v0.21.5.4+ freezes one MWEB output on Litecoin's chain. The draft carried
   that machinery with placeholder values (`0x…0fec`). Placeholders cannot match a real output or
   block, so nothing was frozen in practice, but the intent is now explicit: the frozen list is empty
   and the grandfather hash is null on mainnet and testnet (§2, P1).
4. **Mainnet history passes the two retroactive MWEB rules (§4).** Upstream's MWEB input-metadata
   check and the kernel lock-height check apply to historical blocks. The maintainer ran
   `contrib/devtools/mweb_history_check.py` on a synced v3.2.0 node: the whole MWEB history
   (heights 150,120 to 1,057,006) has no violation, so Ferrite needs no grandfathered block. A
   `-reindex-chainstate` with the release build remains the final check before tagging (§4, §12).
5. **Upstream v0.21.5.6 – v0.21.5.8 is now ported (§5).** The range has 26 upstream commits (an
   earlier draft of this review said 32): 22 code commits were cherry-picked with `-x`, and the 4
   Litecoin-only ones (three version bumps and a DNS seed) were skipped. Two of the ported rules are
   new soft forks. Miners activate them by signalling: on mainnet, v3.3.0 signals on version bit 5
   from block **1,250,000** (expected 8–19 March 2027), and the rules apply once 39 of 40 blocks in a
   window signal, from block 1,250,080 at the earliest. Nothing forces them (§5.2).
6. **RPC contract (§6).** No RPC was added or removed, no argument changed, and the `help` output of
   every command is identical to `ferrite-main`. Five behaviour differences (one new error, one
   nullable field, one bug fix and two test-chain/wallet-file details) and one output addition, the
   new `mwebkernel` soft fork in `getblockchaininfo` and `getblocktemplate` (R6), are listed for
   **MANUAL REVIEW**. R1–R5 do not change a default mainnet configuration's request or response
   shape; R6 adds one key to `getblockchaininfo`'s `softforks`.
7. **Checkpoints 450,000 / 602,214 / 750,000 / 903,321 / 1,000,000 are added (§8, `eef98f2`).**
   The values come from the maintainer's synced node, and each header was verified independently
   (hash and scrypt proof of work) before it was written. `nMinimumChainWork`, `defaultAssumeValid`
   and `chainTxData` moved from block 292,000 to block 1,046,922.
8. **Long-standing test breakage on `ferrite-main`** was found while running the tests. Since
   2023-02-06 the regtest genesis block has failed its own proof of work, so no regtest node could
   start. Regtest difficulty also exploded after height 26 under DGW. The unit-test binary has not
   linked since 2023-07-30, and the functional-test framework has not imported since 2024-03. All four
   are fixed (test infrastructure and regtest only). §7 has what the test suites now report.
9. **Merging into `ferrite-main`:** pull request #65 is open, with GitHub Actions CI (§9). `git merge-tree`
   shows no conflicts. Recommended gates are in §9.
10. **Build tooling** that is still broken but out of scope here (missing executable bits, depends
    Boost recipe, BDB instructions) is listed in §10.
11. **Roadmap** analysis for a 1 FEC tail emission, difficulty-algorithm changes, post-quantum
    options and a CPU-oriented proof of work is in §11. This release is **v3.3.0** (§11.7), with
    draft release notes in `doc/ferrite-release-notes/release-notes-3.3.0.md`.

---

## 2. Defects found and fixed in this review

| ID | Severity | Where | Introduced by | Effect | Fix |
|---|---|---|---|---|---|
| D1 | Build break | `src/libmw/include/mw/models/block/Block.h:57` | `001c907` | `GetSupplyChange()` lost its closing `}`; every translation unit that includes `primitives/transaction.h` failed. | Restored brace (file now identical to upstream). |
| D2 | Build break | `src/mweb/mweb_node.cpp` (`Node::ConnectBlock`) | `129c6e7` | Frozen-output loop and `const bool allow_historical_metadata_mismatch` pasted twice → redeclaration error. | Removed the duplicate (matches upstream). |
| D3 | High (robustness, consensus-adjacent) | `src/validation.cpp` (`CChainState::ConnectTip`) | `5b1a8be` | The pre-v0.21.5.4 line `InvalidBlockFound(pindexNew, state);` survived outside `if (state.IsInvalid())`. On a *local* error (disk/IO, `state.Error()`), a **valid** block would be flagged `BLOCK_FAILED_VALID` and persisted, so the node rejects the real chain until `reconsiderblock`. | Removed the stray call (matches upstream). |
| D4 | High (mining) | `src/libmw/src/node/BlockBuilder.cpp` | `dd6b27e` | Porting "harden amount calculation" pasted an older tail of `AddTransaction()`, reverting `20b012d`: (a) a tx failing `Validate()` was still staged (no `return false`); (b) the guard against staged commitments summing to zero (Litecoin issue #1006 / PR #1017) disappeared. A zero sum makes `secp256k1_pedersen_commit_sum` fail and block-template construction throw. | Restored upstream v0.21.5.5 code. |
| D5 | Medium (tests) | `src/Makefile.test.include` | `7a2cbb1` / `d25b360` | `Test_CoinsView.cpp` (regression test for "Avoid mutating CoinsView state with invalid block", `ff309cda9`) was added but never compiled. | Added to `BITCOIN_TESTS`. |
| D6 | Medium (tests) | `test/functional/test_framework/messages.py` | `eee659f` | Upstream paste re-introduced `import litecoin_scrypt` (module does not exist in Ferrite → every functional test fails at import) and `MAX_MONEY = 84000000`. | `import ferrite_scrypt`, `MAX_MONEY = 60221400 * COIN`. |
| D7 | Medium (tests) | 6 MWEB functional tests | `69c968f`, `a2d084a`, `aa98bc1`, `1b7440e` (and pre-existing in `mweb_mining.py`, `mweb_p2p.py`) | Import `test_framework.ltc_util` (Ferrite's module is `fec_util`); `mweb_duplicate_pegin.py` was committed as `mweb_duplicate_plugin.py`, so `test_runner.py` could not find it. | Imports → `fec_util`; file renamed. |
| D8 | Medium (tests, **pre-existing on main**) | `test/functional/test_framework/fec_util.py` | before `fa52e12` | `setup_mweb_chain(node)` uses `mining_node` → `NameError` in every MWEB functional test. | Parameter renamed. |
| D9 | Medium (tests, **pre-existing on main** since `8bd6583`, 2023-07-30) | `src/wallet/test/init_test_fixture.cpp` | `8bd6583`/`0f13d6c` | `SetWalletDir()` definition was cut off → `test_ferrite` never linked. | Restored upstream file. |
| D10 | Medium (tests, **pre-existing on main** since `a97e96e`, 2023-02-06) | `src/chainparams.cpp` (regtest) | `a97e96e` | With Ferrite's genesis coinbase, nonce 0 does not satisfy scrypt PoW (target ≈ 2²⁵⁵, so roughly a coin flip). Regtest nodes abort with `ReadBlockFromDisk: Errors in block header` on first start. | Nonce 1 (first valid), plus an `assert` on the new hash `66a1fad2…3930` and the stale regtest checkpoint updated. Mainnet and testnet genesis blocks were re-derived independently and are valid. Regtest only; no effect on mainnet/testnet. |
| D11 | Low | `test/functional/test_runner.py` | n/a | `ferrite_scrypt.py` is a helper, not a test; `--ci` would fail on it. | Added to `NON_SCRIPTS`. |
| D12 | Medium (tests, **pre-existing on main** since `e2a6774`/`5a439b8`, 2024-03) | `test/functional/test_framework/p2p.py` | `e2a6774` | `def peer_connect` was indented into the previous method → `IndentationError`; the functional test framework could not be imported at all. | Re-indented (Ferrite log strings kept). |
| D13 | Medium (tests, **pre-existing on main**) | `src/pow.cpp` | DGW introduction | Regtest switches to KGW/DGW at height 26 and neither honours `fPowNoRetargeting`. Regtest blocks are seconds apart, so difficulty rose ~3× per block and mining stalled at height ~27. Every MWEB functional test needs 432 blocks. | Return `powLimit` when `fPowNoRetargeting` is set, as Dash does. Only regtest sets the flag. |
| P1 | Policy (no behaviour change) | `src/chainparams.cpp` | `f6e10cf`, `e1ff9d9`, `190b983` | Placeholder frozen output `0x…0fec` and placeholder grandfather hash. | Frozen list empty; `mweb_input_metadata_grandfather_blockhash = uint256()` (null = no exemption). Plumbing kept so future upstream merges stay easy. |
| P2 | Low (packaging) | `contrib/debian/litecoin-qt.desktop` | `7c91577` | Launched `litecoin-qt`, registered the `litecoin:` URI scheme. | Renamed to `ferrite-qt.desktop`, `Exec=ferrite-qt`, `x-scheme-handler/ferrite`, Ferrite icon. |

### Cosmetic regressions (approved clean-up, done)

- Upstream pastes removed Ferrite copyright lines (`Copyright (c) 2023 The Ferrite Core Developers`)
  from `Test_Kernel.cpp`, `Test_Output.cpp`, `Test_Keychain.cpp`, `mweb_models.h` and
  `mweb_wallet_basic.py`, and renamed the include guard in `mweb_models.h` back to `LITECOIN_…`.
  Restored (`81acda4`).
- `TxBuilder.cpp` (libmw test framework) went back to `ltc_address` naming. Restored (`81acda4`).
- Stray tab indentation and trailing whitespace from web-editor pastes in `wallet.cpp`,
  `validation.cpp`, `chainparams.cpp` (`81acda4`), and in `net_processing.cpp`, `txmempool.cpp`,
  `blockencodings.cpp`, `rpcdump.cpp`, `Kernel.h` and three functional-test files (`1705491`).
  These files now match upstream apart from Ferrite's real changes; `git diff -w` of both commits
  shows nothing else.
- Redundant `-peerblockfilters=0 -blockfilterindex=0` in three functional tests: the draft ported
  upstream `7d9fea0cc` but not its revert `beae01d62`. The revert is now ported (`177aa69`); the
  remaining uses in `p2p_blockfilters.py` and `rpc_misc.py` are upstream's too.
- `depends/patches/miniupnpc/dont_use_wingen.patch` is no longer referenced (upstream deleted it).
  Deleted (`81acda4`).
- The bug-report URL in `configure.ac` read `https://github.comkoh-gt/ferrite/issues` (it appears in
  internal-error messages). Now `https://github.com/koh-gt/ferrite-core/issues` (`40dd9f7`).
  `init.cpp` still names `https://github.com/ferritecoin/ferrite-core` as the source repository;
  **check** that this is the canonical public repository.

---

## 3. How the port was checked

- **Commit → upstream mapping.** For every draft commit, the added lines of every file were matched
  against the added lines of every upstream commit (v0.21.3 → v0.21.5.8) touching that file, after
  normalising Ferrite renames (`fec_util`↔`ltc_util`, `ferrite`↔`litecoin`). Result: Appendix A.
  Nearly every draft commit is a 75–100 % match for one upstream commit. The rest are Ferrite-specific
  and were read individually.
- **Ideal-port comparison.** For each of the 119 changed files:
  `ideal = git merge-file(upstream v0.21.5.5, base = upstream v0.21.3, theirs = Ferrite base fa52e12)`,
  that is upstream's new code with Ferrite's customisations re-applied. The draft was diffed against
  the ideal; non-whitespace differences were inspected one by one. That is how D3–D7 and the cosmetic
  items were found.
- **Upstream completeness.** Each upstream commit's patch was reverse-applied to the draft tree.
  Everything from v0.21.3 → v0.21.5.5 is present (CVE-2024-35202 `PartiallyDownloadedBlock`,
  download-state fix, MWEB incident fixes, …). The only "missing" items were by design:
  `litecoin_scrypt.py` (Ferrite uses `ferrite_scrypt.py`) and upstream's revert `beae01d62`
  (now ported).
- **After the v0.21.5.6 – v0.21.5.8 port** (§5) the ideal-port comparison was repeated against
  upstream v0.21.5.8: every file the port touched equals upstream apart from Ferrite's own
  customisations (names, chain parameters, the relaxed `getblocktemplate` rule check, and the
  Ferrite values in tests).
- **Build and tests**, see §7.

Ferrite-specific commits in the range, all reviewed:

| Commit | Change | Assessment |
|---|---|---|
| `4887eb6` | Checkpoint 292,000; chain stats; min-chain-work / assumevalid at 292,000 | OK (now ~20 months old; refresh in §8) |
| `03bccc7`, `5030ce1` | Version 3.2.1, copyright 2026 | OK (version now 3.3.0, `33352d3`) |
| `e23911c` | Re-applies Ferrite names in `test_framework.py` after the upstream paste | OK |
| `c260888` | `KERNEL_LOCK_HEIGHT_GRANDFATHER_HEIGHT = 903'321` (3rd halving) | **MANUAL REVIEW M2** |
| `f6e10cf`, `e1ff9d9`, `190b983` | Frozen-output / grandfather placeholders | Replaced by P1 |
| `82adf57`, `bfece02` | depends: Boost 1.81 headers only | **Breaks depends builds, B2** |
| `9d9a955`, `4731370`, `391705d` | depends: fontconfig patch changes | **Unverified, B3** |
| doc commits | build guides | See B4/B5 |

---

## 4. Mainnet re-validation of the rules that apply to past blocks

The MWEB incident fixes add rules that are checked for **every block** during IBD, including
historical ones. Checkpoints and `assumevalid` do **not** skip them (`assumevalid` only skips script
checks). Upstream Litecoin handled the one known violating block by grandfathering its hash.
Ferrite exempts no block, which is correct only if Ferrite's own history contains no violation.

| ID | Rule | Upstream commit | Applies to Ferrite blocks | Risk if a past block violates it |
|---|---|---|---|---|
| M1 | MWEB input commitment/pubkey must equal the spent UTXO (`UTXO_MISMATCH`) | `e7cbf1d56`, grandfather `1dcbf3fce` | All MWEB blocks since activation (≈150,120) | v3.3.0 nodes stop at that block (chain split from v3.2.0 nodes) |
| M2 | Kernel lock height ≤ block height | `a54900438`, Ferrite height `c260888` | Heights > 903,321 (already in the past) | Same |
| M3 | No HogEx marker before MWEB activation; HogEx fee/amount mismatch = consensus error; duplicate peg-in guards; kernel fee overflow | `b9bd99a4e`, `7564f05e6`, `8f8ad64d0`, `42e7071a4` | All blocks | Same, though honest history should never trip these |

The v0.21.5.6 – v0.21.5.8 port adds no further retroactive rule: its two new block rules apply only
after miners signal for them (§5.2), and `b4313c34d` only re-runs existing checks.

**Step 1: read-only scan over RPC (any synced v3.2.0 node, minutes to hours).**
`contrib/devtools/mweb_history_check.py` (Python 3.8+, standard library only) walks every block from
MWEB activation to the tip with `getblockhash`/`getblock` and checks M1 (each MWEB input's commitment
and output pubkey against the output it spends, in consensus order) and M2 (kernel lock heights
above 903,321). It also reports, for information, kernels that would break the two future rules of
§5.2. It changes nothing on the node. It was tested on a regtest chain with MWEB peg-ins, transfers
and peg-outs, and against a mock node with injected violations of each rule. Commands: §12.2.

**Result (2026-10-15, maintainer's synced v3.2.0 node):**

```
Scanned heights 150120 to 1057006 (complete MWEB history)
  blocks 906,887   mweb_blocks_with_data 61   inputs 92   outputs 150   kernels 94
  pegout_kernels 33   hogex_pegouts 33   extradata_kernels 0   max_lock_height 0
  unspent_mweb_outputs 58
RESULT: no M1/M2 violations (heights 150120-1057006).
```

- **M1 holds for every MWEB input ever spent on Ferrite (92).** The scan saw all 150 MWEB outputs:
  the `num_txos` count in the MWEB headers at heights 903,321 and later is also 150.
- **M2 cannot fail:** no Ferrite kernel has ever used a lock height (`max_lock_height` 0). The MWEB
  headers at 903,321, 1,000,000 and 1,046,922 also carry the same output count and kernel offset,
  so no MWEB transaction was mined between those heights.
- **X1/P1 (the future rules of §5.2):** no kernel in history would break them.
- The empty grandfather hash and the empty frozen-output list (P1) are therefore correct for
  Ferrite's chain. No consensus parameter change is needed.

**Step 2: definitive re-validation with the release build (before tagging).** Only a full
re-validation exercises M3 and every other consensus rule:

```bash
cp -a ~/.ferrite /data/ferrite-reval          # never experiment on the live datadir
ferrited -datadir=/data/ferrite-reval -reindex-chainstate -daemon
tail -f /data/ferrite-reval/debug.log | grep -E "ConnectBlock .* failed|bad-blk-mweb|mweb-connect-failed|UTXO_MISMATCH|LOCK_HEIGHT|frozen"
# when done:
ferrite-cli -datadir=/data/ferrite-reval getblockchaininfo   # blocks == headers == network tip
ferrite-cli -datadir=/data/ferrite-reval gettxoutsetinfo     # compare with a v3.2.0 node at the same height
```

(Windows: copy `%APPDATA%\Ferrite` to e.g. `D:\ferrite-reval` and start
`ferrited -datadir=D:\ferrite-reval -reindex-chainstate`. `-reindex` instead of `-reindex-chainstate`
additionally re-reads every block file and is slower.)

**If a block fails:**

- M1 failure: the incident's technique was used on Ferrite too. Consistent with the no-blacklist policy,
  set `consensus.mweb_input_metadata_grandfather_blockhash` to that block's hash (exactly what upstream
  did) and **do not** add frozen outputs. Then repeat the re-validation.
- M2 failure: move `KERNEL_LOCK_HEIGHT_GRANDFATHER_HEIGHT` to a **future** height, as upstream did
  (Litecoin chose 3,110,000, a height shortly after its release). Even if M2 passes, a future height
  is the conservative choice: a rule that starts at 903,321 is retroactive, and old v3.2.0 miners keep
  producing blocks without it until the network upgrades.

### M4. `getblocktemplate` without the `mweb` rule (pre-existing Ferrite policy, `332e23b`, 2023-02)

Upstream requires GBT callers to declare both `segwit` and `mweb`. Ferrite commented that out and only
requires `segwit`.

**Decision (maintainer, 2026-10-15): keep the relaxed check.** Several Ferrite pools are not fully
MWEB-aware yet, so v3.3.0 keeps accepting segwit-only callers. `mweb_mining.py` now asserts this
policy (a segwit-only request succeeds and the template still carries the MWEB block) instead of
upstream's strict check (`60c67b0`).

Remaining note for pool operators: since MWEB activated, consensus rejects any block without the
extension block (`mweb-missing`). The template always contains it (the `mweb` field plus the HogEx
transaction as the last entry of `transactions`), and pool software must serialise both into the
block. The relaxed rule only stops `getblocktemplate` from refusing the request; it does not make
blocks without MWEB data valid. The error text for callers without `segwit` still says such blocks
"will neither confirm any MWEB transactions nor receive any MWEB transaction fees". Rewording it is a
possible clean-up, but it is an RPC error string, so it was left unchanged here (§6).

---

## 5. Upstream Litecoin v0.21.5.6 – v0.21.5.8: ported

The range `v0.21.5.5..v0.21.5.8` has 26 commits. The 22 code commits were cherry-picked with
`git cherry-pick -x`, so every Ferrite commit keeps the upstream author and names its upstream hash.
The 4 Litecoin-only commits were skipped: the version bumps `992c5272e` (v0.21.5.6), `b1150f513`
(v0.21.5.7), `ec1b6489a` (v0.21.5.8) and `63ba41f50` (ltcpool DNS seed). Commit hashes in the table are
Ferrite's.

| Ferrite | Upstream | Change | Kind |
|---|---|---|---|
| `05d175c` | `cfdfcf57a` | Enforce Ke and Ko pubkey validity | mempool policy |
| `f8526e9` | `109ed13b7` | Limit MWEB weight for mempool transactions | mempool policy |
| `fb07cc2` | `cb65fc5b6` | Rate-limit MWEB leafset requests | P2P DoS |
| `77da2b8` | `9b5bb289b` | Fix stale values in `mweb_weight.py` | test |
| `f496db2` | `f24dec1c8` | Make the leafset rate limits node-wide | P2P DoS |
| `a7e44c7` | `4dd6aeeb9` | Preserve descendants of mutated MWEB blocks | validation (no new rule) |
| `afc679a` | `136695e05` | Discourage peers that send mutated MWEB compact blocks | P2P DoS |
| `99a50a0` | `67580d6e3` | Reject empty MWEB pegout kernels from standard policy | mempool policy |
| `423e4f2` | `dc7233420` | Enforce the MWEB input limit when building templates | mining |
| `a2ec03b` | `b250b016a` | Kernels with the pegout feature must carry a pegout | **consensus, signalled activation (§5.2)** |
| `43485fd` | `a399fc4ca` | Kernels with the extra-data feature must carry extra data | **consensus, signalled activation (§5.2)** |
| `69a523b` | `22a916236` | Keep the peg-out flag of spent coins in undo data | **undo-data format (§5.3)** |
| `e1b7f54` | `32413709d` | Fix script-check data lifetime on early return | validation (memory safety) |
| `3145150` | `d3b9ac396` | `getmempoolentry`: no MWEB-only transactions when `-rpcserialversion` < 2 | **RPC (§6, R2)** |
| `077847f` | `b4313c34d` | Re-run the MWEB contextual checks on the body actually connected | validation (no new rule) |
| `368d63c` | `d7ab03d31` | secp256k1-zkp: release scratch frames on invalid proofs | hygiene |
| `3abdd69` | `70ea696f3` | Harden same-hash MWEB block mutation handling | validation (no new rule) |
| `2a200a7` | `f6afd2682` | Validate staged block aggregates incrementally | mining |
| `67b13e5` | `36f06455f` | Evict conflicting MWEB transactions when blocks connect | mempool |
| `ec5c012` | `c07d6228c` | Stop MWEB `recentRejects` entries from censoring valid transactions | P2P |
| `713509d` | `0cb141964` | Defer MWEB fast relay until after the block connects | P2P |
| `81746dc` | `b8ccaf355` | libmw: close POSIX copy file descriptors | hygiene (fd leak) |

Ferrite follow-ups in the same series: `130ec36` (the new MWEB tests import `fec_util`, not
`ltc_util`), `177aa69` (upstream's earlier test revert `beae01d62`, see §2), `60c67b0` (M4 policy in
`mweb_mining.py`), `bc56740` (`Test_Kernel` pins Ferrite's activation heights instead of
Litecoin's), `7ce9110` (`MalformedMWEBOutputKeys` uses a Ferrite-encoded key), `28d9c71`
(`contrib/devtools/mweb_history_check.py`, §4), `cba001a` (the two new rules become the signalled
`mwebkernel` deployment, §5.2), and the cosmetic commits `81acda4`, `1705491` and `40dd9f7`.

### 5.1 Conflicts and how they were resolved

| Upstream | Conflict | Resolution |
|---|---|---|
| `4dd6aeeb9` | `mweb_p2p_mutated_block_submitblock.py` imports | Ferrite's `fec_util` names kept |
| `b250b016a`, `a399fc4ca`, `b4313c34d` | `chainparams.cpp`: Litecoin's activation heights, grandfather hash and frozen-output list | New parameters added with Ferrite values; the two activation heights were later replaced by the `mwebkernel` deployment (§5.2). P1 (no frozen outputs, no grandfathered block) unchanged |
| `f6afd2682` | `BlockBuilder.cpp` whitespace around D4's restored guard | Upstream text taken; the file is now identical to upstream v0.21.5.8 |

### 5.2 Two new soft-fork rules, signalled from block 1,250,000

`b250b016a` and `a399fc4ca` make two kernel encodings invalid: the pegout feature bit with an empty
pegout list, and the extra-data feature bit with empty extra data. Both encodings are malleable
variants of a kernel, which is what the mutated-block fixes above are about. Upstream gates them by
height (Litecoin mainnet: 3,154,440, "one expected week after height 3,150,408" per upstream's comment,
and 3,172,640, which `b4313c34d` moved forward from 3,200,000).

**Decision (maintainer, 2026-10-16): signalled activation, with signalling from block 1,250,000.**
It replaces the fixed height 1,150,000 chosen on 2026-10-15 (`b26fd97`). `cba001a` implements it:

- Both rules form one version-bits deployment, `mwebkernel` (`Consensus::DEPLOYMENT_MWEB_KERNEL`),
  in the same height-based scheme as Taproot (signalling from 450,000) and MWEB (from 150,000).
  The two height parameters that upstream added are gone; `mweb_node.cpp` asks
  `IsMWEBKernelRuleEnabled()`.
- Mainnet: bit 5, `nStartHeight` 1,250,000, `nTimeoutHeight` 99,999,999. Windows are 40 blocks.
  When 39 blocks of a window set bit 5, the next window is locked in, and the rules apply from the
  window after it. Earliest schedule: signalling in 1,250,000–1,250,039, locked in for
  1,250,040–1,250,079, rules enforced from block **1,250,080**.
- Nothing forces activation. Ferrite's height-based deployments lock in at their timeout height, and
  99,999,999 (the value Taproot and MWEB use) is far beyond any planning horizon. A window with two
  or more blocks that do not signal does not lock in, so the rules wait until practically all hash
  power runs v3.3.0. **MANUAL REVIEW:** a nearer timeout height would turn this into a deadline
  (lock-in on timeout). None is set.
- Testnet and regtest: always active, as with the fixed height 0 before.
  `-vbparams=mwebkernel:0:0:<start>:<timeout>` switches regtest to signalling.
- Bit 5 was never used on Ferrite (Taproot 2, MWEB 4, testdummy 28) and lies outside the BIP320
  version-rolling bits 13–28 that some mining firmware changes. The checkpoint blocks (§8) all carry
  version `0x20000000`, except block 450,000 (`0x20000004`, Taproot signalling).

**Pools.** v3.3.0 signals without configuration. While the deployment is signalling or locked in,
`getblocktemplate` sets bit 5 in `version` (`0x20000020`), for segwit-only callers too
(`gbt_force`), and lists `"mwebkernel": 5` under `vbavailable`. Once it is active, `mwebkernel`
appears under `rules` without the `!` prefix, so segwit-only callers keep working (M4). A pool
signals only if its software submits the template's `version`. Pool software that hard-codes the
block version (for example `0x20000000`) does not signal; two such blocks in a 40-block window stop
that window from locking in. v3.2.x nodes log "Warning: unknown new rules activated (versionbit 5)" once a
window locks in, and show it in the GUI and RPC `warnings` once the rules are active.

**Why waiting is safe.** Standardness policy keeps both encodings out of v3.3.0 mempools and
templates (empty pegout kernels since `67580d6e3`; any kernel with the extra-data bit was already
non-standard on v3.2.0). Until activation, only a miner who includes such a kernel on purpose can put
one in a block, and that block is valid. A peer cannot use the encodings to get an honest block
marked invalid in the meantime: the colliding body from
`ContextualCheckBlock_KernelSerializationCollisionIsMutated` fails the pegout match and is classified
as mutated (`bad-blk-mweb`), so the block hash is not cached as invalid. `cba001a` adds that
pre-activation case to the test.

**Expected dates** (tip 1,057,002 on 2026-10-15 21:32 UTC; 64.3 s to 69.4 s per block, measured on the
maintainer's node):

| Height | Event | Blocks after 1,057,002 | Expected date |
|---:|---|---:|---|
| 1,204,428 | 4th halving (12.5 → 6.25 FEC) | 147,426 (~15.7 weeks) | 2027-02-02 .. 2027-02-11 |
| 1,250,000 | signalling starts | 192,998 (~20.6 weeks) | 2027-03-08 .. 2027-03-19 |
| 1,250,080 | rules enforced, at the earliest | 193,078 | about 1.5 hours after 1,250,000 |

Tests: `Test_Kernel` (`FeatureActivationDeployment_Test`) pins the deployment parameters on all three
networks. `Test_BlockValidator` runs both rules with the deployment always active and never active.
`mweb_kernel_activation.py` walks a regtest chain through every state with two nodes, one of which
does not signal. It checks that a window with 37 non-signalling blocks out of 144 does not lock in,
and that `getblocktemplate` signals for segwit-only callers. It also checks that the rules start
exactly at the activation block: a committed empty-feature kernel is refused at the signature check
one block before and with `bad-mweb-empty-pegout` / `bad-mweb-empty-extradata` from that block on.
Finally, the node that never signalled keeps mining valid blocks.

`mweb_history_check.py` reports past kernels that would break either rule (X1/P1). They are harmless
for the history, since the rules only start after activation, but any recent hits mean someone is
producing such kernels on Ferrite.

### 5.3 Undo-data format change (release-notes item)

`22a916236` stores the peg-out flag of spent coins in the block undo files (`rev*.dat`, bit 31 of
the height code). A v3.2.0 binary reading undo data written by v3.3.0 decodes a wrong height for spent
peg-out coins if it disconnects such a block in a reorg. The release notes say: after running
v3.3.0, a downgrade needs `-reindex-chainstate` to be safe.

---

## 6. RPC contract (requested: input and output shapes unchanged)

**Method.**
1. Source: every RPC-facing file (`src/rpc/*`, `src/wallet/rpc*.cpp`, `src/core_write.cpp`,
   `src/rest.cpp`, `src/zmq/*`, `src/httprpc.cpp`) was diffed between `ferrite-main` and this branch.
   Five files differ.
2. Runtime: `ferrited` from `ferrite-main` (`6b1c373`) and from this branch were started, and
   `help <command>` was captured for every command. That covers arguments, argument types and
   defaults, documented result fields, and examples. The two dumps were compared.

**Result: same 139 commands on both builds, and the `help` output of every command is byte-identical**
(both dumps have MD5 `2a66b2212bc5fb8bcf7cba450b656d13`). No argument, type, default or documented
result field changed. Re-checked on the final v3.3.0 build, including `cba001a`: still identical (the
`softforks` help is generic, so the new deployment does not change it).

The source differences, all marked **MANUAL REVIEW** because they change behaviour, not shape:

| ID | RPC | Difference | Who can notice | Origin |
|---|---|---|---|---|
| R1 | `getrawmempool` (verbose), `getmempoolentry`, `getmempoolancestors`/`descendants` (verbose) | `mweb.fee` becomes `null` if the MWEB fee overflows. That cannot happen for a transaction that passed mempool acceptance, so in practice the field is always a number. | nobody in practice | upstream `42e7071a4` (kernel fee overflow, draft) |
| R2 | `getmempoolentry` | For an MWEB-only transaction it now returns error −22 "MWEB-only transaction not serializable for rpcserialversion<2" **if** the node runs with `-rpcserialversion=0` or `1`. Default is 2, so default nodes are unaffected. | nodes configured with `rpcserialversion<2` | `d3b9ac396` (§5); drop `3145150` to keep the old behaviour |
| R3 | `getblocktemplate` | On testnet and regtest only, no longer refuses with "not connected" / "in initial sync". Mainnet unchanged. | test networks | upstream `455aff8c1` (draft) |
| R4 | `sendrawtransaction`, `testmempoolaccept` | `maxfeerate=0` now really means "no limit" for transactions with MWEB data, as the help text already says. Before, the MWEB part of the fee could still trip the limit. | callers passing 0 | upstream `8782ab961` (draft); bug fix |
| R5 | `dumpwallet` | The dump file gains one comment line, `# mweb view keys: <scan secret> <spend pubkey>`. `importwallet` skips comment lines, so old and new files import the same way. The file was already full of private keys; treat it as before. | anyone parsing dump files | upstream `eae9e47f9` (draft) |
| R6 | `getblockchaininfo`, `getblocktemplate` | **Output shape:** new soft fork `mwebkernel` (§5.2). `getblockchaininfo` gains `softforks.mwebkernel` on every network; on mainnet it reads `{"type": "bip8", "bip8": {"status": "defined", "start_height": 1250000, "timeout_height": 99999999, "since": 0}, "active": false}` until block 1,250,000, the same shape as the `taproot` and `mweb` entries. Mainnet `getblocktemplate` is unchanged until block 1,250,000. While signalling it adds `"mwebkernel": 5` to `vbavailable` and bit 5 to `version`; once active it adds `"mwebkernel"` to `rules` (no `!` prefix, so segwit-only callers still work). On testnet and regtest the deployment is always active, so `rules` lists it from the start. | software that compares `softforks` or `rules` with a fixed list; pools that hard-code the block version (they do not signal) | this review (`cba001a`) |

Other observable differences, not part of the request/response shape:
- Mempool policy from §5 rejects more MWEB transactions, which `sendrawtransaction` /
  `testmempoolaccept` report with two new reasons, `mweb-txn-too-many-inputs` and
  `mweb-txn-oversize`, and with the existing `non-standard-mweb-tx` (now also for invalid Ke/Ko
  pubkeys and empty pegout kernels). The new block-rule reasons (`bad-mweb-empty-pegout`,
  `bad-mweb-empty-extradata`, `bad-mweb-amount`) only appear in logs and `submitblock` /
  `getblocktemplate` proposal results.
- The bug-report URL in internal-error messages changed (§2).
- `getblocktemplate` still accepts segwit-only callers (M4); its error text is unchanged.

---

## 7. Test results

Environment: Ubuntu 24.04, GCC 13.3, Boost 1.83, system BerkeleyDB 5.3 (`--with-incompatible-bdb`),
`./configure --enable-tests --disable-bench`, with and without `--with-gui=qt5`, Python 3.12 with
`blake3` installed (needed by the MWEB functional tests).

**Build:** `make` exits 0 on the final tree (all commits in Appendix B; `ferrited -version` reports
v3.3.0). The unit and functional results below were re-run on that build and did not change. `ferrited`, `ferrite-cli`,
`ferrite-tx`, `ferrite-wallet`, `test_ferrite` and, with `--with-gui=qt5` (Qt 5.15, QR codes on),
`ferrite-qt` link. The only warnings are GCC 13 `-Wdangling-reference` false positives in
`rpc/mining.cpp`, Boost filesystem deprecations and Qt deprecations, all in unchanged upstream code.

### 7.1 Unit tests (`make check`)

To separate regressions from old problems, `ferrite-main` was built the same way with only the
test-enabling fixes applied (D9, D10, D13, plus the functional-framework repairs) and run through the
same suites. The last column is the final tree, after the v0.21.5.6 – v0.21.5.8 port.

| | `ferrite-main` baseline | Draft + review fixes | Final tree (with the §5 port) |
|---|---|---|---|
| Suites run | 121 | 122 (one more: `Test_CoinsView`) | 122 |
| Suites failing | 15 | 14 | 14 (the same 14) |
| libmw (MWEB) suites | 33 / 33 pass | 34 / 34 pass | 34 / 34 pass |

- **No new failures.** Every suite that fails on this branch fails the same way on `ferrite-main`.
- **One suite fixed by the draft:** `wallet/test/init_tests` fails on `ferrite-main` (wallet directory
  with a trailing separator on Boost ≥ 1.78) and passes here, through the port of upstream
  `3c3aedb97`.
- All MWEB suites pass, including the newly wired `Test_CoinsView` (D5), `Test_Block` (kernel lock
  height), `Test_Kernel` (fee overflow, `mwebkernel` deployment parameters), `Test_BlockValidator`
  (duplicate peg-ins, disk-reparse bypass, kernel serialisation collisions before and after the
  `mwebkernel` activation), `Test_BlockBuilder` (staged
  aggregates, input limit) and `Test_Output`/`Test_Keys` (Ke/Ko validity). `validation_block_tests`,
  which covers `ConnectTip` and the D3 code path, passes after D13; `mempool_tests` (about 400 lines
  of new upstream tests) and `coins_tests` (peg-out flag in undo data) pass.
- Two upstream tests needed Ferrite values to run at all: `FeatureActivationHeights_Test` pinned
  Litecoin's activation heights (`bc56740`; since `cba001a` it is `FeatureActivationDeployment_Test`
  and checks the deployment), and `MalformedMWEBOutputKeys` decoded a Litecoin WIF key,
  so it aborted before checking anything (`7ce9110`, same secret with Ferrite's prefix; 23/23 checks
  pass). That case sits in `scriptpubkeyman_tests`, which still fails as a suite because its older
  `StealthAddresses` case uses Litecoin vectors (see below).
- The final run skipped `versionbits_tests` because of its memory use (§7.3); it fails the same way
  as before.

The remaining failures are all test vectors that were never adapted from Litecoin:

| Suite(s) | Cause |
|---|---|
| `bloom_tests`, `key_tests`, `key_io_tests`, `descriptor_tests`, `rpc_tests`, `util_tests`, `psbt_wallet_tests`, `scriptpubkeyman_tests` | Hard-coded Bitcoin/Litecoin WIF keys and addresses; Ferrite uses different base58 prefixes (pubkey 36, secret 163). In `scriptpubkeyman_tests` the expected `fecmweb1…` addresses were made by replacing the prefix text, so their bech32 checksums are Litecoin's. |
| `validation_tests`, `wallet_tests` | Assume the 50-coin subsidy and 84M supply (Ferrite: 100 FEC, 60.2M). |
| `miner_tests` | Hard-coded `blockinfo[]` nonces mined for Litecoin's parameters (`high-hash`). |
| `pow_tests` | Litecoin retarget vectors; Ferrite uses DGW from 250,000. |
| `versionbits_tests` | Asserts `nTimeoutHeight % nMinerConfirmationWindow == 0`; Ferrite mainnet uses 99,999,999 with a 40-block window. |
| `interfaces_tests` | `TestChain100Setup` assumes `COINBASE_MATURITY = 100`; Ferrite's is 10, so the test reads block 20 of a 10-block chain and crashes (test code, not node code). |

### 7.2 Functional tests (MWEB and every test the draft or the port touched)

Final tree, `test_runner.py -j3`:

| Test | Result | Notes |
|---|---|---|
| `mweb_basic.py` | ✅ pass | after replacing Litecoin's 12.5-coin coinbase expectation with Ferrite's 25 FEC (`7274432`) |
| `mweb_dbcrash.py` | ✅ pass | upstream `bbd3b7841` (RollforwardBlock MWEB state) |
| `mweb_duplicate_pegin.py` | ✅ pass | upstream `8f8ad64d0` (was misnamed, D7) |
| `mweb_kernel_activation.py` | ✅ pass | new in this review (`cba001a`): signalled activation of the §5.2 rules |
| `mweb_mempool.py` | ✅ pass | new in v0.21.5.7 (`d3b9ac396`, R2) |
| `mweb_mining.py` | ✅ pass | asserts Ferrite's segwit-only `getblocktemplate` policy (M4, `60c67b0`) |
| `mweb_newpowvalidblock.py` | ✅ pass | new in v0.21.5.8 (`0cb141964`) |
| `mweb_p2p.py` | ✅ pass | upstream `2ba586ff1` |
| `mweb_p2p_mutated_block_submitblock.py` | ✅ pass | upstream `7eb181bb6`/`17f16cef4`; exercises the D3 code path |
| `mweb_p2p_mutated_compactblock.py` | ✅ pass | new in v0.21.5.6 (`136695e05`) |
| `mweb_pre_activation_hogex_marker.py` | ✅ pass | upstream `b9bd99a4e` |
| `mweb_pegout_all.py`, `mweb_reorg.py`, `mweb_wallet_basic.py` | ✅ pass | |
| `mweb_weight.py` | ✅ pass | fixed by the port (`9b5bb289b` test values, `109ed13b7` mempool weight limit) |
| `mweb_wtxid_recentrejects.py` | ✅ pass | new in v0.21.5.8 (`c07d6228c`) |
| `p2p_invalid_messages.py`, `p2p_blockfilters.py`, `p2p_node_network_limited.py`, `rpc_misc.py`, `wallet_import_rescan.py --legacy-wallet` | ✅ pass | |
| `rpc_blockchain.py` | ❌ | `gettxoutsetinfo` expects Litecoin's 8,725 coins; Ferrite regtest has exactly twice that (100 vs 50 subsidy). Test data, pre-existing. The `softforks` check before it, which now includes `mwebkernel`, passes. |
| `mweb_node_compatibility.py`, `mweb_wallet_address.py`, `mweb_wallet_upgrade.py` | skipped | need previous-release binaries |

**Baseline comparison.** On `ferrite-main` (with the same test-enabling fixes), `test_runner.py` aborts
before running any test: the framework's cache clean-up calls `os.remove()` on the `indexes`
directory. The draft fixes that through its port of upstream `0c59e996e`. Run directly, the MWEB tests
compare as follows:

| Test | `ferrite-main` | This branch |
|---|---|---|
| `mweb_pegout_all.py`, `mweb_reorg.py`, `mweb_wallet_basic.py` | ✅ pass | ✅ pass |
| `mweb_p2p.py` | ❌ does not parse (`IndentationError`, line 45) | ✅ pass |
| `mweb_basic.py` | ❌ Litecoin amounts | ✅ pass (after `7274432`) |
| `mweb_weight.py` | ❌ `bad-txns-vin-empty` (stale test) | ✅ pass (after the §5 port) |
| `mweb_mining.py` | ✅ pass (older test without the GBT-rule assertion) | ✅ pass (Ferrite policy asserted) |

No test passes on `ferrite-main` and fails on this branch.

### 7.3 Test debt worth a dedicated clean-up

The Ferrite test suites still carry Litecoin vectors: base58 prefixes (WIF/addresses), the 50-coin
subsidy and 84M supply, Litecoin regtest nonces in `miner_tests`, Litecoin retarget numbers in
`pow_tests`, and BIP9 timeout assumptions in `versionbits_tests`. This has gone unnoticed because
the unit-test binary did not link and the functional framework did not import. Now that both run,
a Ferrite pass over the vectors would let CI gate every merge (§9).

`versionbits_tests` needs attention first: with Ferrite's mainnet `nTimeoutHeight` of 99,999,999 its
`versionbits_computeblockversion` case builds block indexes towards that height and used more than
11 GB of memory here before it was stopped. On a CI runner or a small machine that ends in an
out-of-memory kill, not a test failure. Run `make check` with that suite excluded until it is fixed.

The cases that still fail are listed one per line in `test/ferrite_unit_test_exclusions.txt` (29
cases in 13 suites), grouped by cause; CI skips exactly those. Some suites abort on their first bad
key, so the list was built by re-running each failing suite until the rest passed: it contains more
cases than the per-suite failures in §7.1 show. With the list applied, one `test_ferrite` process
runs the other 498 cases (about 7 million assertions) in about 40 seconds.

---

## 8. Checkpoints (requested): status and procedure

Requested heights:

| Height | Meaning | Subsidy from this block |
|---:|---|---:|
| 450,000 | Taproot signalling start (`DEPLOYMENT_TAPROOT.nStartHeight`) | 50 FEC |
| 602,214 | 2nd halving (`2 × 301,107`) | 25 FEC |
| 750,000 | "Taproot 750k" | 25 FEC |
| 903,321 | 3rd halving (`3 × 301,107`) | 12.5 FEC |
| 1,000,000 | 1M milestone | 12.5 FEC |

(`GetBlockSubsidy` uses `nHeight / nSubsidyHalvingInterval`, so the "halving block" is the first block
with the reduced subsidy: 602,214 and 903,321.)

**Status: added (`eef98f2`).** The review environment could not reach any Ferrite data source, so the
values come from the maintainer's synced v3.2.0 node (2026-10-15, tip 1,057,002). Each header was
rebuilt from the fields `getblockheader` prints (version, previous hash, merkle root, time, bits,
nonce) and checked independently:

| Height | Block hash | Time (UTC) | Difficulty | Hash | scrypt PoW |
|---:|---|---|---:|---|---|
| 450,000 | `3fed4270…c14002b9` | 2025-06-25 14:38 | 5.99 | ✓ | ✓ |
| 602,214 | `2399672f…e05e05f1` | 2025-10-24 14:16 | 3.92 | ✓ | ✓ |
| 750,000 | `5b6bc304…4a0905f5` | 2026-02-14 08:18 | 74.52 | ✓ | ✓ |
| 903,321 | `b0ef59c7…2ddd6115` | 2026-06-11 13:32 | 19.16 | ✓ | ✓ |
| 1,000,000 | `c75f4a86…5b3cc247` | 2026-08-30 23:49 | 12.68 | ✓ | ✓ |
| 1,046,922 (assumevalid, not a checkpoint) | `189f2e59…9f9e652d` | 2026-10-08 09:28 | 165.57 | ✓ | ✓ |

- "Hash ✓": the double-SHA256 of the rebuilt 80-byte header equals the hash the node reported.
  "scrypt PoW ✓": its scrypt(1024, 1, 1) hash meets the target in its own `nBits`, and the
  reported difficulty matches `nBits`.
- The `confirmations` values of the five checkpoint blocks all imply the same tip (1,056,996), so
  every one is on the node's active chain.
- Chainwork rises between the points at a plausible rate (average difficulty 31–92, average
  spacing 66–72 s per segment).
- A script compared every value written to `chainparams.cpp` with the verified data.
- **Taproot** is active since height **450,080** (`getblockchaininfo`: `bip8`, `since` 450080),
  not 750,000, so the 750,000 checkpoint is labelled as a plain height. MWEB is active since 150,120.
- Ideally a second, independent node or explorer confirms the five hashes before release.

The checkpoint list in `CMainParams` (`src/chainparams.cpp`) now ends:

```cpp
                {   292000, uint256S("0xb42d405513ca11a37bda4420f99577861258a593627401c6143026799e63d1e3")},  // added 20250216
                {   450000, uint256S("0x3fed4270c7fdae13403707a82c4e7ad0bc9d5dec1e4d277ed2e2bf3dc14002b9")},  // added 20261015, Taproot signalling start (active from 450080)
                {   602214, uint256S("0x2399672fc2e4303198f56734ccfe43202e0657de61a92141343f4c16e05e05f1")},  // added 20261015, 2nd halving (25 FEC)
                {   750000, uint256S("0x5b6bc3044a65a0a6625275b0bf0eaa4f080de0dc1b9431cc82ca1f434a0905f5")},  // added 20261015
                {   903321, uint256S("0xb0ef59c7fc0e42672d2b660cee3f00dbb84ca90e80a4ddf0aaad3f4f2ddd6115")},  // added 20261015, 3rd halving (12.5 FEC)
                {  1000000, uint256S("0xc75f4a8694eb7ef8b398bef7510feb37495e9c6994ae3be0594190295b3cc247")},  // added 20261015
```

The values that were still at height 292,000 (Feb 2025) now come from block `H` = 1,046,922
(tip − 10,080): `nMinimumChainWork` = chainwork of `H` (`…6aefe2248ff21e3f`, 7.706 EH, below the
tip's `…6af9b4da644182b8`), `defaultAssumeValid` = hash of `H`, and `chainTxData` = `getchaintxstats
4096 <hash of H>` (time 1791451691, 2,023,430 transactions, 0.0297 tx/s). `nMinimumChainWork` must
come from a real block of the best chain: a value above the real chain's work keeps every node in
initial block download, and mainnet `getblocktemplate` refuses to work then. A fresh mainnet node
built from this branch starts with these values (checked offline). Sync a fresh node from genesis
with the release binary before release.

If Taproot is active, consider *burying* the deployment (a fixed activation height, as Bitcoin did
for older soft forks) in a later release. That simplifies code and removes signalling edge cases.
`getblockchaininfo` reports the activation height.

---

## 9. Merging into `ferrite-main`

- `git merge-tree --write-tree origin/ferrite-main ferrite-backup-3.2.1-draft` reports **no conflicts**,
  re-checked with every commit in Appendix B. The 17 commits only on `ferrite-main` are
  README/translation updates plus "Safer MWEB logger initialization" (`6b1c373`), which is identical
  to the draft's `7b0ec43`.
- Suggested gates before merging:
  1. CI green on pull request #65 (the workflow is described below).
  2. §4 mainnet re-validation: the RPC scan (step 1) passed on 2026-10-15; the
     `-reindex-chainstate` with the release build (step 2) remains before tagging.
  3. Activation of the two new MWEB rules: decided, signalled from block 1,250,000 (§5.2). Tell the
     pools to run v3.3.0 and to mine with the block version from `getblocktemplate`.
  4. Checkpoints and chain statistics refreshed (§8): done; confirm the five hashes on a second,
     independent node or explorer.
  5. A depends build verified for each release platform (B2/B3).
  6. Release notes: drafted in `doc/ferrite-release-notes/release-notes-3.3.0.md` (MWEB fixes,
     the signalling schedule and upgrade notice, "no frozen outputs", the `getblocktemplate` policy,
     RPC differences, the undo-format note). Add the download table and SHA-256 hashes when the
     binaries are built, and check the "Full Changelog" link (it assumes a `v3.2.0` tag).
- Use a merge commit (`--no-ff`) rather than a squash. The 160-commit history is already public on the
  backup branch, and Appendices A and B refer to those hashes.

**CI (`.github/workflows/ci.yml`, added in `ef4e6df`).** GitHub Actions on `ubuntu-24.04`, for pushes
to `ferrite-main`, `ferrite-backup-*`, `fec-v*` and tags, and for every pull request:

- **build-and-test:** build without the GUI, then run every unit test case except the 29 that still
  carry Litecoin test vectors (listed in `test/ferrite_unit_test_exclusions.txt`, §7.3), then run 21
  functional tests: all MWEB tests plus `p2p_invalid_messages`, `p2p_blockfilters`,
  `p2p_node_network_limited`, `rpc_misc` and `wallet_import_rescan --legacy-wallet`.
- **gui:** build `ferrite-qt` with Qt 5.
- Both jobs cache ccache output. The steps were run on a fresh clone in an Ubuntu 24.04 container
  before the workflow was pushed.

As the Litecoin vectors are adapted, delete their lines from the exclusion file; add the remaining
functional tests (for example `rpc_blockchain.py`) once their data is fixed.

---

## 10. Build and tooling issues (not fixed here)

- **B1. Executable bits are missing repo-wide.** *Fixed in `40dc178`: the 259 scripts upstream marks
  executable, plus `mweb_history_check.py` and `fec_replacebyfee.py`, are executable again, and a
  fresh checkout builds without `chmod`. The text below is kept for reference.* `ferrite-main` and the draft have 0 files with mode
  `100755` (upstream has 274), apparently because files went through GitHub's "upload files"
  feature. `autogen.sh`, `share/genbuild.sh`, `contrib/install_db4.sh`, depends scripts and every
  functional test are affected, hence the `chmod -R +x` step in `build-unix.md`.
  Fix once from a local clone that has fetched a Litecoin tag:

  ```bash
  git ls-tree -r <litecoin-v0.21.5.8> | awk '$1=="100755"{print $4}' |
    while read -r f; do [ -f "$f" ] && git update-index --chmod=+x -- "$f"; done
  git update-index --chmod=+x -- test/functional/*.py   # Ferrite-only tests too
  git commit -m "Restore executable bits"
  ```
- **B2. `depends/packages/boost.mk` (`82adf57`) only copies Boost 1.81 headers.** This code base still
  links `Boost.Filesystem`, `Boost.Thread`, `Boost.System` (+`unit_test_framework` for tests);
  `configure.ac` checks them with `AX_BOOST_FILESYSTEM` etc. Every depends-based build (Windows
  cross-compile per `build-windows.md`, static/Gitian/Guix releases) will therefore stop at
  `configure`. Restore a compiled recipe. One option is the original 1.70 recipe with the dead
  Bintray URL replaced by `https://archives.boost.io/release/1.70.0/source/` (same file name and
  SHA-256). Another is a 1.81 recipe that still runs `bootstrap.sh`/`b2` with
  `--with-libraries=filesystem,system,thread,test`. **Not tested here** (no network access to the
  tarballs).
- **B3. fontconfig (`9d9a955`, `4731370`, `391705d`).** The version is still 2.12.6, but the rewritten
  `gperf_header_regen.patch` has context lines (`--pic`, `fcobjshash.h: Makefile fcobjshash.gperf`)
  that look like a newer fontconfig's `Makefile.in`, and `remove_char_width_usage.patch` was dropped.
  **MANUAL REVIEW:** run `make -C depends fontconfig` on a clean checkout; if the patch does not apply,
  restore the upstream versions of both patches.
- **B4. BerkeleyDB 4.8 instructions (`build-unix.md`).** Renaming only line 179 of `dbinc/atomic.h`
  leaves the `atomic_compare_exchange` macro still calling GCC's builtin. The depends recipe and
  `contrib/install_db4.sh` both apply `clang_cxx_11.patch`, which renames the definition *and* the
  macro. Point the guide at `contrib/install_db4.sh` (it also checks the tarball's SHA-256; the Oracle
  URL in the guide is plain `http://`), or use
  `sed -i 's/__atomic_compare_exchange/__atomic_compare_exchange_db/g' dbinc/atomic.h`.
- **B5. `libfmt` (note only).** libfmt is a real dependency: libmw's `mw/util/StringUtil.h` uses
  `fmt::format`, `configure.ac` checks for it exactly as upstream does, and `depends/packages/libfmt.mk`
  provides it for depends builds. The guide's build-from-source step is only needed for fully static
  non-depends builds; `libfmt-dev` is enough otherwise. Saying so in the guide would save people a
  step.
- **B6. Documentation accuracy.** README says "based on Bitcoin Core 23.0". The code base is Litecoin
  Core 0.21, i.e. Bitcoin Core 0.21. The `nSubsidyHalvingInterval` comment says "8 months", but 301,107
  blocks at 60 s is about 209 days (≈ 6.9 months).
- **B7. Workflow.** Most defects came from editing single files in the GitHub web UI ("Update X.cpp").
  A local clone with `git cherry-pick -x`, one PR per upstream release, and CI would avoid them.

---

## 11. Roadmap analysis

### 11.1 Tail emission (1 FEC per block)

Current schedule (`GetBlockSubsidy`: 100 FEC, halving every 301,107 blocks):

| Epoch | Starts at height | Subsidy | Supply at end of epoch |
|---:|---:|---:|---:|
| 3 | 903,321 | 12.5 | 56,457,562 |
| 4 | 1,204,428 | 6.25 | 58,339,481 |
| 5 | 1,505,535 | 3.125 | 59,280,441 |
| 6 | 1,806,642 | 1.5625 | 59,750,920 |
| 7 | 2,107,749 | 0.78125 | 59,986,160 |

Asymptotic supply is 60,221,400 FEC, exactly `MAX_MONEY`.

**Design.** The least disruptive form is a floor, `nSubsidy = max(scheduled, 1 FEC)`, which changes
nothing until height 2,107,749 (epoch 7). The alternative, a fixed 1 FEC from an earlier height, cuts
miner revenue abruptly. At 60 s blocks the tail adds 525,600 FEC/year (0.87 % of 60.2 M in year one,
falling over time).

**Consensus touch-points:**
- `GetBlockSubsidy()` (`validation.cpp`): apply the floor from the activation height, and remove the
  `halvings >= 64 → return 0` early exit.
- **`MAX_MONEY` must be raised in the same fork.** With the floor, total supply passes 60,221,400 about
  0.9 years after the tail starts. `MoneyRange()` guards single values and sums: transaction outputs,
  fee sums, and the MWEB HogEx balance (`mweb_amount`, checked in `mweb_node.cpp`). Dogecoin, for
  example, uses a cap far above circulating supply. Anything under 92 billion FEC fits in an `int64`.
  Also update `test/functional/test_framework/messages.py`, wallet/GUI amount limits, and
  `subsidy_limit_test`.
- This is a **hard fork**: old nodes reject coinbases above their schedule. Activate at a fixed height
  announced months ahead; pools, exchanges and explorers must upgrade.
- Tests: subsidy unit tests around the activation height and epoch 7, a regtest functional test
  mining across the boundary, and a supply-accounting check (`gettxoutsetinfo`).

### 11.2 Difficulty adjustment

Current state (`src/pow.cpp`): Litecoin-style retargeting every 10 blocks (4× clamp) before 250,000,
then Dark Gravity Wave v3 (24-block window, ×3 clamp). `KimotoGravityWell()` is dead code on mainnet
(`nPowKGWHeight == nPowDGWHeight`).

Observations:
- `MAX_FUTURE_BLOCK_TIME` is still Bitcoin's **2 hours**, five times DGW's 24-minute window. Future-dated
  timestamps can therefore swing difficulty within the ×3 clamps. Whatever algorithm is chosen,
  shrinking the future-time limit is the cheapest improvement. Note that `DEFAULT_MAX_TIME_ADJUSTMENT`
  must stay below it.
- DGW v3's weighting is not a true average (see the in-code comment), and a 24-block window is noisy
  for a small scrypt chain exposed to rented/multipool hash rate.

Options, in order of preference for a small chain:
1. **ASERT** (aserti3-2d, as used by Bitcoin Cash): absolutely scheduled, no oscillation, a single
   half-life parameter (on the order of 100–300 blocks, i.e. roughly 2–5 hours at 60 s; Bitcoin Cash
   uses 288 blocks), integer-only reference implementation and test vectors available.
2. **LWMA-1** (N ≈ 60–90): widely deployed by small coins, robust to hash-rate swings.
3. Keep DGW but tighten the future-time limit.

Before deciding, export `height, time, nBits` for the last ~300k blocks from a node (`getblockheader`
in a loop), plot solve-time distribution and difficulty response, and replay candidate algorithms
against that history. A DAA change is a hard fork. Combining it with the tail-emission fork means
one coordinated upgrade instead of two.

### 11.3 Post-quantum

Threat model (a cryptographically relevant quantum computer, CRQC):
- **ECDSA/Schnorr keys** (secp256k1) become forgeable once the public key is known: P2PK outputs,
  reused addresses, any address whose pubkey appeared in a spend, and **every Taproot key-path output**,
  since P2TR puts the tweaked pubkey directly in the output. Hashed outputs (P2PKH/P2WPKH) whose keys
  were never revealed are safe until they are spent.
- **MWEB is the most exposed part.** Pedersen commitment binding and Bulletproof soundness rest on
  discrete log. A CRQC could forge commitments, which means undetectable inflation *inside* MWEB.
  The damage to the transparent chain is bounded by the HogEx/HogAddr accounting (`mweb_amount`), so
  the realistic failure is theft of pegged-in funds via peg-outs, not unbounded main-chain inflation.
- **scrypt/SHA-256 proof of work** is only weakened quadratically (Grover); not a practical concern.

Options:
1. **Now (no fork):** wallet hygiene: avoid address reuse, prefer P2WPKH over P2TR key-path for
   long-term storage. Document this.
2. **Soft fork:** a new output type committing to a hash of a post-quantum public key, spent with a
   NIST-standardised scheme: ML-DSA/Dilithium (FIPS 204; ~2.4 KB sig, 1.3 KB pubkey at level 2),
   SLH-DSA/SPHINCS+ (FIPS 205; hash-based, conservative, 7.8–17 KB sigs), or FN-DSA/Falcon (smallest,
   but tricky constant-time floating point). This follows Bitcoin's BIP-360 discussion; the key point is
   **no key-path spend**, unlike P2TR. 1-minute blocks with 4 MWU give ample room for larger witnesses.
   Vendor and pin the implementation (e.g. PQClean / liboqs reference code) and treat it as consensus
   code: deterministic, constant-time, NIST ACVP test vectors.
3. **MWEB:** there is no production-ready post-quantum Mimblewimble. Realistic plans are a cap on
   peg-ins, or "peg-out only" mode once a CRQC is credible.
4. **Governance note:** migration plans elsewhere sometimes end in freezing unmigrated ECDSA coins.
   That would contradict Ferrite's no-blacklist position, so decide the policy early and publish it.

### 11.4 Other observations

- `COINBASE_MATURITY` is 10 blocks (Litecoin/Bitcoin: 100), i.e. coinbase can be spent after about
  10 minutes. On a chain that rented hash rate can reorganise, a deeper maturity limits how far
  invalidated coinbase spends can cascade. Raising it is a soft fork (it only tightens a rule).
  If it changes, `TestChain100Setup` and other tests that assume 100 need updating; with 10 they
  currently crash (`interfaces_tests` dereferences block 20 of a 10-block chain).
- **A `/FerriteCore:4.0.0/` node is on mainnet.** One peer of the maintainer's node (2026-10-15)
  advertised that user agent with protocol 70016, which matches the 2024 `v4.0.0-a` branch (built
  on v3.1.5), i.e. older code than v3.2.0. Before a real v4.0.0 hard fork ships, retire those nodes
  or rename that build, so that the user agent unambiguously means fork-ready code. The same sample
  had one `/FerriteCore:3.1.4/` and two `/FerriteCore:3.2.0/` peers, but four peers (two on the
  maintainer's own network) say little about how far v3.2.0 has spread. With signalled activation
  (§5.2), the chain itself shows v3.3.0 adoption from block 1,250,000 on: count the blocks with bit 5.

### 11.5 Changing the proof of work (CPU-oriented, "post-quantum")

**Quantum computers do not threaten proof of work in practice.** Grover's algorithm gives at most a
square-root speed-up for hash search. It also parallelises badly: k quantum machines are only √k
times faster than one, while k ASICs are k times faster. The function would have to run as a
reversible quantum circuit, at gate speeds many orders of magnitude below ASICs. Memory-hard functions
such as scrypt are harder still to run that way. scrypt, SHA-256d, RandomX and yespower are all
"post-quantum" for mining purposes. Ferrite's real quantum exposure is signatures and MWEB's
commitments (§11.3), and a new PoW does not change either. Treat "post-quantum" as a property of the
signature and MWEB plan, not of the mining algorithm.

**CPU-oriented candidates:**

| Algorithm | Used by | Verification cost | Notes |
|---|---|---|---|
| RandomX | Monero (since 2019) and several RandomX variants | several milliseconds per hash in light mode (256 MiB cache), around 1 ms in fast mode (2 GiB dataset) | The most battle-tested CPU-oriented PoW. Verification is heavy for header sync, SPV/Electrum servers and pool share checks. XMRig mines most variants after a small patch, so Monero-scale hash power can be pointed at a Ferrite variant. Attractive to botnets. |
| yespower (Openwall, scrypt's successor) | several small coins, each with its own personalisation string | on the order of a millisecond per hash, depending on the chosen N and r (e.g. N = 2048, r = 32 uses 8 MiB) | Closest to the current code: one function behind `GetPoWHash()`. Tunable memory cost. Smaller ecosystem; GPU implementations exist but CPUs stay competitive. |
| Argon2d / VerusHash 2.x | a few coins | low to moderate | Less scrutiny as PoW; VerusHash also runs well on GPUs/FPGAs. |

**The security trade-off for a small chain.** Today Ferrite shares scrypt with Litecoin and
Dogecoin, whose ASIC hash power is many orders of magnitude larger and partly rentable. That is
Ferrite's main 51 % risk. A CPU algorithm removes that particular pool of hash power but exposes
Ferrite to cloud CPUs, botnets and, for RandomX, Monero's miners. A Ferrite-specific parameter set
helps only until mining software adapts. The opposite approach deserves a comparison:
**merged mining (AuxPoW) with Litecoin/Dogecoin**, as Dogecoin did in 2014, which borrows the large
scrypt networks' security instead of avoiding it. Most scrypt pools already merge-mine. Decide by
estimated attack cost (rentable hash rate × price × hours needed) for each option, not by the label.

**What a PoW change involves (hard fork, v4.0.0):**
- Bitcoin-derived code checks PoW in `CheckBlockHeader()`, where the height is unknown. A switch by
  height needs the check moved where the height is known (or a version bit tied to height by a
  contextual check). It touches `CBlockHeader::GetPoWHash()`, header sync (`AcceptBlockHeader`),
  the miner, `getblocktemplate` users, and the test framework (`ferrite_scrypt.py`).
- Reset the difficulty at the fork height. The new algorithm's hash rate has no relation to
  scrypt's. With DGW or ASERT (§11.2), start from `powLimit` or a calibrated target.
- Every pool must deploy the new hash function. Given that some pools are not MWEB-ready yet, this is
  the costliest item on the roadmap for the ecosystem. Survey the pools before committing.
- Keep historical blocks on scrypt; checkpoints and `assumevalid` do not skip header PoW checks, so
  IBD cost is the sum of both algorithms' verification.

### 11.6 Ordering

1. This release (v3.3.0, §11.7): fixes in this branch, the §5 port, §4 re-validation,
   checkpoints (§8), the §5.2 soft fork, signalled from 1,250,000. No hard fork.
2. Soft forks that need no pool changes, in a later minor release: bury Taproot if active (§8),
   `COINBASE_MATURITY` (§11.4), the post-quantum output type once the scheme is chosen (§11.3).
3. v4.0.0 hard fork with one activation height, announced months ahead and run on testnet first:
   tail emission (§11.1), the DAA (§11.2) and, only if the attack-cost comparison in §11.5 favours
   it, the PoW change.

### 11.7 Version number for this release

**Decision (maintainer, 2026-10-15): v3.3.0** (`33352d3`). It describes this release better than
v3.2.1. It brings new consensus checks that apply to all blocks (§4), a soft fork signalled from
1,250,000 (§5.2), a change to the undo-data format that makes downgrades need a reindex (§5.3), new
peer-discouragement rules, and new mempool policy. A patch-level number would suggest a drop-in bug
fix, while pools and exchanges need to plan for this upgrade. The P2P protocol version (70017) is
unchanged. The version is set in `configure.ac` and in `build_msvc/bitcoin_config.h` (which was
still at 3.2.0); the user agent is now `/FerriteCore:3.3.0/`.

---

## 12. Maintainer runbook

### 12.1 Mainnet node queries (checkpoints, chain statistics, activation height)

Read-only. Run them on the synced node and paste the whole output back. `ferrite-cli` talks to a
running `ferrited`, or to `ferrite-qt` started with `server=1` in `ferrite.conf`. In `ferrite-qt`
the same commands also work one at a time in Window → Console, for example
`getblockheader(getblockhash(450000), false)`; the verbose form (without `false`) works as well,
since the header can be rebuilt from its fields. On Windows, `ferrited` has no daemon mode: the
message "-daemon is not supported on this operating system" comes from `ferrited` started with
`-daemon` or with `daemon=1` in `ferrite.conf`. Start it without that option (it then runs in its
own window), or use `ferrite-qt`.

These queries were run on 2026-10-15; the results are in §4, §5.2 and §8.

Windows (Command Prompt, in the folder that contains `ferrite-cli.exe`). The `%h`/`%b` forms are for
typing at the prompt; in a `.bat` file they must be doubled (`%%h`):

```bat
ferrite-cli getblockchaininfo
for %h in (450000 602214 750000 903321 1000000) do @for /f %b in ('ferrite-cli getblockhash %h') do @echo %h %b & @ferrite-cli getblockheader %b false
for /f %t in ('ferrite-cli getblockcount') do @set /a H=%t-10080 >nul
for /f %b in ('ferrite-cli getblockhash %H%') do @echo %H% %b & @ferrite-cli getblockheader %b & @ferrite-cli getchaintxstats 4096 %b
ferrite-cli getchaintxstats 10080
ferrite-cli getnetworkinfo
ferrite-cli getpeerinfo | findstr /c:"subver"
```

Linux/macOS:

```bash
ferrite-cli getblockchaininfo
for h in 450000 602214 750000 903321 1000000; do b=$(ferrite-cli getblockhash $h); echo "$h $b"; ferrite-cli getblockheader $b false; done
H=$(( $(ferrite-cli getblockcount) - 10080 )); b=$(ferrite-cli getblockhash $H); echo "$H $b"; ferrite-cli getblockheader $b; ferrite-cli getchaintxstats 4096 $b
ferrite-cli getchaintxstats 10080
ferrite-cli getnetworkinfo
ferrite-cli getpeerinfo | grep subver | sort | uniq -c
```

What each line is for: Taproot/MWEB status and the tip; hash and raw header of every checkpoint
height (checked as in §8); `nMinimumChainWork`/`defaultAssumeValid`/`chainTxData` from a block one
week of blocks below the tip; the recent block rate, used to turn "six weeks" into an activation
height (§5.2); and the client versions of connected peers, a rough view of how far v3.2.0 has spread.

### 12.2 MWEB history check (read-only, §4 step 1)

Python 3.8 or newer, no extra packages, on the machine that runs the node. From a checkout of this
branch:

```bat
python contrib\devtools\mweb_history_check.py
```

```bash
python3 contrib/devtools/mweb_history_check.py
```

It reads the RPC cookie from the default data directory (`%APPDATA%\Ferrite`, `~/.ferrite`). Use
`--datadir <dir>` for another data directory, `--rpcuser`/`--rpcpassword` if `ferrite.conf` sets
them, and `--rpcurl http://127.0.0.1:<port>/` if `rpcport` is not 9573. It prints progress, then a
summary ending in `RESULT: no M1/M2 violations` or a list of problems; paste the summary.

### 12.3 The branch on GitHub

The branch is pushed (`ferrite-backup-3.2.1-draft`) and open as
[pull request #65](https://github.com/koh-gt/ferrite-core/pull/65) against `ferrite-main`, with CI
running on it. It supersedes pull request #64 (`fec-v3.2.1`, the draft head `9406190`, which does
not build). The bundle files used before the push are no longer needed.

On 2026-10-22 the branch was force-pushed with edited commit messages. The file trees, authors and
dates are unchanged, but every commit after `9406190` has a new hash. This document and Appendix B
use the new hashes. Refresh an existing clone with
`git fetch origin && git reset --hard origin/ferrite-backup-3.2.1-draft`.

### 12.4 Merging into `ferrite-main` and tagging v3.3.0 (only after the §9 gates)

```bat
git fetch origin
git checkout ferrite-main
git merge --ff-only origin/ferrite-main
git merge --no-ff origin/ferrite-backup-3.2.1-draft -m "Merge branch 'ferrite-backup-3.2.1-draft' into ferrite-main"
git push origin ferrite-main
git tag -a v3.3.0 -m "Ferrite Core v3.3.0"
git push origin v3.3.0
```

Build the release binaries from the tag, then add their SHA-256 hashes to the release notes.

---

## Appendix B: commits added by this review

On top of the reviewed draft head `9406190`, oldest first, not counting the
commit that adds this appendix. Upstream commits were cherry-picked with `-x` and keep their
original author.

| # | Ferrite commit | Subject | Upstream Litecoin source |
|---|---|---|---|
| 1 | `b7e9b8a` | Fix build: restore Block.h brace, drop duplicated MWEB ConnectBlock block | — (this review) |
| 2 | `63fc6a2` | validation: remove stray InvalidBlockFound() in ConnectTip | — (this review) |
| 3 | `0561973` | libmw: restore BlockBuilder guards reverted by dd6b27e | — (this review) |
| 4 | `335abcd` | chainparams: freeze no MWEB outputs and grandfather no block | — (this review) |
| 5 | `8526775` | chainparams: fix regtest genesis proof of work | — (this review) |
| 6 | `6d9432a` | test: build Test_CoinsView and fix test_ferrite link error | — (this review) |
| 7 | `d1cc215` | test: repair the functional test framework | — (this review) |
| 8 | `d36f726` | contrib: rebrand the Linux desktop entry for Ferrite | — (this review) |
| 9 | `04f7419` | pow: keep regtest difficulty constant under KGW/DGW | — (this review) |
| 10 | `7274432` | test: use Ferrite regtest amounts in mweb_basic.py | — (this review) |
| 11 | `3dd308b` | doc: add review of the v3.2.1 draft branch | — (this review) |
| 12 | `0a1298a` | doc: add ferrite-main functional baseline to the review | — (this review) |
| 13 | `73f8127` | doc: note that the Qt GUI builds | — (this review) |
| 14 | `05d175c` | policy: Enforce Ke and Ko pubkey validity | `cfdfcf57a` |
| 15 | `f8526e9` | policy: Limit MWEB weight for mempool txs | `109ed13b7` |
| 16 | `fb07cc2` | net: Rate-limit MWEB leafset requests | `cb65fc5b6` |
| 17 | `77da2b8` | test: Fixing stale test values in mweb_weight.py | `9b5bb289b` |
| 18 | `f496db2` | net: Update MWEB leafset rate limits to be node-wide | `f24dec1c8` |
| 19 | `a7e44c7` | validation: preserve descendants of mutated MWEB blocks | `4dd6aeeb9` |
| 20 | `afc679a` | net: discourage mutated MWEB compact blocks | `136695e05` |
| 21 | `99a50a0` | policy: Reject empty MWEB pegout kernels from standard policy | `67580d6e3` |
| 22 | `423e4f2` | mining: enforce MWEB input limit | `dc7233420` |
| 23 | `a2ec03b` | validation: reject empty MWEB pegout features | `b250b016a` |
| 24 | `43485fd` | validate: reject empty MWEB kernel extradata | `a399fc4ca` |
| 25 | `69a523b` | mempool: serialize fPegout in txundo | `22a916236` |
| 26 | `e1b7f54` | validation: Fix script check data lifetime | `32413709d` |
| 27 | `3145150` | Don't return MWEB-only transactions when rpcserialversion <= 1 | `d3b9ac396` |
| 28 | `077847f` | mweb: prevent disk-reparse validation bypass | `b4313c34d` |
| 29 | `368d63c` | secp256k1-zkp: release scratch frames on invalid proofs | `d7ab03d31` |
| 30 | `3abdd69` | mweb: harden same-hash block mutation handling | `70ea696f3` |
| 31 | `2a200a7` | mweb: validate staged block aggregates incrementally | `f6afd2682` |
| 32 | `67b13e5` | mempool: evict MWEB conflicts when blocks connect | `36f06455f` |
| 33 | `ec5c012` | net: avoid MWEB recentRejects censorship | `c07d6228c` |
| 34 | `713509d` | validation: defer MWEB fast relay until after connect | `0cb141964` |
| 35 | `81746dc` | libmw: close POSIX copy file descriptors | `b8ccaf355` |
| 36 | `130ec36` | test: use fec_util in the MWEB tests ported from v0.21.5.6-v0.21.5.8 | — (this review) |
| 37 | `81acda4` | Cosmetic clean-up after the v0.21.5.x ports | — (this review) |
| 38 | `177aa69` | Reverting unnecessary functional test changes | `beae01d62` |
| 39 | `60c67b0` | test: check Ferrite's getblocktemplate rule policy in mweb_mining.py | — (this review) |
| 40 | `1705491` | Cosmetic: restore upstream whitespace in files touched by the ports | — (this review) |
| 41 | `bc56740` | test: pin Ferrite's MWEB feature activation heights in Test_Kernel | — (this review) |
| 42 | `40dd9f7` | build: fix the bug-report URL in AC_INIT | — (this review) |
| 43 | `7ce9110` | test: use a Ferrite-encoded key in MalformedMWEBOutputKeys | — (this review) |
| 44 | `28d9c71` | contrib: add a read-only MWEB history checker | — (this review) |
| 45 | `93a1fac` | doc: update the v3.2.1 review for the v0.21.5.6-v0.21.5.8 port | — (this review) |
| 46 | `eef98f2` | chainparams: add checkpoints up to 1,000,000 and refresh chain statistics | — (this review) |
| 47 | `e802a39` | doc: record the mainnet re-validation and checkpoint verification | — (this review) |
| 48 | `33352d3` | build: bump version to 3.3.0 | — (this review) |
| 49 | `b26fd97` | chainparams: activate the MWEB kernel-feature rules at height 1,150,000 | — (this review) |
| 50 | `bd7985b` | doc: add draft release notes for v3.3.0 | — (this review) |
| 51 | `3e7ee8a` | doc: rename the review for v3.3.0 and record the release decisions | — (this review) |
| 52 | `40dc178` | build: restore executable bits on scripts | — (this review) |
| 53 | `ef4e6df` | ci: add a GitHub Actions workflow | — (this review) |
| 54 | `29db57e` | doc: write the v3.3.0 release notes in the v3.2.0 release style | — (this review) |
| 55 | `2c3db24` | doc: describe the CI workflow and pull request #65 in the review | — (this review) |
| 56 | `cba001a` | consensus: activate the MWEB kernel rules by signalling from block 1,250,000 | — (this review) |
| 57 | `5693211` | doc: describe the signalled MWEB kernel soft fork in the review and release notes | — (this review) |

## Appendix A: every draft commit and its upstream source

Percentages show how much of the commit's added content matches the upstream commit (100 % when
omitted). "Ferrite-specific" means no upstream counterpart; each was read individually.

| # | Ferrite commit | Date | File(s) | Upstream Litecoin source | Verdict |
|---|---|---|---|---|---|
| 1 | `4887eb67c` Update chainparams.cpp  - add checkpoint 20250216 | 2025-02-16 | `src/chainparams.cpp` | — (Ferrite-specific) | Ferrite: checkpoint 292,000 + chain stats (OK) |
| 2 | `464bff774` Update Makefile.test.include | 2025-02-18 | `src/Makefile.test.include` | 5d4a2e5b9 CVE-2024-35202: Fix assertions in PartiallyDownloadedBlock | OK — faithful port |
| 3 | `0c0290f79` Update blockencodings.cpp | 2025-02-18 | `src/blockencodings.cpp` | 5d4a2e5b9 CVE-2024-35202: Fix assertions in PartiallyDownloadedBlock | OK — faithful port |
| 4 | `60898483d` Update blockencodings.h | 2025-02-18 | `src/blockencodings.h` | 5d4a2e5b9 CVE-2024-35202: Fix assertions in PartiallyDownloadedBlock | OK — faithful port |
| 5 | `3ba7b0c21` Create partially_downloaded_block.cpp | 2025-02-18 | `src/test/fuzz/partially_downloaded_block.cpp` | 5d4a2e5b9 CVE-2024-35202: Fix assertions in PartiallyDownloadedBlock (fuzz target; LF endings per dcc7bc505) | OK — faithful port |
| 6 | `fb0d18aee` Update mining.cpp | 2025-02-18 | `src/rpc/mining.cpp` | 455aff8c1 rpc: allow getblocktemplate for test chains when unconnected (80%) | OK — faithful port |
| 7 | `8870c0bff` Update build-unix.md - fix depends broken permissions | 2025-02-18 | `doc/build-unix.md` | — (Ferrite-specific) | Docs (OK) |
| 8 | `03bccc764` Update configure.ac | 2025-02-18 | `configure.ac` | — (Ferrite-specific) | Ferrite: version 3.2.1 (OK) |
| 9 | `4f9994a60` Update build-unix.md -lfmt | 2025-02-19 | `doc/build-unix.md` | — (Ferrite-specific) | Docs — `-lfmt` (see B5) |
| 10 | `82adf57e8` Update boost.mk | 2025-02-20 | `depends/packages/boost.mk` | — (Ferrite-specific) | Ferrite-specific: header-only Boost 1.81 — **breaks depends builds (B2)** |
| 11 | `4731370f8` Update gperf_header_regen.patch | 2025-02-20 | `depends/patches/fontconfig/gperf_header_regen.patch` | — (Ferrite-specific) | Ferrite-specific: patch context looks like newer fontconfig — **verify (B3)** |
| 12 | `9d9a95560` Update fontconfig.mk | 2025-02-20 | `depends/packages/fontconfig.mk` | — (Ferrite-specific) | Ferrite-specific (goes with B3) |
| 13 | `391705d16` Delete depends/patches/fontconfig/remove_char_width_usage.pa | 2025-02-20 | `depends/patches/fontconfig/remove_char_width_usage.patch` | (deletions only) | Ferrite-specific (goes with B3) |
| 14 | `bfece0222` Delete depends/patches/boost directory | 2025-02-20 | `depends/patches/boost/unused_var_in_process.patch` | (deletions only) | Ferrite-specific (goes with B2) |
| 15 | `b805e2596` Update net_processing.cpp - p2p: Avoid prematurely clearing  | 2026-06-23 | `src/net_processing.cpp` | dab3bb7e1 p2p: Avoid prematurely clearing download state for other pee | OK — faithful port |
| 16 | `96dfc7c90` Update p2p_blockfilters.py - functional test fixes | 2026-06-23 | `test/functional/p2p_blockfilters.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 17 | `870300276` Update p2p_node_network_limited.py | 2026-06-23 | `test/functional/p2p_node_network_limited.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 18 | `462fbd1fd` Update rpc_blockchain.py | 2026-06-23 | `test/functional/rpc_blockchain.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 19 | `12dfcfb28` Update rpc_misc.py | 2026-06-23 | `test/functional/rpc_misc.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 20 | `93084149e` Update messages.py | 2026-06-23 | `test/functional/test_framework/messages.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 21 | `367c0de06` Update wallet_import_rescan.py | 2026-06-23 | `test/functional/wallet_import_rescan.py` | 7d9fea0cc functional test fixes | OK — faithful port |
| 22 | `03676c056` Update miniupnpc.mk build: miniupnpc 2.2.2 | 2026-06-23 | `depends/packages/miniupnpc.mk` | 16ba8b8ad build: miniupnpc 2.2.2 | OK — faithful port |
| 23 | `ebb78eeb5` Create dont_leak_info.patch | 2026-06-23 | `depends/patches/miniupnpc/dont_leak_info.patch` | 16ba8b8ad build: miniupnpc 2.2.2 | OK — faithful port |
| 24 | `7003e139a` Update dependencies.md | 2026-06-23 | `doc/dependencies.md` | 16ba8b8ad build: miniupnpc 2.2.2 | OK — faithful port |
| 25 | `401830c77` Default -peerblockfilters and -blockfilterindex to off when  | 2026-06-23 | `src/init.cpp` | 0d04e754a Default -peerblockfilters and -blockfilterindex to off when  | OK — faithful port |
| 26 | `ff8620ccc` Update rpcdump.cpp wallet, rpc: add MWEB view keys to dumpwa | 2026-06-23 | `src/wallet/rpcdump.cpp` | eae9e47f9 wallet, rpc: add MWEB view keys to dumpwallet | OK — faithful port |
| 27 | `ef2cebc64` Update scriptpubkeyman.h wallet, rpc: add MWEB view keys to  | 2026-06-23 | `src/wallet/scriptpubkeyman.h` | eae9e47f9 wallet, rpc: add MWEB view keys to dumpwallet | OK — faithful port |
| 28 | `20b012d91` MINER: Don't include MWEB transactions if the input and outp | 2026-06-23 | `src/libmw/src/node/BlockBuilder.cpp` | f423a840a MINER: Don't include MWEB transactions if the input and outp | OK — faithful port |
| 29 | `d19855861` Update load.cpp Fix wallet with boost library >= 1.78 | 2026-06-23 | `src/wallet/load.cpp` | 3c3aedb97 Fix wallet with boost library >= 1.78 | OK — faithful port |
| 30 | `e336e866e` Update base.cpp - Fixed an issue where transaction indexes o | 2026-06-23 | `src/index/base.cpp` | eb7f68a9e Fixed an issue where transaction indexes of block could be l | OK — faithful port |
| 31 | `87b36ebc9` Update load.cpp - confirmed - build: wallet: Fix wallet with | 2026-06-23 | `src/wallet/load.cpp` | (deletions only) | Fix-up of previous paste (OK) |
| 32 | `840098183` Update chainparamsbase.h - cstdint fix build error | 2026-06-23 | `src/chainparamsbase.h` | 58f89ba03 Add <cstdint> include. | OK — faithful port |
| 33 | `c5d8d875c` Update zmqabstractnotifier.h - cstdint fix build error | 2026-06-23 | `src/zmq/zmqabstractnotifier.h` | 58f89ba03 Add <cstdint> include. | OK — faithful port |
| 34 | `bd00f5399` debugBuildFix Update Logger.h | 2026-06-23 | `src/libmw/include/mw/common/Logger.h` | 6fc0530c7 fix: debug build conflict with logger symbol (83%) | OK — faithful port |
| 35 | `4af45d4fb` debugBuildFix Update Logger.cpp | 2026-06-23 | `src/libmw/src/common/Logger.cpp` | 6fc0530c7 fix: debug build conflict with logger symbol | OK — faithful port |
| 36 | `83812888a` Belt-and-suspenders input commitment and pubkey checks - Upd | 2026-06-23 | `src/libmw/src/node/CoinsViewCache.cpp` | e7cbf1d56 Belt-and-suspenders input commitment and pubkey checks | OK — faithful port |
| 37 | `9beb87ab1` Fix data corruption issue on PMMR rewind - Update File.cpp | 2026-06-23 | `src/libmw/src/file/File.cpp` | 23e5eac3c Fix data corruption issue on PMMR rewind | OK — faithful port |
| 38 | `825e32a8a` Fix data corruption issue on PMMR rewind - Update validation | 2026-06-23 | `src/validation.cpp` | 23e5eac3c Fix data corruption issue on PMMR rewind | OK — faithful port |
| 39 | `2b249b14c` Improve file write durability for MMRs - Update AppendOnlyFi | 2026-06-23 | `src/libmw/include/mw/file/AppendOnlyFile.h` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 40 | `cf1f5e302` Improve file write durability for MMRs - Update File.h | 2026-06-23 | `src/libmw/include/mw/file/File.h` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 41 | `d194a0621` Improve file write durability for MMRs - Update File.cpp | 2026-06-23 | `src/libmw/src/file/File.cpp` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 42 | `a42556555` Improve file write durability for MMRs - Update LeafSet.cpp | 2026-06-23 | `src/libmw/src/mmr/LeafSet.cpp` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 43 | `59bc467d3` Update PruneList.cpp | 2026-06-23 | `src/libmw/src/mmr/PruneList.cpp` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 44 | `e628bddee` Improve file write durability for MMRs - Update txdb.cpp | 2026-06-23 | `src/txdb.cpp` | bf25a7c1c Improve file write durability for MMRs | OK — faithful port |
| 45 | `d571ce7db` Open file with update attribute in Commit - Update File.cpp | 2026-06-23 | `src/libmw/src/file/File.cpp` | 3110a7ea8 Open file with update attribute in Commit | OK — faithful port |
| 46 | `f417d2768` Update tx_verify.cpp | 2026-06-23 | `src/consensus/tx_verify.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 47 | `f3828470a` Create Amount.h | 2026-06-23 | `src/libmw/include/mw/consensus/Amount.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 48 | `cfc8ae2f0` Update KernelSumValidator.h | 2026-06-23 | `src/libmw/include/mw/consensus/KernelSumValidator.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 49 | `c51b2eeb1` Update ValidationException.h | 2026-06-23 | `src/libmw/include/mw/exceptions/ValidationException.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 50 | `001c907d4` Update Block.h | 2026-06-23 | `src/libmw/include/mw/models/block/Block.h` | 42e7071a4 Prevent kernel fee overflow (75%) | **D1 fixed** — dropped `}` in `GetSupplyChange()`; nothing compiled |
| 51 | `7f7eb9e76` Update Kernel.h | 2026-06-23 | `src/libmw/include/mw/models/tx/Kernel.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 52 | `3cba5a206` Update Transaction.h | 2026-06-23 | `src/libmw/include/mw/models/tx/Transaction.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 53 | `0dd6151ce` Update TxBody.h | 2026-06-23 | `src/libmw/include/mw/models/tx/TxBody.h` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 54 | `69e9d7ed8` Update TxBody.cpp | 2026-06-23 | `src/libmw/src/models/tx/TxBody.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 55 | `dd6b27e8e` Update BlockBuilder.cpp | 2026-06-23 | `src/libmw/src/node/BlockBuilder.cpp` | 42e7071a4 Prevent kernel fee overflow (88%) | **D4 fixed** — reverted `20b012d` (sum-to-zero guard, `return false`) |
| 56 | `2092b914e` Update Test_Block.cpp | 2026-06-23 | `src/libmw/test/tests/models/block/Test_Block.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 57 | `f2089783d` Update Test_Kernel.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_Kernel.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 58 | `1c47f2148` Update Test_Transaction.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_Transaction.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 59 | `ef494efeb` Update Test_TxBody.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_TxBody.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 60 | `ce396a9eb` Update Test_BlockValidator.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_BlockValidator.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 61 | `d6f657838` Update mweb_miner.cpp | 2026-06-23 | `src/mweb/mweb_miner.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 62 | `06cbcbee8` Update mweb_models.h | 2026-06-23 | `src/mweb/mweb_models.h` | 42e7071a4 Prevent kernel fee overflow (70%) | OK — faithful port |
| 63 | `d72da34a5` Update mweb_node.cpp | 2026-06-23 | `src/mweb/mweb_node.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 64 | `222d961c6` Update blockchain.cpp | 2026-06-23 | `src/rpc/blockchain.cpp` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 65 | `4c3b7893c` Update fec_util.py | 2026-06-23 | `test/functional/test_framework/fec_util.py` | 42e7071a4 Prevent kernel fee overflow | OK — faithful port |
| 66 | `eee659fbd` Update messages.py | 2026-06-23 | `test/functional/test_framework/messages.py` | 42e7071a4 Prevent kernel fee overflow (82%) | **D6 fixed** — `import litecoin_scrypt`, `MAX_MONEY` reset to 84M |
| 67 | `608b18b46` Update test_framework.py | 2026-06-23 | `test/functional/test_framework/test_framework.py` | 0c59e996e Functional test framework fix (36%) | OK — faithful port |
| 68 | `e23911cfe` Update test_framework.py | 2026-06-23 | `test/functional/test_framework/test_framework.py` | — (Ferrite-specific) | Ferrite: re-applies Ferrite names after upstream paste (OK) |
| 69 | `aa98bc12c` Create mweb_p2p_mutated_block_submitblock.py | 2026-06-23 | `test/functional/mweb_p2p_mutated_block_submitblock.py` | 7eb181bb6 test: Functional test demonstrating handling of mutated bloc | **D7 fixed** — imports `ltc_util` |
| 70 | `418b0b6de` Update mweb_node.cpp | 2026-06-23 | `src/mweb/mweb_node.cpp` | 742ee9454 Erase block data for mutated blocks | OK — faithful port |
| 71 | `5b1a8bee1` Update validation.cpp | 2026-06-23 | `src/validation.cpp` | 742ee9454 Erase block data for mutated blocks (83%) | **D3 fixed** — left old `InvalidBlockFound()` outside `if (state.IsInvalid())` |
| 72 | `a11f4845f` Update wallet.cpp | 2026-06-23 | `src/wallet/wallet.cpp` | 1cc1ceef8 wallet: Quick pegout accounting fix (83%) | OK — faithful port |
| 73 | `5030ce100` 2026 Update configure.ac | 2026-06-23 | `configure.ac` | 940c89ad7 v0.21.5.4 | OK — faithful port |
| 74 | `d25b3600e` Update Makefile.test.include | 2026-06-23 | `src/Makefile.test.include` | 940c89ad7 v0.21.5.4 | OK — faithful port |
| 75 | `cd584ff58` Add files via upload | 2026-06-23 | `doc/mweb/consensus.md`<br>`doc/mweb/kernels.md`<br>`doc/mweb/serialization.md`<br>`doc/mweb/weight.md` | b4d7e70af MWEB doc updates | OK — faithful port |
| 76 | `f125900a3` Update stealth-addresses.md | 2026-06-23 | `doc/mweb/stealth-addresses.md` | b4d7e70af MWEB doc updates | OK — faithful port |
| 77 | `7c9157783` contrib: Add litecoin-qt.desktop for Linux desktop integrati | 2026-06-23 | `contrib/debian/litecoin-qt.desktop` | 53fc301ad contrib: Add litecoin-qt.desktop for Linux desktop integrati | Upstream port, still Litecoin-branded; **rebranded (P2)** |
| 78 | `d9b2ea03a` Update Blinds.h | 2026-06-23 | `src/libmw/include/mw/crypto/Blinds.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 79 | `87280b6b3` Update SecretKey.h | 2026-06-23 | `src/libmw/include/mw/models/crypto/SecretKey.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases (60%) | OK — faithful port |
| 80 | `1b18656d6` Update Input.h | 2026-06-23 | `src/libmw/include/mw/models/tx/Input.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 81 | `b438894d2` Update Output.h | 2026-06-23 | `src/libmw/include/mw/models/tx/Output.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases (75%) | OK — faithful port |
| 82 | `2fea46c26` Update OutputMask.h | 2026-06-23 | `src/libmw/include/mw/models/tx/OutputMask.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 83 | `b1073744e` Update Input.cpp | 2026-06-23 | `src/libmw/src/models/tx/Input.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 84 | `f7f401a38` fix accidental copy all Update SecretKey.h | 2026-06-23 | `src/libmw/include/mw/models/crypto/SecretKey.h` | (deletions only) | Fix-up of previous paste (OK) |
| 85 | `d194581c1` Update Kernel.cpp | 2026-06-23 | `src/libmw/src/models/tx/Kernel.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 86 | `d50ac374a` Update Output.cpp | 2026-06-23 | `src/libmw/src/models/tx/Output.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 87 | `b0aef47d2` Update Keychain.cpp | 2026-06-23 | `src/libmw/src/wallet/Keychain.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 88 | `9876d7b1a` Update TestMWEB.h | 2026-06-23 | `src/libmw/test/framework/include/test_framework/TestMWEB.h` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 89 | `0d20fded0` Update Test_StealthSumValidator.cpp | 2026-06-23 | `src/libmw/test/tests/consensus/Test_StealthSumValidator.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 90 | `d53ddc2ad` Update Test_Keys.cpp | 2026-06-23 | `src/libmw/test/tests/crypto/Test_Keys.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 91 | `f2a04e0c0` Update Test_RangeProofs.cpp | 2026-06-23 | `src/libmw/test/tests/crypto/Test_RangeProofs.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 92 | `520590d9c` Update Test_Input.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_Input.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases | OK — faithful port |
| 93 | `ffa63db3f` Update Test_Kernel.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_Kernel.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases (86%) | OK — faithful port |
| 94 | `ec81ffa01` Update Test_Output.cpp | 2026-06-23 | `src/libmw/test/tests/models/tx/Test_Output.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases (89%) | OK — faithful port |
| 95 | `c39148cbb` Update Test_Keychain.cpp | 2026-06-23 | `src/libmw/test/tests/wallet/Test_Keychain.cpp` | 773c138f7 Adding fallbacks for Hash-to-SecretKey edge cases (89%) | OK — faithful port |
| 96 | `5c10100b1` Create ferrite_scrypt.py | 2026-06-23 | `test/functional/ferrite_scrypt.py` | d1392228f litecoin_scrypt python dependency replacement | OK — faithful port |
| 97 | `cee3a09fd` Fix cache lead bounds check Update PMMRCache.cpp | 2026-06-23 | `src/libmw/src/mmr/PMMRCache.cpp` | 6dd29525b Fix cache leaf bounds check | OK — faithful port |
| 98 | `03d56d55a` Update mweb_node.cpp | 2026-06-23 | `src/mweb/mweb_node.cpp` | 7564f05e6 Treat hogex fee and amount mismatches as BLOCK_CONSENSUS err | OK — faithful port |
| 99 | `323b489e6` Verify no HogEx flag is set pre-MWEB activation - Create mwe | 2026-06-23 | `test/functional/mweb_pre_activation_hogex_marker.py` | b9bd99a4e Verify no HogEx flag is set pre-MWEB activation | OK — faithful port |
| 100 | `bfdce2e68` Verify no HogEx flag is set pre-MWEB activation - Update mwe | 2026-06-23 | `src/mweb/mweb_node.cpp` | b9bd99a4e Verify no HogEx flag is set pre-MWEB activation | OK — faithful port |
| 101 | `8879d00c1` Update validation.cpp | 2026-06-23 | `src/validation.cpp` | bbd3b7841 Update MWEB chainstate in RollforwardBlock | OK (carried D3; fixed) |
| 102 | `a2d084a5a` Create mweb_dbcrash.py | 2026-06-23 | `test/functional/mweb_dbcrash.py` | bbd3b7841 Update MWEB chainstate in RollforwardBlock | **D7 fixed** — imports `ltc_util` |
| 103 | `7f7e3d475` Update test_runner.py | 2026-06-23 | `test/functional/test_runner.py` | bbd3b7841 Update MWEB chainstate in RollforwardBlock | OK — faithful port |
| 104 | `6b9845ab0` Update BlockValidator.cpp | 2026-06-23 | `src/libmw/src/node/BlockValidator.cpp` | 8f8ad64d0 Add additional guards against duplicate pegins | OK — faithful port |
| 105 | `09caf2983` Update Test_BlockValidator.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_BlockValidator.cpp` | 8f8ad64d0 Add additional guards against duplicate pegins | OK — faithful port |
| 106 | `69c968f9d` Create mweb_duplicate_plugin.py | 2026-06-23 | `test/functional/mweb_duplicate_plugin.py` | 8f8ad64d0 Add additional guards against duplicate pegins | **D7 fixed** — misnamed `…_plugin.py` (runner expects `…_pegin.py`), imports `ltc_util` |
| 107 | `98db43ad2` Update test_runner.py | 2026-06-23 | `test/functional/test_runner.py` | 8f8ad64d0 Add additional guards against duplicate pegins | OK — faithful port |
| 108 | `00d785053` mempool: MWEB assertions in CTxMempool::check - Update txmem | 2026-06-23 | `src/txmempool.cpp` | 3b569ae48 mempool: MWEB assertions in CTxMempool::check | OK — faithful port |
| 109 | `24e6c719c` Update miner.cpp | 2026-06-23 | `src/miner.cpp` | 873d9d2c8 miner: avoid disk lookup for previous HogEx and improve GBT  | OK — faithful port |
| 110 | `317ec48b4` Update mweb_miner.cpp | 2026-06-23 | `src/mweb/mweb_miner.cpp` | 873d9d2c8 miner: avoid disk lookup for previous HogEx and improve GBT  | OK — faithful port |
| 111 | `186a47a58` Update mweb_miner.h | 2026-06-23 | `src/mweb/mweb_miner.h` | 873d9d2c8 miner: avoid disk lookup for previous HogEx and improve GBT  (50%) | OK — faithful port |
| 112 | `5f0ab1e3f` Update mweb_mining.py | 2026-06-23 | `test/functional/mweb_mining.py` | 873d9d2c8 miner: avoid disk lookup for previous HogEx and improve GBT  | OK (imports `ltc_util`, pre-existing on main; fixed in D7) |
| 113 | `ebc284138` Update rawtransaction.cpp | 2026-06-23 | `src/rpc/rawtransaction.cpp` | 8782ab961 rpc: support maxfeerate=0 for MwEB transactions in sendrawtr | OK — faithful port |
| 114 | `1b7440e19` Update mweb_wallet_basic.py | 2026-06-23 | `test/functional/mweb_wallet_basic.py` | 8782ab961 rpc: support maxfeerate=0 for MwEB transactions in sendrawtr (91%) | **D7 fixed** — imports `ltc_util` (Ferrite import was overwritten) |
| 115 | `1e5e010ed` Update mweb_p2p.py | 2026-06-23 | `test/functional/mweb_p2p.py` | 2ba586ff1 p2p: More thorough functional tests for MWEB p2p messages | OK (imports `ltc_util`, pre-existing on main; fixed in D7) |
| 116 | `cb034999b` Update messages.py | 2026-06-23 | `test/functional/test_framework/messages.py` | 2ba586ff1 p2p: More thorough functional tests for MWEB p2p messages | OK (carried D6; fixed) |
| 117 | `7ab7e1ae2` mining: fixing mweb_mining test - Update mweb_mining.py | 2026-06-23 | `test/functional/mweb_mining.py` | a5dc3fc20 mining: fixing mweb_mining test | OK — faithful port |
| 118 | `14bcd813b` Update Params.h | 2026-06-23 | `src/libmw/include/mw/consensus/Params.h` | a54900438 consensus: Start enforcing kernel lock height | OK — faithful port |
| 119 | `03074aaff` Update ValidationException.h | 2026-06-23 | `src/libmw/include/mw/exceptions/ValidationException.h` | a54900438 consensus: Start enforcing kernel lock height | OK — faithful port |
| 120 | `5120188ab` Update Block.cpp | 2026-06-23 | `src/libmw/src/models/block/Block.cpp` | a54900438 consensus: Start enforcing kernel lock height | OK — faithful port |
| 121 | `27a4df432` Update TxBuilder.h | 2026-06-23 | `src/libmw/test/framework/include/test_framework/TxBuilder.h` | a54900438 consensus: Start enforcing kernel lock height (50%) | OK — faithful port |
| 122 | `5816053d6` Update TxBuilder.cpp | 2026-06-23 | `src/libmw/test/framework/src/TxBuilder.cpp` | a54900438 consensus: Start enforcing kernel lock height (67%) | OK — faithful port |
| 123 | `d548e119b` Update Test_Block.cpp | 2026-06-23 | `src/libmw/test/tests/models/block/Test_Block.cpp` | a54900438 consensus: Start enforcing kernel lock height | OK — faithful port |
| 124 | `44865c931` Update mweb_node.cpp | 2026-06-23 | `src/mweb/mweb_node.cpp` | 17f16cef4 Ensure mutated MWEB block doesn't get flagged as BLOCK_CONSE | OK — faithful port |
| 125 | `8652535ba` Update CoinsView.h | 2026-06-23 | `src/libmw/include/mw/node/CoinsView.h` | ff309cda9 Avoid mutating CoinsView state with invalid block | OK — faithful port |
| 126 | `236d1920e` Update CoinsViewCache.cpp | 2026-06-23 | `src/libmw/src/node/CoinsViewCache.cpp` | ff309cda9 Avoid mutating CoinsView state with invalid block | OK — faithful port |
| 127 | `06cfa899c` Update TxBuilder.h | 2026-06-23 | `src/libmw/test/framework/include/test_framework/TxBuilder.h` | ff309cda9 Avoid mutating CoinsView state with invalid block | OK — faithful port |
| 128 | `0576849fd` Update TxOutput.h | 2026-06-23 | `src/libmw/test/framework/include/test_framework/models/TxOutput.h` | ff309cda9 Avoid mutating CoinsView state with invalid block (96%) | OK — faithful port |
| 129 | `0d8f1bf1b` Update TxBuilder.cpp | 2026-06-23 | `src/libmw/test/framework/src/TxBuilder.cpp` | ff309cda9 Avoid mutating CoinsView state with invalid block (88%) | OK — faithful port |
| 130 | `d3b90ec1a` Update Tx.cpp | 2026-06-23 | `src/libmw/test/framework/src/models/Tx.cpp` | ff309cda9 Avoid mutating CoinsView state with invalid block | OK — faithful port |
| 131 | `7a2cbb14e` Update Test_CoinsView.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_CoinsView.cpp` | ff309cda9 Avoid mutating CoinsView state with invalid block | **D5 fixed** — test added but never listed in `Makefile.test.include` |
| 132 | `2f46cd5b2` Update feerate.cpp | 2026-06-23 | `src/policy/feerate.cpp` | 1cd94bbcb Harden amount calculation | OK — faithful port |
| 133 | `b3300945a` Update amount_tests.cpp | 2026-06-23 | `src/test/amount_tests.cpp` | 1cd94bbcb Harden amount calculation | OK — faithful port |
| 134 | `f6e10cf75` Update chainparams.cpp | 2026-06-23 | `src/chainparams.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output (60%) | Ferrite placeholder frozen ID; **made explicit-empty (P1)** |
| 135 | `34ccbe2ff` Update chainparams.h | 2026-06-23 | `src/chainparams.h` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 136 | `48a07ee90` Update params.h | 2026-06-23 | `src/consensus/params.h` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output (83%) | OK — faithful port |
| 137 | `9d95423a1` Update tx_verify.cpp | 2026-06-23 | `src/consensus/tx_verify.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 138 | `b41f704fb` Update CoinsView.h | 2026-06-23 | `src/libmw/include/mw/node/CoinsView.h` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 139 | `f5b25bc71` Update CoinsViewCache.cpp | 2026-06-23 | `src/libmw/src/node/CoinsViewCache.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 140 | `c4e267393` Update Test_BlockBuilder.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_BlockBuilder.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 141 | `9e8a44efb` Update Test_MineChain.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_MineChain.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output (67%) | OK — faithful port |
| 142 | `56fff0c93` Update Test_Reorg.cpp | 2026-06-23 | `src/libmw/test/tests/node/Test_Reorg.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output (75%) | OK — faithful port |
| 143 | `129c6e79a` Update mweb_node.cpp | 2026-06-23 | `src/mweb/mweb_node.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | **D2 fixed** — pasted the frozen-output block twice (redeclared `const bool`) |
| 144 | `fadecb0ef` Update wallet.cpp | 2026-06-23 | `src/wallet/wallet.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output | OK — faithful port |
| 145 | `c2608884b` Height enforcement - Update Params.h | 2026-06-23 | `src/libmw/include/mw/consensus/Params.h` | — (Ferrite-specific) | Ferrite-specific: lock-height grandfather = 903,321 — **manual review M2** |
| 146 | `e1ff9d99c` Update chainparams.cpp | 2026-06-24 | `src/chainparams.cpp` | 1dcbf3fce Consensus: Freeze MWEB-rebalancing output (50%) | Testnet frozen/grandfather placeholders; **made explicit (P1)** |
| 147 | `8ca654977` Update mweb_node.cpp | 2026-06-24 | `src/mweb/mweb_node.cpp` | 17f16cef4 Ensure mutated MWEB block doesn't get flagged as BLOCK_CONSE | Carried D2 forward; fixed with D2 |
| 148 | `d85ee6e91` Update mweb_p2p_mutated_block_submitblock.py | 2026-06-24 | `test/functional/mweb_p2p_mutated_block_submitblock.py` | 17f16cef4 Ensure mutated MWEB block doesn't get flagged as BLOCK_CONSE (97%) | OK — faithful port |
| 149 | `190b983e9` Update chainparams.cpp | 2026-06-24 | `src/chainparams.cpp` | — (Ferrite-specific) | Ferrite: `ParseHex` form of placeholder (superseded by P1) |
| 150 | `74b089c3c` Update mweb_p2p_mutated_block_submitblock.py | 2026-06-24 | `test/functional/mweb_p2p_mutated_block_submitblock.py` | 66f685654 Use ParseHex instead of uint256S | OK — faithful port |
| 151 | `b5567b285` Update net.cpp | 2026-06-24 | `src/net.cpp` | 457bcd78c net: Increase MAX_PROTOCOL_MESSAGE_LENGTH to 32MB | OK — faithful port |
| 152 | `263aa8c0c` Update net.h | 2026-06-24 | `src/net.h` | 457bcd78c net: Increase MAX_PROTOCOL_MESSAGE_LENGTH to 32MB | OK — faithful port |
| 153 | `3eae1edbb` Update p2p_invalid_messages.py | 2026-06-24 | `test/functional/p2p_invalid_messages.py` | 457bcd78c net: Increase MAX_PROTOCOL_MESSAGE_LENGTH to 32MB | OK — faithful port |
| 154 | `d1ebdef37` Update messages.py | 2026-06-24 | `test/functional/test_framework/messages.py` | 457bcd78c net: Increase MAX_PROTOCOL_MESSAGE_LENGTH to 32MB | OK (carried D6; fixed) |
| 155 | `fde82bf39` Update test_runner.py | 2026-06-24 | `test/functional/test_runner.py` | 457bcd78c net: Increase MAX_PROTOCOL_MESSAGE_LENGTH to 32MB | OK — faithful port |
| 156 | `a6d0e4966` Update policy.cpp | 2026-06-24 | `src/policy/policy.cpp` | c25bf8923 policy: Enforce IsStandard for pegout scripts | OK — faithful port |
| 157 | `7b0ec43c8` Safer MWEB logger initialization - Update init.cpp | 2026-06-26 | `src/init.cpp` | b4c003779 Safer MWEB logger initialization | OK — faithful port |
| 158 | `3cf578d9e` bdb4.8 20260627 Update build-unix.md | 2026-06-27 | `doc/build-unix.md` | — (Ferrite-specific) | Docs — see B4 |
| 159 | `e4a822863` Update build-windows.md | 2026-06-27 | `doc/build-windows.md` | — (Ferrite-specific) | Docs (OK) |
| 160 | `9406190c5` GCC and BDB namespace fix Update build-unix.md | 2026-06-27 | `doc/build-unix.md` | — (Ferrite-specific) | Docs — BDB fix incomplete (B4) |
