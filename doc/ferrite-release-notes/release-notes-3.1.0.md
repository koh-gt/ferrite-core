### This is a revision of Ferrite Core v3.0.1 based on v3.1.0r1 changes.

## If you are using v1.x.x or v2.x.x, please update to v3+.
### Ferrite Core v3.0.0+ is a **hardfork** of v2.1.2
(Fork height $250,000$, Current height: $142,434$ 15 May 2023)
Hard fork will occur later next year.

### Nodes are encouraged to upgrade to v3+ when possible.

> Difficulty adjustment algorithm (DAA) changed to DarkGravityWell v3 activated on Block Height $250,000$ to avoid hashrate and difficulty oscillations for providing a stable and consistent block confirmation time. This is to avoid difficulty oscillations due to highly variable hashrate from abusing the default difficulty algorithm. More details on the new DAA hardfork upgrade can be found on the [updated whitepaper](https://github.com/koh-gt/ferrite-core/wiki/About-Ferrite-Core#difficulty-algorithm-hardfork).

> Given the average block time has slowed from 70-80s to 300-600s in the past 10,000 blocks, this would provide at least 400 days of time for the difficulty algorithm hard fork. As the block emission schedule is unchanged, all funds stored or mined before this height will be safely compatible with v3.0.0 even in the case of late upgrading.

### All current and future versions of Ferrite Core wallets will maintain backwards compatibility.
> At height 250,000 all Ferrite will be split into 1 Ferrite (FEC) and 1 Ferrite Classic (FECC). v1.x.x and v2.x.x will only be able to spend the FECC portion, while v3+ will only be able to spend the FEC portion. 
Do not be alarmed if coins sent through v1.x.x and v2.x.x (Ferrite Classic) do not appear in your v3+ (Ferrite) balance - your Ferrite balance is unchanged.
Coins sent to Ferrite Classic through Ferrite will not show up, but are not lost. Ferrite Classic users need to copy their wallet to a v3+ Ferrite Core client to be able to access the coins. 
One can attempt to run both Ferrite Classic and Ferrite after Ferrite Classic is launched. Ferrite Classic may encounter hashrate instability.

## v3.1.0 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-3.1.0-win64-setup.exe) | `528f7f23f52a484bafce9a0b14df930aa8b4660141fd6869867c1a0c910b1676` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-qt.exe) |  `6c6dcd1d08801aeba7f5acc00b6d7cc175b37ef650fc65e7f6c6e1b684eed8c1` |
| Windows             | 64     | Qt + Daemon         | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-3.1.0-win64.7z) | `130cde3f15bd89d52dd1368f500a0a2e2ed1dfa57b01a71777616f275aa42fce` |
| Linux                    | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-qt-linux.7z) | `de4e854ff9ecfde363cb2228588e50fd5c34565fb98f4bc4a0bc397c4b3a4311` |
| Linux                    |  64     | Qt + Daemon       | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-3.1.0-linux.7z)  | `5acbe0a21880b02946eb0f646beb130505c02eeae8fdeaed5436dfe99780da56` |
| MacOS                 | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-qt) | `b840ebcd14a7da5923a97312a2d853359f6ed41ce6131fca397edaa56142bd3b` |
| MacOS                 |  64     | Qt + Daemon       | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-3.1.0-macos.7z)  | `380d35876a8f51bebc996f9b84c946bd3a326670f5fa16d542c5d3a5e56699b6` |

### v1.3.30 wallets (32-bit)
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 32     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-1.3.30-win32-setup.exe) | `ebd11b7ad69435d492893351c051d90f9af96a46b3f8c5becb5abe589c977327` |
| Windows             | 32     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-qt-win32.exe) |  `86841129b5625b2680ee0e9937af3b788b788038ba6e2aae37e9c131aee3a413` |
| Windows             | 32     | Qt + Daemon         | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.1.0/ferrite-1.3.30-win32.7z) | `6bcdc77631e3083553c253c56b8de980786c3787ac63f578a9c2b1f975b0804e` |

## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
### [MiningPoolStats](https://miningpoolstats.stream/ferrite)

## Changes
Changes in v3.1.0
More addnodes are added into seeds for faster block synchronisation and a more reliable network.
MWEB is now deployed on mainnet and will be fully deployed when 97.5% of miners add the 'mweb' rule and include MWEB transactions. This consensus is determined by windows of 40 blocks, of which at least 39 blocks must be mined with the additional 'mweb' rule in order for MWEB to be fully activated.
MWEB is still usable even when the 'mweb' rule is not needed. This will however mean that some blocks may not include any MWEB transactions, leaving MWEB transactions in the mempool to wait for the next block that contains the 'mweb' rule for peg-in or peg-out.

When this consensus occurs, all blocks must require the additional 'mweb' rule and include MWEB transactions.
Till this happens, the 'mweb' rule is optional and MWEB transactions may need to wait in the mempool until a block is mined by a miner that includes MWEB transactions.

### Previous changes (in v3.0.0)
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
Taproot testing on Testnet 4
Checkpoints for block 150,000. 200,000 and 250,000 (important).
Small fixes if necessary.
MacOS compatibility issues when necessary.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to [rent an Scrypt ASIC](https://github.com/koh-gt/ferrite-core/wiki/Rent-an-ASIC-miner).

Everyone is invited to pull ferrite-core or join the [Telegram group](https://t.me/ferrite_core) to suggest feature updates/chain parameters.
[Setup guide](https://github.com/koh-gt/ferrite-core/wiki/Getting-Started)

* Windows and Linux builds compiled on Ubuntu Linux 22.10 by @koh-gt
* MacOS builds are compiled on MacOS Monterey 12.0.1 by @koh-gt and @artucuxi 
Uploaded on 15 May 2023.

## What's Changed
* Update MWEB by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/50
* Update nodes_main.txt by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/51


**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.0.0...v3.1.0