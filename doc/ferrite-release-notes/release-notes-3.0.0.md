## Ferrite Core v3.0.0 is a **hardfork** of v2.1.2
(Fork height $250,000$, Current height: $127,710$ 13 Apr 2023)

![](https://progress-bar.dev/120000/?scale=120000&title=&nbsp;Blocks&nbsp;Remaining&nbsp;&width=240&color=323040&suffix=&nbsp;&sol;&nbsp;120&#44;000&nbsp;blocks)

### All nodes are encouraged to upgrade to v3.0.0 when possible.

Difficulty adjustment algorithm (DAA) changed to DarkGravityWell v3 activated on Block Height $250,000$ to avoid hashrate and difficulty oscillations for providing a stable and consistent block confirmation time. This is to avoid difficulty oscillations due to highly variable hashrate from abusing the default difficulty algorithm. More details on the new DAA hardfork upgrade can be found on the [updated whitepaper](https://github.com/koh-gt/ferrite-core/wiki/About-Ferrite-Core#difficulty-algorithm-hardfork).

Current block height -> $127,710$ as of 20230413 **(Upgrade before 20240601)**

Given the average block time has slowed from 70-80s to 300-600s in the past 10,000 blocks, this would provide at least 400 days of time for the difficulty algorithm hard fork.

**No cold wallet holdings will be lost for late upgrading.** 
However, coins mined/transacted using v1 and v2 will not be recognised after block 250,000 by v3.
In this case, replace the old ferrite-qt with v3.0.0+ and your funds will be reflected. 

As the block emission schedule is unchanged, all funds stored or mined before this height will be safely compatible with v3.0.0 even in the case of late upgrading.

Current and future versions of Ferrite Core will maintain wallet backwards compatibility with all versions of Ferrite Core.

## v3.0.0 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.0/ferrite-3.0.0-win64-setup.exe) | `4d788dd0b15dc1b2ef53d4717f9abea320ae6d8c177cdc7d1f8b86dc8b700ce2` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.0/ferrite-qt.exe) |  `ca215e4d0392fb9d078791baec1daabe2b55c08c7daf436ac331b606c219428f` |
| Windows             | 64     | Qt + Daemon         | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.0/ferrite-3.0.0.7z) | `dbc25e74d053d00fb47062fa9ad10e2ff90925620f19a4849bcafd83aade794e` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.0/ferrite-qt.7z) | `656d293e621577c688226d90c66287a482bfd2ca2754e1f8185fe598c6c3dc83` |
| Unix                     |  64     | Qt + Daemon       | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.0/ferrite-3.0.0-unix.7z)  | `0522f554092211acd5e597b91f91f2d51cb764ab1f44eb1e24b90dead262162d` |

## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
### [MiningPoolStats](https://miningpoolstats.stream/ferrite)

## Changes
Difficulty adjustment algorithm -> DarkGravityWell v3 from [Dash (Darkcoin)](https://en.wikipedia.org/wiki/Dash_(cryptocurrency)) by [Evan Duffield](https://www.worldcryptoindex.com/creators/evan-duffield/)

## MWEB is optional
MWEB is optional for getblocktemplate as of v2.1.0. Mining pools that do not have MWEB compatibility can mine Ferrite, however no MWEB transactions will be validated and MWEB fees will not be collected.

> MWEB will be currently optional since most miners mining Ferrite are currently not fully compatible with MWEB, which is a relatively new Litecoin feature.
> Before, getblocktemplate on v2.0.0 must be called with the MWEB rule
> getblocktemplate '{"rules": ["mweb", "segwit"]}'
> Now, pools that are not compatible with MWEB can also validate blocks without MWEB transactions using
> getblocktemplate '{"rules": ["segwit"]}'
> For the near future, MWEB will be activated no earlier than 2024 to allow time for mining pools to gradually update for MWEB support.

### Future changes
Checkpoints for block 150,000. 200,000 and 250,000 (important).
New tab for wallet management (dumprivkey, importprivkey).

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core or join the [Telegram group](https://t.me/ferrite_core) to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 13 Apr 2023.

## What's Changed
* v3.0.0-sf by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/47

**Changelog (from previous release v2.1.2)**: https://github.com/koh-gt/ferrite-core/compare/v2.1.2...v3.0.0
**Full Changelog (from previous version v2.0.0)**: https://github.com/koh-gt/ferrite-core/compare/v2.1.2...v3.0.0
**Complete Changelog**: https://github.com/koh-gt/ferrite-core/compare/v1.0.0...v3.0.0