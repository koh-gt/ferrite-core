### This is a revision of Ferrite Core v3.1.0 based on v3.1.1r1 changes.
Final pre-MWEB revision. MWEB activation height 150,000.

## If you are using v1.x.x or v2.x.x, please update to v3+ due to [DAA change](https://github.com/koh-gt/ferrite-core/wiki/About-Ferrite-Core#difficulty-algorithm-hardfork).
### Ferrite Core v3.0.0+ is a **hardfork** of v2.1.2
(Fork height $250,000$, Current height: $142,901$ 2 Jul 2023)
Hard fork will occur later next year in 2024.

### Nodes are encouraged to upgrade to v3+ when possible.

## v3.1.1 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | New/clean install   | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.1/ferrite-3.1.1-win64-setup.exe) | `9425b0af58b07492b3b06ac94757f2578cfa952c4a5452e2fa117494d8ec7aa5` |
| Windows             | 64     | Wallet Update        | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.1/ferrite-qt.exe) |  `6a332f4205855cacf0b10a2cdf55d361ddf4442bd807b52d6a71b2c15a71b110` |
| Windows             | 64     | Server                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.11/ferrite-3.1.1-win64.7z) | `b2ba278213f319589f2f20ee36a1bc22a262795ab1b702f5dc10ad7a87f01139` |
| Linux                    | 64     | Wallet Update       | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.1/ferrite-qt-linux.7z) | `d5d598abf27ff89f0b2b04c276be4b5c5e6efaec39fc3eee8869b50855aabc2d` |
| Linux                    |  64     | Server                    | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.1/ferrite-3.1.1-linux.7z)  | `6cea9c772318f705491e02e1ae2615c5d3a032af7b40dddfb7815654b3f0817c` |

## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
### [MiningPoolStats](https://miningpoolstats.stream/ferrite)

## Changes
Changes in v3.1.1
Checkpoint added to height 149,000
Pruning is only automatically enabled for New installers for height > 500,000. 
Pruning/Txindex can be disabled in ferrite.conf file.
Minor changes and security updates from [Litecoin 0.21.2.2](https://github.com/litecoin-project/litecoin/releases/tag/v0.21.2.2)

### Previous changes (in v3+)
> Difficulty adjustment algorithm -> DarkGravityWell v3 from [Dash (Darkcoin)](https://en.wikipedia.org/wiki/Dash_(cryptocurrency)) by [Evan Duffield](https://www.worldcryptoindex.com/creators/evan-duffield/)
> 
> ## MWEB is optional
> MWEB is optional for getblocktemplate as of v2.1.0. Mining pools that do not have MWEB compatibility can mine Ferrite, however no MWEB transactions will be validated and MWEB fees will not be collected.
> 
> > MWEB will be currently optional since most miners mining Ferrite are currently not fully compatible with MWEB, which is a relatively new Litecoin feature.
> > Before, getblocktemplate on v2.0.0 must be called with the MWEB rule
> > getblocktemplate '{"rules": ["mweb", "segwit"]}'
> > Now, pools that are not compatible with MWEB can also validate blocks without MWEB transactions using
> > getblocktemplate '{"rules": ["segwit"]}'
> > For the near future, MWEB will be activated no earlier than 2024 to allow time for mining pools to gradually update for MWEB support.

### Future changes
Checkpoint just after MWEB activation, new nodes.
MWEB fixes
Checkpoints for block 200,000 and 250,000 (important).
Small fixes if necessary.
MacOS compatibility issues when necessary.
Bitcoin 24.0 updates

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).

Everyone is invited to pull ferrite-core or join the [Telegram group](https://t.me/ferrite_core) to suggest feature updates/chain parameters.
[Setup guide](https://github.com/koh-gt/ferrite-core/wiki/Getting-Started)

* Windows and Linux builds compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 1 Jul 2023.

## Other notes

### All versions of Ferrite Core **wallets** will maintain backwards compatibility
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.

## What's Changed
* v3.1.0 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/50
* Ferrite v3.1.1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/52

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.1.0...v3.1.1