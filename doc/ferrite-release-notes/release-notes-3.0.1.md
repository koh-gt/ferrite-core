### This is a revision of Ferrite Core v3.0.0 based on v3.0.0r1 changes.

## Ferrite Core v3.0.0+ is a **hardfork** of v2.1.2
(Fork height $250,000$, Current height: $131,807$ 22 Apr 2023)

![](https://progress-bar.dev/118193/?scale=120000&title=&nbsp;Blocks&nbsp;Remaining&nbsp;&width=240&color=323040&suffix=&nbsp;&sol;&nbsp;120&#44;000&nbsp;blocks)

### Nodes are encouraged to upgrade to v3 when possible.

> Difficulty adjustment algorithm (DAA) changed to DarkGravityWell v3 activated on Block Height $250,000$ to avoid hashrate and difficulty oscillations for providing a stable and consistent block confirmation time. This is to avoid difficulty oscillations due to highly variable hashrate from abusing the default difficulty algorithm. More details on the new DAA hardfork upgrade can be found on the [updated whitepaper](https://github.com/koh-gt/ferrite-core/wiki/About-Ferrite-Core#difficulty-algorithm-hardfork).

> Given the average block time has slowed from 70-80s to 300-600s in the past 10,000 blocks, this would provide at least 400 days of time for the difficulty algorithm hard fork. As the block emission schedule is unchanged, all funds stored or mined before this height will be safely compatible with v3.0.0 even in the case of late upgrading.

Current and future versions of Ferrite Core will maintain wallet backwards compatibility with all versions of Ferrite Core.

## v3.0.1 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.1/ferrite-3.0.1-win64-setup.exe) | `da1accfa9800d7fb9af9c9c75d9bc3fbca66798064af969ac3a216ebf25ee04d` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.1/ferrite-qt.exe) |  `18c4e9a26be6528ffd2134a1cacca90574cc5216ea2a068c84d2b61544fb262f` |
| Windows             | 64     | Qt + Daemon         | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.1/ferrite-3.0.1.7z) | `f13b20323d24687d6c010f129dcc133a1e963c6bee461d361d15876915115c55` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.1/ferrite-qt.7z) | `734e05d877bbe6fabc2303b32a29a72c0261e98f28af7a1ecf400bc0a24a29b1` |
| Unix                     |  64     | Qt + Daemon       | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v3.0.1/ferrite-3.0.1-unix.7z)  | `c60c098188b11c4f64997b23d834382660fa433126f10cdd41e883d3e6b6065e` |
> **Mac wallets - In progress**


## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
### [MiningPoolStats](https://miningpoolstats.stream/ferrite)

## Changes
Changes in v3.0.1
More addnodes are added into seeds for faster block synchronisation and a more reliable network.
Testnet nodes have been added. 
Testnet 4 is launched with DGWv3, Taproot and MWEB support enabled. You can get testnet coins from our [Telegram](https://t.me/ferrite-core) or mine using `generatetoaddress 1 <wallet_address>`.

In order to switch to testnet mode, add this to the bottom of your configuration file `ferrite.conf`.
```
# testnet parameters
testnet=1   # Change this to 0 to switch to mainnet, 1 for testnet.
[test]
# Add your testnet settings below
#
```

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
Checkpoints for block 150,000. 200,000 and 250,000 (important).
New tab for wallet management (dumprivkey, importprivkey).

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core or join the [Telegram group](https://t.me/ferrite_core) to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 21 Apr 2023.



## What's Changed
* v3.0.1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/48
* V3.0.1 #2 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/49

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v3.0.0...v3.0.1