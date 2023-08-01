## [**Download Ferrite Wallet**](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-qt.exe) - Ferrite Core Qt for Windows 64-bit
_[Windows 32-bit](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt-x86.exe) / [Linux and Mac OS 64-bit](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-qt.7z)_

---

This is a revision of Ferrite Core v2.0.0 based on v2.1.0r1 changes. 

MWEB will be currently optional since most miners mining Ferrite are currently not fully compatible with MWEB, which is a relatively new Litecoin feature. 
Before, getblocktemplate on v2.0.0 must be called with the MWEB rule 
`getblocktemplate '{"rules": ["mweb", "segwit"]}'`
Now, pools that are not compatible with MWEB can also validate blocks without MWEB transactions using 
`getblocktemplate '{"rules": ["segwit"]}'`
For the near future, MWEB will be activated no earlier than 2024 to allow time for mining pools to gradually update for MWEB support.

Since the current update is not supported on 32-bit builds, v1.3.1 is released for 32-bit systems incompatible with v2.1.0

### v2.1.0 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-2.1.0-win64-setup.exe) | `f34e68d23c1dc327c6507c91fd4093e7da7b86d17fe3601adff61f97531eb71b` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-qt.exe) |  `d4e059a15de06bc5cf22794e3f92f9ee6a2fa5fb5e4773a2cbbae896f7e9251c` |
| Windows             | 64     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-2.1.0.7z) | `55ecd625a1630b723c135dd397534a379e50af18b2eeef3d124d457051f8979b` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-qt.7z) | `7bec53cfa5a55d93ffa86aec517c3d0328b288b4f2b86c3fb3cfb378c5ae8813` |
| Unix                     |  64     | Qt + CLI                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.0/ferrite-2.1.0-unix.7z)  | `1da3cf1a22ac081cf37450dbeecce896c26c76676a8e735efc268c63abf13a11` |

### v1.3.1 wallet for 32-bit
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 32     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1-win32-setup.exe) | `fef28d405e7ec2c2061ec7771c7e72323a3de8c6f2709595fe76c4699e038477` |
| Windows             | 32     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-qt-x86.exe) | `f8055d287b89fa22987a0d886e0ac18953d6b7363ddfa10f2a9b6542acaa9ee2` |
| Windows             | 32     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.0.0/ferrite-1.3.1.7z) | `f60353297a737d906e8582fab7565b265aaafce99f4bbf238db597369f2e9af3` |


## Mining
### [List of pools and settings](https://github.com/koh-gt/ferrite-core/wiki/Mining-Pools-List)
| Miner                  | Type                     | Link                             | SHA256 Checksum                                                                |
|-------------------|---------------------|-------------------------|-----------------------------------------------------------------|
| CCMiner             | ASIC / GPU / CPU | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v1.3.0/ferrite-pool-miner-1.3.0.7z) | `6799b0d090e3007d5cebb8485744c0d07a68c5dd0f4df84c17198ba94dac2ffd` |

## Changes
Optional to mine with MWEB support.
Small changes.

### Future changes
Monospace font selector for v2
Newly added pools and future checkpoints (Block 100,000) will be added to addnodes on version 1.3.2 or when block 100,000 is reached, whichever is earlier.

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 22 Feb 2023.

## What's Changed
* Update README.md by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/41
* gbt fix 1 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/43
* ferrite -  remove mweb getblocktemplate requirement by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/42

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v2.0.0...v2.1.0