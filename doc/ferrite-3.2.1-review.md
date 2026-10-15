# Ferrite Core v3.2.1 draft: review, fixes and suggestions

| | |
|---|---|
| Branch reviewed | `ferrite-backup-3.2.1-draft` at `9406190` (160 commits ahead of `ferrite-main`, 17 behind) |
| Merge base with `ferrite-main` | `fa52e12` (2024-12-20, "add h250000 graphic") |
| Upstream reference | Litecoin Core `v0.21.3`, `v0.21.4`, `v0.21.5.4` … `v0.21.5.8` (github.com/litecoin-project/litecoin) |
| Review date | 2026-10-15 |

Items marked **MANUAL REVIEW** need a decision or data that this review could not produce on its own.
Nothing in this document changes consensus on mainnet except where it says so explicitly.

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
4. **Two retroactive consensus rules need a mainnet re-validation before release (MANUAL REVIEW, §4).**
   Upstream's new MWEB input-metadata check and the kernel lock-height check apply to historical
   blocks. If any past Ferrite block violates them, v3.2.1 nodes would stop at that block.
   A `-reindex-chainstate` of a mainnet node with this build settles it.
5. **Upstream has moved on.** Litecoin v0.21.5.6, v0.21.5.7 and v0.21.5.8 (Jul–Sep 2026, 32 commits)
   are not ported yet. Several are security fixes for the same MWEB incident (§5).
6. **Checkpoints (requested: 450,000 / 602,214 / 750,000 / 903,321 / 1,000,000) are not added yet.**
   The block hashes could not be obtained from inside the review environment, and a wrong checkpoint
   hash forks a node off the network, so no placeholder was written. §7 has the exact procedure and
   code template; it is a five-minute job on a synced node.
7. **Long-standing test breakage on `ferrite-main`** was found while running the tests. Since
   2023-02-06 the regtest genesis block has failed its own proof of work, so no regtest node could
   start. Regtest difficulty also exploded after height 26 under DGW. The unit-test binary has not
   linked since 2023-07-30, and the functional-test framework has not imported since 2024-03. All four
   are fixed (test infrastructure and regtest only). §6 has what the test suites now report.
8. **Merging into `ferrite-main`:** `git merge-tree` shows no conflicts. Recommended gates are in §8.
9. **Build tooling** that is still broken but out of scope here (missing executable bits, depends
   Boost recipe, BDB instructions) is listed in §9.
10. **Roadmap** analysis for a 1 FEC tail emission, difficulty-algorithm changes and post-quantum
    options is in §10.

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

### Cosmetic regressions left as-is (optional clean-up)

- Upstream pastes removed Ferrite copyright lines (`Copyright (c) 2023 The Ferrite Core Developers`)
  from `Test_Kernel.cpp`, `Test_Output.cpp`, `Test_Keychain.cpp`, `mweb_models.h` and
  `mweb_wallet_basic.py`, and renamed the include guard in `mweb_models.h` back to `LITECOIN_…`.
- `TxBuilder.cpp` (libmw test framework) went back to `ltc_address` naming.
- Stray tab indentation from web-editor pastes in `wallet.cpp`, `validation.cpp`, `chainparams.cpp`.
- Redundant `-peerblockfilters=0 -blockfilterindex=0` in four functional tests: the draft ported
  upstream `7d9fea0cc` but not its revert `beae01d62`. Harmless.
- `depends/patches/miniupnpc/dont_use_wingen.patch` is no longer referenced (upstream deleted it).

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
  download-state fix, MWEB incident fixes, …). The only "missing" items are by design:
  `litecoin_scrypt.py` (Ferrite uses `ferrite_scrypt.py`) and upstream's revert `beae01d62`.
- **Build and tests**, see §6.

Ferrite-specific commits in the range, all reviewed:

| Commit | Change | Assessment |
|---|---|---|
| `4887eb6` | Checkpoint 292,000; chain stats; min-chain-work / assumevalid at 292,000 | OK (now ~20 months old; refresh in §7) |
| `03bccc7`, `5030ce1` | Version 3.2.1, copyright 2026 | OK |
| `e23911c` | Re-applies Ferrite names in `test_framework.py` after the upstream paste | OK |
| `c260888` | `KERNEL_LOCK_HEIGHT_GRANDFATHER_HEIGHT = 903'321` (3rd halving) | **MANUAL REVIEW M2** |
| `f6e10cf`, `e1ff9d9`, `190b983` | Frozen-output / grandfather placeholders | Replaced by P1 |
| `82adf57`, `bfece02` | depends: Boost 1.81 headers only | **Breaks depends builds, B2** |
| `9d9a955`, `4731370`, `391705d` | depends: fontconfig patch changes | **Unverified, B3** |
| doc commits | build guides | See B4/B5 |

---

## 4. Consensus rules in this release that need a mainnet re-validation (MANUAL REVIEW)

The MWEB incident fixes add rules that are checked for **every block** during IBD, including
historical ones. Checkpoints and `assumevalid` do **not** skip them (`assumevalid` only skips script
checks). Upstream Litecoin handled the one known violating block by grandfathering its hash.
Ferrite exempts no block, which is correct only if Ferrite's own history contains no violation.

| ID | Rule | Upstream commit | Applies to Ferrite blocks | Risk if a past block violates it |
|---|---|---|---|---|
| M1 | MWEB input commitment/pubkey must equal the spent UTXO (`UTXO_MISMATCH`) | `e7cbf1d56`, grandfather `1dcbf3fce` | All MWEB blocks since activation (≈150,120) | v3.2.1 nodes stop at that block (chain split from v3.2.0 nodes) |
| M2 | Kernel lock height ≤ block height | `a54900438`, Ferrite height `c260888` | Heights > 903,321 (already in the past) | Same |
| M3 | No HogEx marker before MWEB activation; HogEx fee/amount mismatch = consensus error; duplicate peg-in guards; kernel fee overflow | `b9bd99a4e`, `7564f05e6`, `8f8ad64d0`, `42e7071a4` | All blocks | Same, though honest history should never trip these |

**Procedure (on a machine with a synced mainnet node; use a copy of the datadir):**

```bash
cp -a ~/.ferrite /data/ferrite-reval          # never experiment on the live datadir
ferrited -datadir=/data/ferrite-reval -reindex-chainstate -daemon
tail -f /data/ferrite-reval/debug.log | grep -E "ConnectBlock .* failed|bad-blk-mweb|mweb-connect-failed|UTXO_MISMATCH|LOCK_HEIGHT|frozen"
# when done:
ferrite-cli -datadir=/data/ferrite-reval getblockchaininfo   # blocks == headers == network tip
ferrite-cli -datadir=/data/ferrite-reval gettxoutsetinfo     # compare with a v3.2.0 node at the same height
```

(`-reindex` instead of `-reindex-chainstate` additionally re-reads every block file and is slower.)

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
requires `segwit`. Its error text says blocks mined "without MWEB support will neither confirm any MWEB
transactions nor receive any MWEB transaction fees". That is not what consensus does: once MWEB is
active, `MWEB::Node::ContextualCheckBlock` rejects any block without the extension block
(`mweb-missing`). Pool software that ignores the template's `mweb` field therefore produces
**invalid blocks**, not merely fee-less ones. Options:

- Restore the upstream check. Non-MWEB-aware pool software then fails loudly at `getblocktemplate`
  instead of submitting invalid blocks. Confirm first that every Ferrite pool sends `"mweb"` in
  `rules`.
- Or keep the relaxed check but correct the message.

Either way, `mweb_mining.py` (upstream test) asserts the strict behaviour and fails on Ferrite until
this is decided.

---

## 5. Upstream changes not yet ported: Litecoin v0.21.5.6 – v0.21.5.8

63 files, +3,036 / −243. Recommendation: port these **before** tagging v3.2.1 final. Many harden
exactly the code this release introduces. Priorities are this review's assessment.

| Priority | Upstream | Summary | Ferrite decision needed |
|---|---|---|---|
| P0 | `b4313c34d` mweb: prevent disk-reparse validation bypass | Re-runs `ContextualCheckBlock` on the MWEB body actually being applied (e.g. after reload from disk). | — |
| P0 | `70ea696f3`, `4dd6aeeb9` same-hash mutation handling, preserve descendants of mutated blocks | Completes the mutated-block work started in v0.21.5.4/.5 (D3 area). | — |
| P0 | `32413709d` validation: fix script check data lifetime | Use-after-scope race between `txsdata` and the script-check queue on early return. | — |
| P1 | `136695e05` discourage mutated MWEB compact blocks | DoS: repeated expensive verification via compact blocks. | — |
| P1 | `cb65fc5b6`, `f24dec1c8` rate-limit MWEB leafset requests (node-wide) | DoS. | — |
| P1 | `c07d6228c` avoid MWEB `recentRejects` censorship; `0cb141964` defer MWEB fast relay until after connect | Relay correctness/censorship resistance. | — |
| P1 | `36f06455f` evict MWEB conflicts when blocks connect | Mempool consistency. | — |
| P1 | `dc7233420` mining: enforce MWEB input limit | Prevents invalid templates (pairs with D4). | — |
| P1 | `b250b016a` reject empty MWEB pegout features; `a399fc4ca` reject empty kernel extradata | **Soft-fork rules gated by activation height.** | Ferrite must choose `mweb_pegout_feature_activation_height` and `mweb_extradata_feature_activation_height` (future heights, ~1–2 weeks of blocks after release). Litecoin used 3,154,440 / 3,172,640. |
| P2 | `cfdfcf57a` policy: enforce Ke/Ko pubkey validity; `109ed13b7` limit MWEB weight for mempool txs; `67580d6e3` reject empty pegout kernels | Standardness policy. | — |
| P2 | `22a916236` serialize `fPegout` in txundo | **Changes the undo-data encoding**: spent peg-out coins get the high bit set. Once ported, an older binary reading that undo data after a downgrade decodes a wrong height for those coins; mention in release notes. | — |
| P2 | `d3b9ac396` don't return MWEB-only txs when `rpcserialversion<=1`; `b8ccaf355` close POSIX copy fds | RPC correctness; fd leak. | — |
| P2 | `d7ab03d31` secp256k1-zkp: release scratch frames on invalid proofs | Error paths left scratch frames allocated. libmw creates and destroys a scratch space per verification call, so release builds just free it; mainly hygiene (and `VERIFY` builds). | — |
| — | `63ba41f50` ltcpool DNS seed | Litecoin-only. | Skip. |

When porting, please use `git cherry-pick -x <upstream-commit>` from a local clone rather than
pasting files in the GitHub web editor. Nearly every defect in §2 came from pasting whole files or
partial functions.

---

## 6. Test results

Environment: Ubuntu 24.04, GCC 13.3, Boost 1.83, system BerkeleyDB 5.3 (`--with-incompatible-bdb`),
`./configure --without-gui --enable-tests --disable-bench`, Python 3.12 with `blake3` installed
(needed by the MWEB functional tests).

**Build:** `make` exits 0. `ferrited`, `ferrite-cli`, `ferrite-tx`, `ferrite-wallet`, `test_ferrite`
link. The only warnings are GCC 13 `-Wdangling-reference` false positives in `rpc/mining.cpp` and Boost
filesystem deprecations, both in unchanged upstream code.

### 6.1 Unit tests (`make check`)

To separate regressions from old problems, `ferrite-main` was built the same way with only the
test-enabling fixes applied (D9, D10, D13, plus the functional-framework repairs) and run through the
same suites.

| | Draft (this branch) | `ferrite-main` baseline |
|---|---|---|
| Suites run | 122 (one more: `Test_CoinsView`) | 121 |
| Suites failing | 14 | 15 |
| libmw (MWEB) suites | 34 / 34 pass | 33 / 33 pass (no `Test_CoinsView`) |

- **No new failures.** Every suite that fails on the draft fails the same way on `ferrite-main`.
- **One suite fixed by the draft:** `wallet/test/init_tests` fails on `ferrite-main` (wallet directory
  with a trailing separator on Boost ≥ 1.78) and passes on the draft, which ported upstream
  `3c3aedb97`.
- All MWEB suites pass, including the newly wired `Test_CoinsView` (D5), `Test_Block` (kernel lock
  height), `Test_Kernel` (fee overflow), `Test_BlockValidator` (duplicate peg-ins) and
  `Test_BlockBuilder`. `validation_block_tests`, which covers `ConnectTip` and the D3 code path,
  passes after D13.

The remaining failures are all test vectors that were never adapted from Litecoin:

| Suite(s) | Cause |
|---|---|
| `bloom_tests`, `key_tests`, `key_io_tests`, `descriptor_tests`, `rpc_tests`, `util_tests`, `psbt_wallet_tests`, `scriptpubkeyman_tests` | Hard-coded Bitcoin/Litecoin WIF keys and addresses; Ferrite uses different base58 prefixes (pubkey 36, secret 163). |
| `validation_tests`, `wallet_tests` | Assume the 50-coin subsidy and 84M supply (Ferrite: 100 FEC, 60.2M). |
| `miner_tests` | Hard-coded `blockinfo[]` nonces mined for Litecoin's parameters (`high-hash`). |
| `pow_tests` | Litecoin retarget vectors; Ferrite uses DGW from 250,000. |
| `versionbits_tests` | Asserts `nTimeoutHeight % nMinerConfirmationWindow == 0`; Ferrite mainnet uses 99,999,999 with a 40-block window. |
| `interfaces_tests` | `TestChain100Setup` assumes `COINBASE_MATURITY = 100`; Ferrite's is 10, so the test reads block 20 of a 10-block chain and crashes (test code, not node code). |

### 6.2 Functional tests (MWEB and every test the draft touched)

| Test | Result | Notes |
|---|---|---|
| `mweb_dbcrash.py` | ✅ pass | upstream `bbd3b7841` (RollforwardBlock MWEB state) |
| `mweb_duplicate_pegin.py` | ✅ pass | upstream `8f8ad64d0` (was misnamed, D7) |
| `mweb_p2p.py` | ✅ pass | upstream `2ba586ff1` |
| `mweb_p2p_mutated_block_submitblock.py` | ✅ pass | upstream `7eb181bb6`/`17f16cef4`; exercises the D3 code path |
| `mweb_pre_activation_hogex_marker.py` | ✅ pass | upstream `b9bd99a4e` |
| `mweb_pegout_all.py`, `mweb_reorg.py`, `mweb_wallet_basic.py` | ✅ pass | |
| `mweb_basic.py` | ✅ pass | after replacing Litecoin's 12.5-coin coinbase expectation with Ferrite's 25 FEC (`06cb170`) |
| `mweb_mining.py` | ❌ first assertion | Expects GBT to require the `mweb` rule; Ferrite relaxed that on purpose (M4). With only that assertion removed, the rest passes: valid GBT block, many peg-ins, many peg-outs, stale MWEB spend across a reorg. |
| `mweb_weight.py` | ❌ | Stale upstream test: pegs in before block ~102, so the first MWEB block (432) has no peg-in. Upstream fixed the **test** in v0.21.5.6 (`9b5bb289b`); the second half also needs v0.21.5.6's mempool weight limit (`109ed13b7`). |
| `p2p_invalid_messages.py`, `p2p_blockfilters.py`, `p2p_node_network_limited.py`, `rpc_misc.py`, `wallet_import_rescan.py --legacy-wallet` | ✅ pass | |
| `rpc_blockchain.py` | ❌ | `gettxoutsetinfo` expects Litecoin's 8,725 coins; Ferrite regtest has exactly twice that (100 vs 50 subsidy). Test data, pre-existing. |
| `mweb_node_compatibility.py`, `mweb_wallet_address.py`, `mweb_wallet_upgrade.py` | skipped | need previous-release binaries |

**Baseline comparison.** On `ferrite-main` (with the same test-enabling fixes), `test_runner.py` aborts
before running any test: the framework's cache clean-up calls `os.remove()` on the `indexes`
directory. The draft fixes that through its port of upstream `0c59e996e`. Run directly, the MWEB tests
compare as follows:

| Test | `ferrite-main` | Draft |
|---|---|---|
| `mweb_pegout_all.py`, `mweb_reorg.py`, `mweb_wallet_basic.py` | ✅ pass | ✅ pass |
| `mweb_p2p.py` | ❌ does not parse (`IndentationError`, line 45) | ✅ pass |
| `mweb_basic.py` | ❌ Litecoin amounts | ✅ pass (after `06cb170`) |
| `mweb_weight.py` | ❌ same `bad-txns-vin-empty` (identical tx hash) | ❌ same |
| `mweb_mining.py` | ✅ pass (older test without the GBT-rule assertion) | ❌ first assertion only (M4) |

No test passes on `ferrite-main` and fails on the draft because of a code change.

### 6.3 Test debt worth a dedicated clean-up

The Ferrite test suites still carry Litecoin vectors: base58 prefixes (WIF/addresses), the 50-coin
subsidy and 84M supply, Litecoin regtest nonces in `miner_tests`, Litecoin retarget numbers in
`pow_tests`, and BIP9 timeout assumptions in `versionbits_tests`. This has gone unnoticed because
the unit-test binary did not link and the functional framework did not import. Now that both run,
a Ferrite pass over the vectors would let CI gate every merge (§8).

---

## 7. Checkpoints (requested): status and procedure

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

**Status: not added.** The review environment could not reach any Ferrite data source:
`explorer.ferritecoin.org` is denied by the environment's network policy, and the `ferritecoin.org`
domains did not resolve from the web tools. A placeholder hash would make every node reject the real
block at that height, so none was written.

**On any synced, trusted node (ideally two independent ones):**

```bash
for h in 450000 602214 750000 903321 1000000; do
  printf '                {%9s, uint256S("0x%s")},\n' "$h" "$(ferrite-cli getblockhash $h)"
done
ferrite-cli getblockchaininfo | grep -A6 '"taproot"'           # confirms Taproot status/activation height
TIP=$(ferrite-cli getblockcount); H=$((TIP - 1000))             # a block safely buried
ferrite-cli getblockheader "$(ferrite-cli getblockhash $H)" | grep -E '"(hash|height|chainwork)"'
ferrite-cli getchaintxstats 4096 "$(ferrite-cli getblockhash $H)"
```

Then in `CMainParams` (`src/chainparams.cpp`):

```cpp
                {   292000, uint256S("0xb42d405513ca11a37bda4420f99577861258a593627401c6143026799e63d1e3")},  // added 20250216
                {   450000, uint256S("0x<hash>")},  // Taproot signalling start
                {   602214, uint256S("0x<hash>")},  // 2nd halving (25 FEC)
                {   750000, uint256S("0x<hash>")},  // Taproot
                {   903321, uint256S("0x<hash>")},  // 3rd halving (12.5 FEC)
                {  1000000, uint256S("0x<hash>")},  // height 1,000,000
```

At the same time, refresh the values that are still at height 292,000 (Feb 2025):
`nMinimumChainWork` (use `chainwork` of block `H`), `defaultAssumeValid` (hash of `H`) and `chainTxData`
(`time`, `txcount`, `txrate` from `getchaintxstats`). Cross-check every hash on a second node or
explorer, then sync a fresh node from genesis with the new binary before release.

If Taproot is active, consider *burying* the deployment (a fixed activation height, as Bitcoin did
for older soft forks) in a later release. That simplifies code and removes signalling edge cases.
`getblockchaininfo` reports the activation height.

---

## 8. Merging into `ferrite-main`

- `git merge-tree --write-tree origin/ferrite-main ferrite-backup-3.2.1-draft` reports **no conflicts**.
  The 17 commits only on `ferrite-main` are README/translation updates plus "Safer MWEB logger
  initialization" (`6b1c373`), which is identical to the draft's `7b0ec43`.
- Suggested gates before merging:
  1. CI green: build, `make check`, MWEB functional tests (a starter workflow is below).
  2. §4 mainnet re-validation passes.
  3. Decision on §5 (port v0.21.5.6–.8 now, or ship 3.2.1 and follow with 3.2.2 quickly).
  4. Checkpoints and chain statistics refreshed (§7).
  5. A depends build verified for each release platform (B2/B3).
  6. Release notes: MWEB fixes, the undo-format note (§5), "no frozen outputs" (P1), and the fact that
     v3.2.1 enforces new MWEB rules (miners and pools should upgrade together).
- Use a merge commit (`--no-ff`) rather than a squash. The 160-commit history is already public on the
  backup branch, and Appendix A refers to those hashes.

Starter CI workflow (`.github/workflows/ci.yml`). The build step alone would have caught D1/D2 on the
first push. It only gates on what passes today; add `make check` and the remaining functional tests
once the legacy Litecoin test vectors are fixed (§6.3) and M4 is decided.

```yaml
name: ci
on: [push, pull_request]
jobs:
  linux:
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: sudo apt-get update && sudo apt-get install -y build-essential libtool autotools-dev automake pkg-config bsdmainutils python3 libevent-dev libboost-dev libboost-system-dev libboost-filesystem-dev libboost-test-dev libboost-thread-dev libsqlite3-dev libdb-dev libdb++-dev libminiupnpc-dev libnatpmp-dev libzmq3-dev libfmt-dev
      - run: chmod +x autogen.sh share/genbuild.sh  # see B1
      - run: ./autogen.sh && ./configure --with-incompatible-bdb --without-gui --enable-tests --disable-bench
      - run: make -j"$(nproc)"
      - run: pip install blake3
      - run: >
          python3 test/functional/test_runner.py -j"$(nproc)"
          mweb_basic.py mweb_dbcrash.py mweb_duplicate_pegin.py mweb_p2p.py mweb_p2p_mutated_block_submitblock.py
          mweb_pegout_all.py mweb_pre_activation_hogex_marker.py mweb_reorg.py mweb_wallet_basic.py
          p2p_invalid_messages.py p2p_blockfilters.py p2p_node_network_limited.py rpc_misc.py
```

---

## 9. Build and tooling issues (not fixed here)

- **B1. Executable bits are missing repo-wide.** `ferrite-main` and the draft have 0 files with mode
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

## 10. Roadmap analysis

### 10.1 Tail emission (1 FEC per block)

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

### 10.2 Difficulty adjustment

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

### 10.3 Post-quantum

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

### 10.4 Other observations

- `COINBASE_MATURITY` is 10 blocks (Litecoin/Bitcoin: 100), i.e. coinbase can be spent after about
  10 minutes. On a chain that rented hash rate can reorganise, a deeper maturity limits how far
  invalidated coinbase spends can cascade. Raising it is a soft fork (it only tightens a rule).
  If it changes, `TestChain100Setup` and other tests that assume 100 need updating; with 10 they
  currently crash (`interfaces_tests` dereferences block 20 of a 10-block chain).

### 10.5 Ordering

1. v3.2.1: fixes in this branch, §4 re-validation, §5 port, checkpoints (§7). No hard fork.
2. A data-driven DAA proposal (§10.2) and a tail-emission spec (§10.1) with a single activation height,
   announced well in advance, on testnet first.
3. Post-quantum output type as a soft fork once the scheme choice is settled (§10.3).

---

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
