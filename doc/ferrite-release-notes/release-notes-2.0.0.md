## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt.7z)_

---

This is a major update of Ferrite Core v1.3.0 based on new Bitcoin Core 23.0 and Litecoin 0.21.2 source code changes. This brings new UI features such as easily creating multiple wallets, privacy mode, as well Taproot and MWEB support.
MWEB will be currently disabled since most miners mining Ferrite are currently not fully compatible with MWEB, which is a relatively new feature. 

Miners can signal for Taproot support starting block 450,000. (~2024).
When 97.5% consensus of blocks (39 in a row out of 40) are mined with Taproot support after height 450,000, Taproot policy will be enabled on the network.

Since the current update is not supported on 32-bit builds, v1.3.1 is released for 32-bit systems as an incremental update to v2.0.0 that is partially compatible with v2.0.0.

### New v2.0.0 wallet + other executables installers
**Note: Pool operators running pools not compatible with MWEB getblocktemplate should use [v1.3.0](https://github.com/koh-gt/ferrite-core/releases/tag/v1.3.0) Non-MWEB pools will always be able to mine for block rewards and process normal transactions, without processing MWEB transactions.**  
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt.exe) |  `c232d738d534494b8ed53bcf51a6a7a1e5cef719a8296d6141b1ba3fa614092d` |
| Windows             | 64     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-2.0.0.7z) | `f76f7da506ec1a8efefd8813923db462b7da07df38ea882f75e5e22faf5f139f` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt.7z) | `c05f926814ba24f06ba55eac5f3523c8776d6a993d6e400f86a46d3dfd0a3527` |
| Unix                     |  64     | Qt + CLI                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-2.0.0-unix.7z)  | `aeba5f25d7132cd514572733aa370f91fbee04bc43d56907d2da678a85bd0d51` |

### v1.3.1 wallet for 32-bit compatibility
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 32     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1-win32-setup.exe) | `fef28d405e7ec2c2061ec7771c7e72323a3de8c6f2709595fe76c4699e038477` |
| Windows             | 32     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt-x86.exe) | `f8055d287b89fa22987a0d886e0ac18953d6b7363ddfa10f2a9b6542acaa9ee2` |
| Windows             | 32     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1.7z) | `f60353297a737d906e8582fab7565b265aaafce99f4bbf238db597369f2e9af3` |


## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
| Miner                  | Type                     | Link                             | Checksum                                                                             |
|-------------------|---------------------|-------------------------|-----------------------------------------------------------------|
| CCMiner             | ASIC / GPU / CPU | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v1.3.0/ferrite-pool-miner-1.3.0.7z) | `6799b0d090e3007d5cebb8485744c0d07a68c5dd0f4df84c17198ba94dac2ffd` |

## Changes
Updated base source code from Litecoin 0.18 to Bitcoin 23.0 + Litecoin 0.21.2
Full multi wallet support
Multi-signature transaction support
Privacy mode

### Future changes
Newly added pools and future checkpoints (Block 100,000) will be added to addnodes on version 1.3.2 or when block 100,000 is reached, whichever is earlier.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.04 by @koh-gt
Uploaded on 15 Feb 2023.

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/ferrite-backup-1.3.0...ferrite-backup-2.0.0