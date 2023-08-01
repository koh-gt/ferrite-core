### This is a revision of Ferrite Core v2.1.1 based on v2.1.2r1 and v2.1.2r2 changes. 

Pruning option disabled by default in ferrite.conf rather than settings to avoid potential txindex error when both txindex and prune is enabled. This line `prune=1` can be removed from ferrite.conf to prevent the default configuration file from overriding Ferrite Core pruning option.

Default configuration file now has more built in addnodes and seednodes for faster block propagation and synchronisation.

**Forwarding your external WAN port 9574 at your router allows your node to broadcast block information to miners faster, reducing the risk of another miner finding a valid block at the same block height during the block propagation process.** 
> Forwarding port 9573 on your router can reduce propagation time by 500-2000 milliseconds, providing a 1-3% increase in mining output.

New 32 bit binaries will no longer be released, with the final release as v1.3.1. Current and future versions of Ferrite Core will maintain backwards compatibility with all versions of Ferrite Core.

### v2.1.2 wallets
| OS                        | Bit      | Name                   | Link                              | SHA256 Checksum                         |
|--------------------|-------|---------------------|--------------------------|---------------------------------------|
| Windows             | 64     | Setup                     | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.2/ferrite-2.1.2-win64-setup.exe) | `4fdc2486f1bf1770cc4a8570ac0e620c44d1128fd29d3a5d823d86532a6da7d0` |
| Windows             | 64     | Qt Wallet               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.2/ferrite-qt.exe) |  `d62683228a8de98272405e445327e22fb4952bd3b0c09e1cba4b8f9fa65c64fa` |
| Windows             | 64     | Qt + CLI               | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.2/ferrite-2.1.2.7z) | `d4c98da1268eacb5f02c3150d599d2dca5a0e2055ae369bf07ee293e3705b832` |
| Unix                     | 64     | Qt Wallet                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.2/ferrite-qt.7z) | `cdd6008fb89f7793725db766024d2459d1b015ecda6e1f81994fcbb0ebfae726` |
| Unix                     |  64     | Qt + CLI                | [Download](https://github.com/koh-gt/ferrite-core/releases/download/v2.1.2/ferrite-2.1.2-unix.7z)  | `99bf3a3cf348cfbba00d2f07eff5dfa73c0107e37ef794efd9356e48b345a4a1` |

### v1.3.1 wallet for 32-bit (old release)
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
Built in nodes.
Default configuration file settings.
Small changes.

### Future changes
Checkpoints for block 150,000.
New tab for wallet management (dumprivkey, importprivkey).

Included in this release is a stratum-pool miner based on [CCMiner v2.3.1 by @tpruvot](https://github.com/tpruvot/ccminer/releases/tag/2.3.1-tpruvot) with new pool configurations.
ASIC mining is recommended over GPU and CPU mining. You can opt to rent an Scrypt ASIC.

Everyone is invited to pull ferrite-core to suggest feature updates/chain parameters.

* Compiled on Ubuntu Linux 22.10 by @koh-gt
Uploaded on 2 Apr 2023.

## What's Changed
* v2.1.2 by @koh-gt in https://github.com/koh-gt/ferrite-core/pull/46

**Full Changelog**: https://github.com/koh-gt/ferrite-core/compare/v2.1.1...v2.1.2